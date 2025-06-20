#include <Arduino.h>
#include <WiFi.h>            // ESP32 WiFi library
#include <AsyncTCP.h>        // ESP32 Async TCP (for AsyncWebServer)
#include <ESPAsyncWebServer.h> // ESPAsyncWebServer for ESP32
#include <FastLED.h>         // FastLED library
#include <LittleFS.h>        // LittleFS for file system operations (also supported on ESP32)

// --- WiFi Configuration (Access Point Mode) ---
const char* ssid = "NeoPixel_Controller"; // The name of the Wi-Fi network the ESP32 will create
const char* password = "123456789";        // The password for your Wi-Fi network (min 8 characters)

// --- Web Server Object ---
AsyncWebServer server(80); // Create an AsyncWebServer object on port 80

// --- NeoPixel Configuration for FastLED ---
const int LED_PIN = 4;        // Digital pin connected to the NeoPixel data input (GPIO4 is common)
const int NUM_LEDS = 99;      // Total number of pixels in your strip
const int NUM_SEGMENTS = 6;   // Number of segments

#define LED_TYPE            WS2812B
#define COLOR_ORDER         GRB

CRGB leds[NUM_LEDS]; // FastLED LED buffer

// --- NeoPixel Segment Definitions ---
// Initial lengths (client-side defaults). Actual lengths constrained by NUM_LEDS.
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19};
int segmentStartPixel[NUM_SEGMENTS]; // Start index for each segment
int segmentEndPixel[NUM_SEGMENTS];   // End index for each segment

CRGB currentSegmentColors[NUM_SEGMENTS]; // Stores the color for each segment

// --- Per-Segment Flashing Variables ---
bool segmentIsFlashing[NUM_SEGMENTS] = {false};             // Is this segment configured to flash?
unsigned long segmentFlashIntervalMillis[NUM_SEGMENTS];     // Flash interval for each segment
unsigned long segmentLastFlashTime[NUM_SEGMENTS];           // Last time this segment changed flash state
bool segmentCurrentFlashState[NUM_SEGMENTS];                // Current ON/OFF state for its blinking cycle (true=ON, false=OFF)

// Global variable for overall brightness
uint8_t currentBrightness = 153; // Default brightness (60% of 255)

// --- Physical Button Configuration ---
const int BUTTON_PIN = 13; // Digital pin for the button (GPIO13)
volatile bool buttonToggleRequested = false; // Flag set by ISR to request strip toggle
volatile unsigned long lastButtonPressMillis = 0; // Last time button was debounced
const unsigned long debounceDelay = 200; // Debounce time in milliseconds

// --- Global State for Strip On/Off ---
volatile bool stripIsOn = false; // Overall power state of the strip (controlled by physical button)


// --- Helper function to recalculate segment pixel ranges ---
// This function updates the start and end pixel indices for each segment
// ensuring they respect the overall NUM_LEDS limit and segment lengths.
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        // Ensure effective length is non-negative
        int effectiveLength = max(0, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;

        // Cap the end pixel at the total number of LEDs
        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1);

        // If the segment's start pixel is beyond the total LEDs or its effective length is 0,
        // mark it as an empty segment.
        if (currentPixel >= NUM_LEDS || effectiveLength == 0) {
            segmentStartPixel[i] = currentPixel; // Logical start, but no pixels
            segmentEndPixel[i] = currentPixel - 1; // Mark as empty range
        }
        currentPixel = segmentEndPixel[i] + 1; // Move to the start of the next segment
    }
    // Serial.println("Updated Segment Boundaries."); // Reduced debug output
}

// --- Helper function to convert hex string to CRGB color ---
// Converts a 6-character hex string (e.g., "FF00FF") to a FastLED CRGB color.
CRGB colorFromHexString(const char* hexString) {
    if (strlen(hexString) != 6) {
        Serial.printf("Invalid hex string length: %s\n", hexString);
        return CRGB::Black;
    }
    unsigned long hexValue = strtol(hexString, NULL, 16);
    uint8_t r = (hexValue >> 16) & 0xFF;
    uint8_t g = (hexValue >> 8) & 0xFF;
    uint8_t b = hexValue & 0xFF;
    return CRGB(r, g, b);
}

// --- Function to build the LED buffer based on current segment states ---
// This function populates the 'leds' array (FastLED's internal buffer)
// based on segment colors and flash states. It does NOT call FastLED.show().
void updateLedBuffer() {
    FastLED.clear(); // Start by clearing the entire buffer
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
      // Only process segments that actually have pixels assigned
      if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
        for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
          // Ensure pixel index is within bounds of the physical strip
          if (i < NUM_LEDS) {
            // If segment is flashing and currently in its 'OFF' state, set pixel to black
            if (segmentIsFlashing[seg] && !segmentCurrentFlashState[seg]) {
              leds[i] = CRGB::Black;
            } else { // Otherwise, set to the segment's defined color
              leds[i] = currentSegmentColors[seg];
            }
          }
        }
      }
    }
    // Serial.println("LED buffer updated."); // Reduced debug output
}


