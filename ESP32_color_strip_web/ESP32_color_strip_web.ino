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
const int LED_PIN = 4; // Digital pin connected to the NeoPixel data input (GPIO4 is common)
#define NUM_LEDS            99  // Total number of pixels in your strip - NOW 99

#define LED_TYPE            WS2812B
#define COLOR_ORDER         GRB

CRGB leds[NUM_LEDS];

// --- NeoPixel Segment Definitions ---
const int NUM_SEGMENTS = 6;
// Ensure initial lengths sum up to NUM_LEDS or less.
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19}; // Adjusted to sum to 99
int segmentStartPixel[NUM_SEGMENTS];
int segmentEndPixel[NUM_SEGMENTS];

CRGB currentSegmentColors[NUM_SEGMENTS];

// --- Per-Segment Flashing Variables ---
bool segmentIsFlashing[NUM_SEGMENTS] = {false}; // Is this segment configured to flash?
unsigned long segmentFlashIntervalMillis[NUM_SEGMENTS] = {1000, 1000, 1000, 1000, 1000, 1000}; // Default 1 second
unsigned long segmentLastFlashTime[NUM_SEGMENTS] = {0}; // Last time this segment changed flash state
bool segmentCurrentFlashState[NUM_SEGMENTS] = {true}; // Current ON/OFF state for its blinking cycle (true=ON, false=OFF)

// Global variable to store current brightness (0-255)
uint8_t currentBrightness = 153;

// --- Physical Button Configuration ---
const int BUTTON_PIN = 13; // Digital pin for the button (GPIO13)
volatile bool buttonToggleRequested = false;
volatile unsigned long lastButtonPressMillis = 0;
const unsigned long debounceDelay = 200;

// --- Global State for Strip On/Off ---
volatile bool stripIsOn = false; // Overall power state of the strip (controlled by physical button)


// --- Helper function to recalculate segment pixel ranges ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        // Allow effective length to be 0 now
        int effectiveLength = max(0, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;
        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1); // Cap at actual NUM_LEDS

        if (currentPixel >= NUM_LEDS || effectiveLength == 0) { // If beyond strip or length is 0
            segmentStartPixel[i] = currentPixel; // Segment effectively empty
            segmentEndPixel[i] = currentPixel - 1;
        }

        currentPixel = segmentEndPixel[i] + 1;
    }
    Serial.println("Updated Segment Boundaries:");
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        Serial.printf("  Segment %d: Start=%d, End=%d, Actual Length=%d\n", i, segmentStartPixel[i], segmentEndPixel[i], max(0, segmentEndPixel[i] - segmentStartPixel[i] + 1));
    }
}

// --- Helper function to convert hex string to CRGB color ---
CRGB colorFromHexString(String hexString) {
    if (hexString.startsWith("#")) {
        hexString = hexString.substring(1);
    }
    if (hexString.length() != 6) {
        Serial.printf("Invalid hex string length: %s\n", hexString.c_str());
        return CRGB::Black;
    }
    unsigned long hexValue = strtol(hexString.c_str(), NULL, 16);
    uint8_t r = (hexValue >> 16) & 0xFF;
    uint8_t g = (hexValue >> 8) & 0xFF;
    uint8_t b = hexValue & 0xFF;
    return CRGB(r, g, b);
}

// --- Function to build the LED buffer based on current segment states (no FastLED.show()) ---
void updateLedBuffer() {
    // Fill the main 'leds' buffer based on current states
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
      if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
        for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
          if (i < NUM_LEDS) {
            if (segmentIsFlashing[seg] && !segmentCurrentFlashState[seg]) { // If flashing and currently in OFF state
              leds[i] = CRGB::Black;
            } else { // Static segment, or flashing segment in ON state
              leds[i] = currentSegmentColors[seg];
            }
          }
        }
      }
    }
    Serial.println("LED buffer updated (not yet shown).");
}


