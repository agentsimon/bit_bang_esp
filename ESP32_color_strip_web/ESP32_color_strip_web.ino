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
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19}; // Default lengths
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

// --- Global Strip State Variables ---
// Mode 0: OFF
// Mode 1: ON (Static/Segment Flashing)
// Mode 2: ON (Global ON/OFF Cycling)
int currentStripMode = 0; // Initial mode is OFF

// --- Global ON/OFF Cycle Variables ---
unsigned long globalOnDurationMillis = 5000;  // Default 5 seconds ON
unsigned long globalOffDurationMillis = 2000; // Default 2 seconds OFF
unsigned long globalCycleLastToggleTime = 0;  // Last time global cycle phase changed
bool globalCycleIsOnPhase = false;            // True if currently in ON phase of global cycle

// --- Helper function to recalculate segment pixel ranges ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        int effectiveLength = max(0, segmentLengths[i]); // Allow 0 length
        int rawEndPixel = currentPixel + effectiveLength - 1;

        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1);

        if (currentPixel >= NUM_LEDS || effectiveLength == 0) {
            segmentStartPixel[i] = currentPixel; // Logical start, but no pixels
            segmentEndPixel[i] = currentPixel - 1; // Mark as empty range
        }
        currentPixel = segmentEndPixel[i] + 1;
    }
    // Serial.println("Updated Segment Boundaries.");
}

// --- Helper function to convert hex string to CRGB color ---
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
void updateLedBuffer() {
    FastLED.clear(); // Clear the entire buffer before drawing segments
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
      if (segmentEndPixel[seg] >= segmentStartPixel[seg]) { // Only process if segment has active pixels
        for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
          if (i < NUM_LEDS) {
            if (segmentIsFlashing[seg] && !segmentCurrentFlashState[seg]) {
              leds[i] = CRGB::Black; // If flashing and currently in OFF state
            } else {
              leds[i] = currentSegmentColors[seg]; // Static or flashing ON state
            }
          }
        }
      }
    }
    // Serial.println("LED buffer updated.");
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
  Serial.printf("Free Heap at start: %lu bytes\n", ESP.getFreeHeap());

  // --- FastLED Initialization ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  Serial.printf("FastLED strip initialized on GPIO%d with %d LEDs.\n", LED_PIN, NUM_LEDS);

  FastLED.setBrightness(currentBrightness);
  updateSegmentPixels(); // Initial calculation of segment boundaries

  // Initialize segment colors and flash states
  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black;
      segmentLastFlashTime[i] = millis();
      segmentCurrentFlashState[i] = true;
      segmentFlashIntervalMillis[i] = 1000; // Default 1 second flash interval
  }
  FastLED.clear();
  FastLED.show(); // Ensure strip is physically off at boot

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
  Serial.printf("Free Heap after WiFi setup: %lu bytes\n", ESP.getFreeHeap());

  // --- Initialize LittleFS ---
  Serial.println("Attempting to mount LittleFS...");
  if (!LittleFS.begin(true)) { // 'true' attempts to format if mount fails
    Serial.println("LittleFS Mount Failed or Formatted! Attempting to format and remount...");
    LittleFS.format();
    if(!LittleFS.begin()){
      Serial.println("LittleFS mount failed after format! Cannot continue without filesystem.");
      while(true); // Halt on critical error
    }
    Serial.println("LittleFS formatted and remounted successfully.");
  } else {
    Serial.println("LittleFS mounted successfully.");
  }
  Serial.printf("Free Heap after LittleFS operations: %lu bytes\n", ESP.getFreeHeap());

  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
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
      
      // Setting brightness always implies stopping global cycle and per-segment flashing
      currentStripMode = 1; // Transition to static/segment flashing mode
      // Turn off per-segment flashing for all segments
      for(int i=0; i<NUM_SEGMENTS; ++i) segmentIsFlashing[i] = false;

      // Update LED buffer and show immediately
      updateLedBuffer();
      FastLED.show();
      Serial.printf("Brightness set to: %d%%\n", brightnessPct);
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });

  // New combined endpoint to receive all segment and global cycle configurations
  server.on("/setAllConfig", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setAllConfig command.");
    bool shouldRedraw = false;

    // Parse Segment Data
    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        CRGB newColor = colorFromHexString(request->arg(paramColorName).c_str());
        if (newColor != currentSegmentColors[i]) {
            currentSegmentColors[i] = newColor;
            shouldRedraw = true;
        }
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        if (newLength < 0) newLength = 0;
        if (newLength > NUM_LEDS) newLength = NUM_LEDS;
        if (newLength != segmentLengths[i]) {
            segmentLengths[i] = newLength;
            shouldRedraw = true;
        }
      }

      String paramFlashName = "f" + String(i);
      if (request->hasArg(paramFlashName)) {
          bool newIsFlashing = (request->arg(paramFlashName) == "true");
          if (newIsFlashing != segmentIsFlashing[i]) {
              segmentIsFlashing[i] = newIsFlashing;
              // Reset flash state and timer when changing flash mode
              segmentCurrentFlashState[i] = true; // Start ON
              segmentLastFlashTime[i] = millis();
              shouldRedraw = true;
          }
      }

      String paramFlashRateName = "fr" + String(i);
      if (request->hasArg(paramFlashRateName)) {
          float newFlashRateSeconds = request->arg(paramFlashRateName).toFloat();
          if (newFlashRateSeconds < 0.25) newFlashRateSeconds = 0.25;
          if (newFlashRateSeconds > 2.0) newFlashRateSeconds = 2.0;
          unsigned long newFlashRateMillis = (unsigned long)(newFlashRateSeconds * 1000.0);
          if (newFlashRateMillis != segmentFlashIntervalMillis[i]) {
              segmentFlashIntervalMillis[i] = newFlashRateMillis;
              // Reset timer when rate changes to apply new rate immediately
              segmentLastFlashTime[i] = millis();
              segmentCurrentFlashState[i] = true; // Start ON
              shouldRedraw = true;
          }
      }
    }

    // Parse Global ON/OFF Durations
    if (request->hasArg("gon")) {
      globalOnDurationMillis = (unsigned long)(request->arg("gon").toFloat() * 1000.0);
      Serial.printf("Global ON duration set to: %.1f s (%lu ms)\n", request->arg("gon").toFloat(), globalOnDurationMillis);
    }
    if (request->hasArg("goff")) {
      globalOffDurationMillis = (unsigned long)(request->arg("goff").toFloat() * 1000.0);
      Serial.printf("Global OFF duration set to: %.1f s (%lu ms)\n", request->arg("goff").toFloat(), globalOffDurationMillis);
    }
    
    // Always recalculate segment boundaries if anything might have changed
    updateSegmentPixels(); 
    
    // Only update FastLED display if currently in a mode that displays colors
    // We don't change the currentStripMode here, it's controlled by physical button.
    if (currentStripMode == 1 || currentStripMode == 2) {
      updateLedBuffer();
      FastLED.show();
    }

    request->send(200, "text/plain", "All configurations updated!");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command");
    FastLED.clear();
    FastLED.show();
    currentStripMode = 0; // Explicitly set to OFF mode
    Serial.println("Lights OFF (web command).");
    request->send(200, "text/plain", "Lights OFF");
  });

  server.begin();
  Serial.println("HTTP server started.");
  Serial.printf("Ready. Connect to WiFi SSID: '%s' (Password: '%s') then open a browser to http://%s\n", ssid, password, WiFi.softAPIP().toString().c_str());
  Serial.printf("Free Heap at end of setup: %lu bytes\n", ESP.getFreeHeap());
}