// --- Interrupt Service Routine (ISR) for the Button ---
// This function is called automatically when the button pin goes LOW (pressed).
// It sets a flag to be processed in the main loop to avoid complex logic in ISR.
void IRAM_ATTR handleButtonPress() {
    // Basic debouncing to prevent multiple triggers from a single press
    if (millis() - lastButtonPressMillis > debounceDelay) {
        buttonToggleRequested = true;
        lastButtonPressMillis = millis();
        Serial.println("Button pressed (ISR detected)."); // Keep for critical feedback
    }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- ESP32 NeoPixel Controller Boot ---");
  Serial.printf("Free Heap at start: %lu bytes\n", ESP.getFreeHeap()); // Fixed format specifier

  // --- FastLED Initialization ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  Serial.printf("FastLED strip initialized on GPIO%d with %d LEDs.\n", LED_PIN, NUM_LEDS);

  FastLED.setBrightness(currentBrightness);
  updateSegmentPixels(); // Initial calculation of segment boundaries

  // Initialize segment colors and flash states
  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black; // All segments start black
      segmentLastFlashTime[i] = millis();    // Initialize flash timers for each segment
      segmentCurrentFlashState[i] = true;    // All flashing segments start in ON state
      segmentFlashIntervalMillis[i] = 1000;  // Default flash interval
  }
  FastLED.clear();
  FastLED.show(); // Ensure strip is physically off at boot
  stripIsOn = false; // Initial state: strip is off

  // --- Configure button pin and interrupt ---
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up resistor
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING); // Trigger on button press
  Serial.printf("Button configured on GPIO%d with INPUT_PULLUP and FALLING interrupt.\n", BUTTON_PIN);

  // --- WiFi Setup (Access Point) ---
  Serial.print("Setting up Access Point \"");
  Serial.print(ssid);
  Serial.println("\"...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP); // Keep for initial setup feedback
  Serial.println("SSID is now visible.");
  Serial.printf("Free Heap after WiFi setup: %lu bytes\n", ESP.getFreeHeap()); // Fixed format specifier


  // --- Initialize LittleFS ---
  Serial.println("Attempting to mount LittleFS...");
  // The 'true' parameter formats the filesystem if it fails to mount.
  // This is good for first-time use or corrupted filesystems, but will erase all data.
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed or Formatted! Attempting to format and remount...");
    LittleFS.format(); // This will erase all files!
    if(!LittleFS.begin()){
      Serial.println("LittleFS mount failed after format! Cannot continue without filesystem.");
      while(true); // Halt if filesystem still can't be mounted
    }
    Serial.println("LittleFS formatted and remounted successfully.");
  } else {
    Serial.println("LittleFS mounted successfully.");
  }

  // --- List LittleFS files (for debugging) ---
  Serial.println("Listing files in LittleFS:");
  File root = LittleFS.open("/");
  if(root){
    File file = root.openNextFile();
    while(file){
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
      file = root.openNextFile();
    }
  } else {
    Serial.println("  Failed to open LittleFS root directory.");
  }
  Serial.printf("Free Heap after LittleFS operations: %lu bytes\n", ESP.getFreeHeap()); // Fixed format specifier


  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    // Serial.println("Client requested root URL '/'"); // Removed frequent print
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
        // Serial.println("  Sent /index.html"); // Removed frequent print
    } else {
        request->send(404, "text/plain", "index.html not found on filesystem!");
        Serial.println("  ERROR: index.html not found!"); // Keep critical error print
    }
  });

  server.on("/ssid", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", ssid);
  });

  server.on("/setBrightness", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasArg("value")) {
      int brightnessPct = request->arg("value").toInt();
      currentBrightness = map(brightnessPct, 0, 100, 0, 255);
      FastLED.setBrightness(currentBrightness);
      
      // Setting brightness implies stopping any active flashing on segments
      for(int i=0; i<NUM_SEGMENTS; ++i) segmentIsFlashing[i] = false;

      // Update LED buffer and show immediately if strip is on
      if (stripIsOn) {
          updateLedBuffer();
          FastLED.show();
      }
      Serial.printf("Brightness set to: %d%%\n", brightnessPct);
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });

  server.on("/setSegments", HTTP_GET, [](AsyncWebServerRequest *request){
    // Serial.println("Received /setSegments command."); // Removed frequent print
    bool shouldRedraw = false; // Flag to indicate if LEDs need to be updated

    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        // Use c_str() to pass a const char* to avoid String object creation in helper
        CRGB newColor = colorFromHexString(request->arg(paramColorName).c_str());
        if (newColor != currentSegmentColors[i]) {
            currentSegmentColors[i] = newColor;
            shouldRedraw = true;
            // Serial.printf("  Segment %d color set.\n", i + 1); // Reduced print
        }
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        // Constrain length to be at least 0 and not exceed NUM_LEDS
        if (newLength < 0) newLength = 0;
        if (newLength > NUM_LEDS) newLength = NUM_LEDS;

        if (newLength != segmentLengths[i]) {
            segmentLengths[i] = newLength;
            shouldRedraw = true;
            // Serial.printf("  Segment %d length set.\n", i + 1); // Reduced print
        }
      }

      // Handle flash state (f<idx>)
      String paramFlashName = "f" + String(i);
      if (request->hasArg(paramFlashName)) {
          bool newIsFlashing = (request->arg(paramFlashName) == "true");
          if (newIsFlashing != segmentIsFlashing[i]) {
              segmentIsFlashing[i] = newIsFlashing;
              // Reset flash state and timer when changing flash mode
              segmentCurrentFlashState[i] = true; // Start ON
              segmentLastFlashTime[i] = millis();
              shouldRedraw = true;
              // Serial.printf("  Segment %d flashing set.\n", i + 1); // Reduced print
          }
      }

      // Handle flash rate (fr<idx>)
      String paramFlashRateName = "fr" + String(i);
      if (request->hasArg(paramFlashRateName)) {
          float newFlashRateSeconds = request->arg(paramFlashRateName).toFloat();
          // Ensure interval is within bounds (0.25 to 2 seconds)
          if (newFlashRateSeconds < 0.25) newFlashRateSeconds = 0.25;
          if (newFlashRateSeconds > 2.0) newFlashRateSeconds = 2.0;
          unsigned long newFlashRateMillis = (unsigned long)(newFlashRateSeconds * 1000.0);
          if (newFlashRateMillis != segmentFlashIntervalMillis[i]) {
              segmentFlashIntervalMillis[i] = newFlashRateMillis;
              // Reset timer when rate changes to apply new rate immediately
              segmentLastFlashTime[i] = millis();
              segmentCurrentFlashState[i] = true; // Start ON
              shouldRedraw = true;
              // Serial.printf("  Segment %d flash rate set.\n", i + 1); // Reduced print
          }
      }
    }

    if (shouldRedraw) {
        updateSegmentPixels(); // Recalculate segment bounds
        updateLedBuffer(); // Update buffer with new static or flash settings
        if(stripIsOn) FastLED.show(); // Show updated state if master is ON
    } else {
        // Serial.println("No segment parameters changed."); // Removed frequent print
    }
    request->send(200, "text/plain", "Segment configurations updated!");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command"); // Keep for explicit action
    FastLED.clear(); // Clear the internal buffer
    FastLED.show(); // Push black to the strip
    stripIsOn = false; // Web OFF command sets physical state to OFF
    // Flashing states persist; only the physical display is off.
    Serial.println("Lights OFF (web command).");
    request->send(200, "text/plain", "Lights OFF");
  });

  server.begin();
  Serial.println("HTTP server started.");
  Serial.printf("Ready. Connect to WiFi SSID: '%s' (Password: '%s') then open a browser to http://%s\n", ssid, password, WiFi.softAPIP().toString().c_str());
  Serial.printf("Free Heap at end of setup: %lu bytes\n", ESP.getFreeHeap()); // Fixed format specifier
}