// --- Interrupt Service Routine (ISR) for the Button ---
void IRAM_ATTR handleButtonPress() {
    if (millis() - lastButtonPressMillis > debounceDelay) {
        buttonToggleRequested = true;
        lastButtonPressMillis = millis();
        Serial.println("Button pressed (ISR detected).");
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

  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black; // Initialize colors to black
      segmentLastFlashTime[i] = millis(); // Initialize flash timers
      segmentCurrentFlashState[i] = true; // Start all flashing segments in ON state
  }
  FastLED.clear();
  FastLED.show(); // Ensure strip is off physically at boot
  stripIsOn = false;

  // --- Configure button pin and interrupt ---
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  Serial.printf("Button configured on GPIO%d with INPUT_PULLUP and FALLING interrupt.\n", BUTTON_PIN);

  // --- WiFi Setup (Access Point) ---
  Serial.print("Setting up Access Point \"");
  Serial.print(ssid);
  Serial.println("\"...");
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println("SSID is now visible.");
  Serial.printf("Free Heap after WiFi setup: %lu bytes\n", ESP.getFreeHeap()); // Fixed format specifier


  // --- Initialize LittleFS ---
  Serial.println("Attempting to mount LittleFS...");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed or Formatted! Attempting to format and remount...");
    LittleFS.format();
    if(!LittleFS.begin()){
      Serial.println("LittleFS mount failed after format! Cannot continue without filesystem.");
      while(true);
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
    Serial.println("Client requested root URL '/'");
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
        Serial.println("  Sent /index.html");
    } else {
        request->send(404, "text/plain", "index.html not found on filesystem!");
        Serial.println("  ERROR: index.html not found!");
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
      // Immediately apply changes if strip is on, and stop any flashing
      if (stripIsOn) {
          updateLedBuffer(); // Update buffer with new brightness, all segments static
          FastLED.show();
      }
      // Brightness change implies static display, so turn off flashing logic for all segments
      for(int i=0; i<NUM_SEGMENTS; ++i) segmentIsFlashing[i] = false;

      Serial.printf("Brightness set to: %d%%\n", brightnessPct);
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });

  server.on("/setSegments", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setSegments command.");
    bool shouldRedraw = false; // Flag to indicate if LEDs need to be updated

    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        String hexColor = request->arg(paramColorName);
        CRGB newColor = colorFromHexString(hexColor);
        if (newColor != currentSegmentColors[i]) {
            currentSegmentColors[i] = newColor;
            shouldRedraw = true;
            Serial.printf("  Segment %d color set to: #%s\n", i + 1, hexColor.c_str());
        }
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        // Constrain length to be at least 0 and not exceed NUM_LEDS
        if (newLength < 0) newLength = 0; // Changed from 1 to 0
        if (newLength > NUM_LEDS) newLength = NUM_LEDS; // Important: cap at total LEDs

        if (newLength != segmentLengths[i]) {
            segmentLengths[i] = newLength;
            shouldRedraw = true;
            Serial.printf("  Segment %d length set to: %d\n", i + 1, newLength);
        }
      }

      // Handle flash state (f<idx>) and flash rate (fr<idx>) for each segment
      String paramFlashName = "f" + String(i);
      if (request->hasArg(paramFlashName)) {
          bool newIsFlashing = (request->arg(paramFlashName) == "true");
          if (newIsFlashing != segmentIsFlashing[i]) {
              segmentIsFlashing[i] = newIsFlashing;
              // Reset flash state and timer when changing flash mode
              segmentCurrentFlashState[i] = true; // Start ON
              segmentLastFlashTime[i] = millis();
              shouldRedraw = true;
              Serial.printf("  Segment %d flashing set to: %s\n", i + 1, newIsFlashing ? "true" : "false");
          }
      }

      String paramFlashRateName = "fr" + String(i);
      if (request->hasArg(paramFlashRateName)) {
          float newFlashRateSeconds = request->arg(paramFlashRateName).toFloat();
          // Ensure interval is within bounds
          if (newFlashRateSeconds < 0.25) newFlashRateSeconds = 0.25;
          if (newFlashRateSeconds > 2.0) newFlashRateSeconds = 2.0;
          unsigned long newFlashRateMillis = (unsigned long)(newFlashRateSeconds * 1000.0);
          if (newFlashRateMillis != segmentFlashIntervalMillis[i]) {
              segmentFlashIntervalMillis[i] = newFlashRateMillis;
              // Reset timer when rate changes to apply new rate immediately
              segmentLastFlashTime[i] = millis();
              segmentCurrentFlashState[i] = true; // Start ON
              shouldRedraw = true;
              Serial.printf("  Segment %d flash rate set to: %.2f seconds (%lu ms)\n", i + 1, newFlashRateSeconds, newFlashRateMillis);
          }
      }
    }

    if (shouldRedraw) {
        updateSegmentPixels(); // Recalculate segment bounds
        updateLedBuffer(); // Update buffer with new static or flash settings
        if(stripIsOn) FastLED.show(); // Show updated state if master is ON
    } else {
        Serial.println("No segment parameters changed.");
    }
    request->send(200, "text/plain", "Segment configurations updated!");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command");
    FastLED.clear(); // Clear the internal buffer
    FastLED.show(); // Push black to the strip
    stripIsOn = false; // Web OFF command sets physical state to OFF
    // Do NOT reset flashing states here, they persist when strip is off via web.
    Serial.println("Lights OFF (web command) and strip state set to OFF.");
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
      Serial.println("Strip turned OFF by physical button. Clearing strip.");
      FastLED.clear(); // Clear the internal buffer
      FastLED.show(); // Push black to the physical strip
      // Do NOT reset flashing mode here; the settings persist even when off.
    }
  }

  // --- Main LED Update Logic (only runs if strip is ON) ---
  if (stripIsOn) {
    unsigned long currentMillis = millis();

    // Check individual segments for flash state changes
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsFlashing[i]) {
        if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
          segmentLastFlashTime[i] = currentMillis;
          segmentCurrentFlashState[i] = !segmentCurrentFlashState[i]; // Toggle
          Serial.printf("Segment %d flash state toggled to %s.\n", i, segmentCurrentFlashState[i] ? "ON" : "OFF");
        }
      }
    }

    // Always update the buffer with the latest states and show if strip is ON
    updateLedBuffer();
    FastLED.show();

    // Add a small delay to yield to other tasks, but keep it tight for good animation fluidity
    delay(5); // Adjust this value if animations are choppy or WiFi responsiveness is poor
  } else {
    // If strip is OFF, ensure it stays off and yield
    FastLED.clear();
    FastLED.show();
    delay(10); // Longer yield when completely off
  }
}

// colorWipe function is present in original, but not used. Keeping for completeness.
void colorWipe(CRGB color, int wait) {
  for(int i=0; i<NUM_LEDS; i++) {
    leds[i] = color;
    FastLED.show();
    delay(wait);
  }
}