void loop() {
  unsigned long currentMillis = millis(); // Get current time once per loop

  // --- Handle physical button press to toggle strip modes ---
  if (buttonToggleRequested) {
    buttonToggleRequested = false;
    currentStripMode = (currentStripMode + 1) % 3; // Cycle through 0, 1, 2

    Serial.printf("Button pressed. New mode: %d\n", currentStripMode);

    if (currentStripMode == 0) { // Transition to OFF
      FastLED.clear();
      FastLED.show();
      Serial.println("Transitioned to: OFF.");
    } else if (currentStripMode == 1) { // Transition to ON (Static/Segment Flashing)
      // Reset per-segment flash timers to ensure they start in ON state
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis; // Reset individual timers
      }
      updateLedBuffer(); // Populate buffer according to segment settings
      FastLED.show();
      Serial.println("Transitioned to: ON (Static/Segment Flashing).");
    } else if (currentStripMode == 2) { // Transition to ON (Global ON/OFF Cycle)
      globalCycleIsOnPhase = true; // Start global cycle in ON phase
      globalCycleLastToggleTime = currentMillis; // Reset global cycle timer
      // Reset per-segment flash timers to ensure they start in ON state for the global ON phase
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis; // Reset individual timers
      }
      updateLedBuffer(); // Populate buffer
      FastLED.show();
      Serial.println("Transitioned to: ON (Global ON/OFF Cycling).");
    }
  }

  // --- Main LED Update Logic based on current mode ---
  if (currentStripMode == 1) { // Mode 1: Static/Segment Flashing
    // Only check individual segments for flash state changes
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsFlashing[i]) {
        if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
          segmentLastFlashTime[i] = currentMillis;
          segmentCurrentFlashState[i] = !segmentCurrentFlashState[i]; // Toggle flash state
        }
      }
    }
    updateLedBuffer(); // Update buffer with latest segment states
    FastLED.show();
    delay(5); // Small delay to yield
  }
  else if (currentStripMode == 2) { // Mode 2: Global ON/OFF Cycling
    if (globalCycleIsOnPhase) { // Currently in ON phase
      if (currentMillis - globalCycleLastToggleTime >= globalOnDurationMillis) {
        // Time to switch to OFF phase
        globalCycleIsOnPhase = false;
        globalCycleLastToggleTime = currentMillis;
        FastLED.clear(); // Turn off physical strip
        FastLED.show();
        Serial.println("Global Cycle: Switched to OFF phase.");
      } else {
        // Still in ON phase, update segment flashes
        for (int i = 0; i < NUM_SEGMENTS; ++i) {
          if (segmentIsFlashing[i]) {
            if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
              segmentLastFlashTime[i] = currentMillis;
              segmentCurrentFlashState[i] = !segmentCurrentFlashState[i]; // Toggle individual flash state
            }
          }
        }
        updateLedBuffer(); // Update buffer based on individual segment flash states
        FastLED.show();
      }
    } else { // Currently in OFF phase
      if (currentMillis - globalCycleLastToggleTime >= globalOffDurationMillis) {
        // Time to switch to ON phase
        globalCycleIsOnPhase = true;
        globalCycleLastToggleTime = currentMillis;
        // Reset per-segment flash timers to ensure they start ON for the new global ON phase
        for(int i = 0; i < NUM_SEGMENTS; ++i) {
            segmentCurrentFlashState[i] = true;
            segmentLastFlashTime[i] = currentMillis; // Reset individual timers
        }
        updateLedBuffer(); // Turn on physical strip with current segment config
        FastLED.show();
        Serial.println("Global Cycle: Switched to ON phase.");
      }
      // If still in OFF phase, nothing to do (strip is already clear)
    }
    delay(5); // Small delay to yield
  }
  else { // Mode 0: OFF
    delay(10); // Longer yield when completely off
  }
}