void loop() {
  // Handle physical button press to toggle strip on/off
  if (buttonToggleRequested) {
    buttonToggleRequested = false;
    stripIsOn = !stripIsOn; // Toggle the overall strip state

    if (stripIsOn) {
      Serial.println("Strip turned ON by physical button.");
      // When turned ON, reset timers for all flashing segments to start them ON
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true; // Ensure flashing segments start ON
          segmentLastFlashTime[i] = millis(); // Reset their individual timers
      }
      updateLedBuffer(); // Populate buffer according to current states
      FastLED.show(); // Show it
    } else {
      Serial.println("Strip turned OFF by physical button.");
      FastLED.clear(); // Clear the internal buffer
      FastLED.show(); // Push black to the physical strip
    }
  }

  // --- Main LED Update Logic (only runs if strip is ON) ---
  if (stripIsOn) {
    unsigned long currentMillis = millis();
    bool anySegmentFlashStateChanged = false; // Flag to track if any segment's flash state changed

    // Check individual segments for flash state changes
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsFlashing[i]) {
        if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
          segmentLastFlashTime[i] = currentMillis;
          segmentCurrentFlashState[i] = !segmentCurrentFlashState[i]; // Toggle flash state
          anySegmentFlashStateChanged = true; // Mark that a toggle happened
          // Serial.printf("Segment %d flash state toggled to %s.\n", i, segmentCurrentFlashState[i] ? "ON" : "OFF"); // Debug for specific segment toggle
        }
      }
    }

    // Only redraw the LEDs if a flash state changed or if the strip was just turned on
    // (the latter is handled by the buttonToggleRequested block).
    // For per-segment flashing, it's generally best to always update the buffer and show
    // if the strip is on, as state changes are frequent.
    updateLedBuffer();
    FastLED.show();

    // Add a small delay to yield to other tasks, but keep it tight for good animation fluidity
    delay(5);
  } else {
    // If strip is OFF, ensure it stays off and yield
    // These are already off, so just yielding is main purpose
    delay(10);
  }
}

// Removed colorWipe function as it was unused.
