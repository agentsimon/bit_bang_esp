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
#define NUM_LEDS            99  // Total number of pixels in your strip

#define LED_TYPE            WS2812B
#define COLOR_ORDER         GRB

CRGB leds[NUM_LEDS];

// --- NeoPixel Segment Definitions ---
const int NUM_SEGMENTS = 6;
// Ensure initial lengths sum up to NUM_LEDS or less, or adjust NUM_LEDS if needed.
// These are client-side defaults, actual strip is NUM_LEDS.
int segmentLengths[NUM_SEGMENTS] = {8, 8, 8, 8, 9, 9}; // Adjusted to sum to 50 for NUM_LEDS=50
int segmentStartPixel[NUM_SEGMENTS];
int segmentEndPixel[NUM_SEGMENTS];

CRGB currentSegmentColors[NUM_SEGMENTS];

uint8_t currentBrightness = 153;

// --- Blinking Feature Variables ---
float blinkIntervalSeconds = 1.0;
unsigned long blinkIntervalMillis = 1000;
bool isBlinkingActive = false; // This flag controls if the loop's blinking logic runs
unsigned long lastBlinkTime = 0;
bool currentBlinkLedState = false; // true = ON, false = OFF for blinking cycle

// --- Physical Button Configuration ---
const int BUTTON_PIN = 13; // Digital pin for the button (GPIO13)
volatile bool buttonToggleRequested = false;
volatile unsigned long lastButtonPressMillis = 0;
const unsigned long debounceDelay = 200;

// --- Global State for Strip On/Off ---
volatile bool stripIsOn = false; // Overall power state of the strip (controlled by button)


// --- Helper function to recalculate segment pixel ranges ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        int effectiveLength = max(1, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;
        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1); // Cap at actual NUM_LEDS

        if (currentPixel >= NUM_LEDS) {
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

// --- Function to apply current segment colors to the strip's buffer ---
void applySegmentColors() {
    FastLED.clear();
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
        if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
            for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
                if (i < NUM_LEDS) {
                    leds[i] = currentSegmentColors[seg];
                }
            }
        }
    }
    if (stripIsOn) { // Only show if the strip is meant to be on
        FastLED.show();
        Serial.println("NeoPixel segments updated and shown.");
    } else {
        Serial.println("NeoPixel segments updated (internal buffer), but strip is OFF.");
    }
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
  Serial.printf("Free Heap at start: %d bytes\n", ESP.getFreeHeap());

  // --- FastLED Initialization ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  Serial.printf("FastLED strip initialized on GPIO%d with %d LEDs.\n", LED_PIN, NUM_LEDS);

  FastLED.setBrightness(currentBrightness);
  updateSegmentPixels(); // Initial calculation of segment boundaries

  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black; // Initialize colors to black
  }
  FastLED.clear();
  FastLED.show(); // Ensure strip is off physically at boot
  stripIsOn = false;
  isBlinkingActive = false;

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
  Serial.printf("Free Heap after WiFi setup: %d bytes\n", ESP.getFreeHeap());


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
  Serial.printf("Free Heap after LittleFS operations: %d bytes\n", ESP.getFreeHeap());


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
      if (stripIsOn) { FastLED.show(); } // Update visual if strip is on
      isBlinkingActive = false; // Setting brightness stops blinking mode
      Serial.printf("Brightness set to: %d%%\n", brightnessPct);
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });

  server.on("/setSegments", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setSegments command.");
    bool anyColorChanged = false;
    bool anyLengthChanged = false;

    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        String hexColor = request->arg(paramColorName);
        CRGB newColor = colorFromHexString(hexColor);
        if (newColor != currentSegmentColors[i]) {
            currentSegmentColors[i] = newColor;
            anyColorChanged = true;
            Serial.printf("  Segment %d color set to: #%s\n", i + 1, hexColor.c_str());
        }
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        // Constrain length to be at least 1 and not exceed NUM_LEDS
        if (newLength < 1) newLength = 1;
        if (newLength > NUM_LEDS) newLength = NUM_LEDS; // Important: cap at total LEDs

        if (newLength != segmentLengths[i]) {
            segmentLengths[i] = newLength;
            anyLengthChanged = true;
            Serial.printf("  Segment %d length set to: %d\n", i + 1, newLength);
        }
      }
    }

    if (anyLengthChanged) { updateSegmentPixels(); }

    if (anyColorChanged || anyLengthChanged) {
        applySegmentColors(); // This will show if stripIsOn
        isBlinkingActive = false; // Applying new segments stops blinking mode
    } else {
        Serial.println("No segment colors or lengths changed.");
    }
    request->send(200, "text/plain", "Segment colors and lengths updated!");
  });

  server.on("/setAllColor", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setAllColor command.");
    if (request->hasArg("color")) {
      String hexColor = request->arg("color");
      CRGB newColor = colorFromHexString(hexColor);

      for(int i=0; i<NUM_LEDS; i++) { leds[i] = newColor; }
      if (stripIsOn) { FastLED.show(); } // Update visual if strip is on
      isBlinkingActive = false; // Setting all color stops blinking mode
      Serial.printf("All LEDs color updated to: #%s\n", hexColor.c_str());
      request->send(200, "text/plain", "All LEDs color updated (use button to turn on)!");
    } else {
      request->send(400, "text/plain", "Color value missing");
    }
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command");
    FastLED.clear(); // Clear the internal buffer
    FastLED.show(); // Push black to the strip
    isBlinkingActive = false; // Web OFF command stops blinking mode
    stripIsOn = false; // Web OFF command sets physical state to OFF
    Serial.println("Lights OFF (web command) and strip state set to OFF.");
    request->send(200, "text/plain", "Lights OFF");
  });

  server.on("/setBlinkInterval", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setBlinkInterval command.");
    if (request->hasArg("value")) {
      String receivedValueStr = request->arg("value");
      blinkIntervalSeconds = receivedValueStr.toFloat();

      // Ensure interval is within bounds (0.25 to 2 seconds)
      if (blinkIntervalSeconds < 0.25) blinkIntervalSeconds = 0.25;
      if (blinkIntervalSeconds > 2.0) blinkIntervalSeconds = 2.0;

      blinkIntervalMillis = (unsigned long)(blinkIntervalSeconds * 1000.0);
      isBlinkingActive = true; // Setting blink interval *activates* blinking mode
      currentBlinkLedState = true; // Ensure it starts ON
      lastBlinkTime = millis(); // Reset timer to start blinking immediately

      Serial.printf("Blink interval set to: %.2f seconds (converted to %lu ms). Blinking activated.\n", blinkIntervalSeconds, blinkIntervalMillis);
      // If the strip is currently on, apply colors to start the blink cycle visually
      if(stripIsOn) {
          applySegmentColors(); // Shows the 'ON' state of the blink
      }
      request->send(200, "text/plain", "Blink interval set and blinking activated!");
    } else {
      request->send(400, "text/plain", "Blink interval value missing");
    }
  });

  server.begin();
  Serial.println("HTTP server started.");
  Serial.printf("Ready. Connect to WiFi SSID: '%s' (Password: '%s') then open a browser to http://%s\n", ssid, password, WiFi.softAPIP().toString().c_str());
  Serial.printf("Free Heap at end of setup: %d bytes\n", ESP.getFreeHeap());
}

void loop() {
  if (buttonToggleRequested) {
    buttonToggleRequested = false;
    stripIsOn = !stripIsOn; // Toggle the overall strip state

    if (stripIsOn) {
      Serial.println("Strip turned ON by physical button.");
      if (isBlinkingActive) {
        currentBlinkLedState = true; // Ensure blinking starts with LEDs ON
        lastBlinkTime = millis(); // Reset blink timer
        applySegmentColors(); // Apply the ON state of the blink
      } else {
        // If not blinking, show the last static pattern.
        // applySegmentColors() will ensure the buffer has the correct colors
        // and then call FastLED.show() because stripIsOn is true.
        applySegmentColors();
      }
    } else {
      Serial.println("Strip turned OFF by physical button. Clearing strip.");
      FastLED.clear(); // Clear the internal buffer
      FastLED.show(); // Push black to the physical strip
      // isBlinkingActive is NOT set to false here by button.
      // This means the blinking *mode* persists when turned off by button.
    }
  }

  // --- Blinking Logic (only active if isBlinkingActive AND stripIsOn) ---
  // This is the core blinking mechanism.
  if (isBlinkingActive && stripIsOn) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastBlinkTime >= blinkIntervalMillis) {
      lastBlinkTime = currentMillis;
      currentBlinkLedState = !currentBlinkLedState; // Toggle LED state for blinking

      if (currentBlinkLedState) {
        applySegmentColors(); // Apply set colors (if stripIsOn is true, it will show)
        Serial.println("LEDs ON (Blink with segment colors)");
      } else {
        FastLED.clear(); // Set internal buffer to black
        FastLED.show(); // Push black to the strip
        Serial.println("LEDs OFF (Blink)");
      }
    }
  }
  // --- Else blocks for when strip is OFF OR blinking is not active ---
  else if (!stripIsOn) { // If the master switch is OFF, always ensure it's black and yield.
    FastLED.clear(); // Ensure the internal buffer is black
    FastLED.show(); // Push black to the strip
    delay(10); // Small delay to yield processing time
  }
  // If stripIsOn is true but isBlinkingActive is false, the static pattern is already shown
  // by applySegmentColors() when stripIsOn became true. Nothing more needed here.
}

// colorWipe function is present in original, but not used. Keeping for completeness.
void colorWipe(CRGB color, int wait) {
  for(int i=0; i<NUM_LEDS; i++) {
    leds[i] = color;
    FastLED.show();
    delay(wait);
  }
}
