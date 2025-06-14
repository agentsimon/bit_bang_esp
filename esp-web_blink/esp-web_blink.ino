#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h> // Include the FastLED library
#include <LittleFS.h> // Include the LittleFS library for file system operations

// --- WiFi Configuration (Access Point Mode) ---
const char* ssid = "NeoPixel_Controller"; // The name of the Wi-Fi network the NodeMCU will create
const char* password = "123456789";        // The password for your Wi-Fi network (min 8 characters)

// --- Web Server Object ---
AsyncWebServer server(80); // Create an AsyncWebServer object on port 80 (standard HTTP)

// --- NeoPixel Configuration for FastLED ---
#define LED_PIN             D2  // Digital pin connected to the NeoPixel data input (D2 on NodeMCU)
#define NUM_LEDS            99  // Total number of pixels in your strip

// Define the LED type and color order (most common for WS2812B is GRB)
#define LED_TYPE            WS2812B
#define COLOR_ORDER         GRB

// Create a FastLED CRGB array to hold the LED color data
CRGB leds[NUM_LEDS]; // This replaces the Adafruit_NeoPixel object

// --- NeoPixel Segment Definitions ---
const int NUM_SEGMENTS = 6;
// These arrays will now be dynamically updated
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19}; // Initial lengths, summing to 99
int segmentStartPixel[NUM_SEGMENTS];
int segmentEndPixel[NUM_SEGMENTS];    

// Store current colors for each segment (initialized to black/off)
CRGB currentSegmentColors[NUM_SEGMENTS]; // Changed type to CRGB

// Global variable to store current brightness (0-255)
uint8_t currentBrightness = 153; // Initial brightness 60% (153 out of 255)

// --- Blinking Feature Variables ---
float blinkIntervalSeconds = 1.0;     // Default interval: 1 second
unsigned long blinkIntervalMillis = 1000; // Converted to milliseconds
bool isBlinkingActive = false;
unsigned long lastBlinkTime = 0;
bool currentBlinkLedState = false; // true = ON, false = false = OFF


// --- Helper function to recalculate segment pixel ranges based on lengths ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        
        // Calculate the end pixel based on the desired length
        // Ensure that segmentLength is at least 1 to avoid negative lengths or zero
        int effectiveLength = max(1, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;

        // Cap the end pixel at the total number of LEDs minus one
        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1);

        // If a segment starts beyond the total number of LEDs, it's an empty segment
        if (currentPixel >= NUM_LEDS) {
            segmentStartPixel[i] = currentPixel; // Still set start, but effectively empty
            segmentEndPixel[i] = currentPixel - 1; // Mark as empty (end < start)
        }
        
        // Update currentPixel for the start of the next segment
        currentPixel = segmentEndPixel[i] + 1;
    }

    // Optional: Log the new segment boundaries for debugging
    Serial.println("Updated Segment Boundaries:");
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        Serial.print("  Segment "); Serial.print(i);
        Serial.print(": Start="); Serial.print(segmentStartPixel[i]);
        Serial.print(", End="); Serial.print(segmentEndPixel[i]);
        // Calculate actual length for logging (can be 0 if segment is empty)
        Serial.print(", Actual Length="); Serial.println(max(0, segmentEndPixel[i] - segmentStartPixel[i] + 1));
    }
}

// --- Helper function to convert hex string (e.g., "#FF00FF") to FastLED CRGB color ---
CRGB colorFromHexString(String hexString) {
    if (hexString.startsWith("#")) {
        hexString = hexString.substring(1); // Remove '#'
    }
    if (hexString.length() != 6) {
        Serial.print("Invalid hex string length: ");
        Serial.println(hexString);
        return CRGB::Black; // Return black for invalid input using FastLED's Black constant
    }

    // Convert hex string to unsigned long
    // Use strtol to convert hexadecimal string to long integer
    unsigned long hexValue = strtol(hexString.c_str(), NULL, 16);

    // Extract R, G, B components
    uint8_t r = (hexValue >> 16) & 0xFF;
    uint8_t g = (hexValue >> 8) & 0xFF;
    uint8_t b = hexValue & 0xFF;

    return CRGB(r, g, b); // Return a CRGB object
}

// --- Function to apply current segment colors to the strip ---
void applySegmentColors() {
    // First, clear the entire strip to ensure previous colors don't linger
    FastLED.clear();

    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
        // Only draw if the segment actually has pixels (end >= start)
        if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
            for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
                // Ensure pixel index is within bounds of the strip
                if (i < NUM_LEDS) { // Use NUM_LEDS for array bounds checking
                    leds[i] = currentSegmentColors[seg]; // Set pixel directly in the CRGB array
                }
            }
        }
    }
    FastLED.show(); // Push colors to the strip using FastLED
    Serial.println("NeoPixel segments updated.");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n");

  // --- FastLED Initialization ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  Serial.println("FastLED strip initialized.");

  // Set initial brightness. The web page will send its default on load.
  FastLED.setBrightness(currentBrightness);

  // Initialize segment pixel ranges based on initial segmentLengths
  updateSegmentPixels();

  // Initialize segment colors to black
  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black;
  }
  applySegmentColors(); // Apply these initial segment colors (all black)

  // --- WiFi Setup (Access Point) ---
  Serial.print("Setting up Access Point \"");
  Serial.print(ssid);
  Serial.println("\"...");
  // Changed this line to make the network VISIBLE
  WiFi.softAP(ssid, password, 1, false);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println("SSID is now visible."); // Update serial message

  // --- Initialize LittleFS (or SPIFFS) ---
  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }
  Serial.println("LittleFS mounted successfully.");


  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Client requested root URL '/'");
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/ssid", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", ssid);
  });


  server.on("/setBrightness", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasArg("value")) {
      int brightnessPct = request->arg("value").toInt();
      currentBrightness = map(brightnessPct, 0, 100, 0, 255);
      FastLED.setBrightness(currentBrightness);
      FastLED.show();
      isBlinkingActive = false; // Stop blinking if brightness is changed
      Serial.print("Brightness set to: ");
      Serial.print(brightnessPct);
      Serial.println("%");
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
            Serial.print("Segment "); Serial.print(i + 1); Serial.print(" color set to: #"); Serial.println(hexColor);
        }
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        if (newLength < 1) newLength = 1;
        if (newLength != segmentLengths[i]) {
            segmentLengths[i] = newLength;
            anyLengthChanged = true;
            Serial.print("Segment "); Serial.print(i + 1); Serial.print(" length set to: "); Serial.println(newLength);
        }
      }
    }

    if (anyLengthChanged) {
        updateSegmentPixels();
    }

    if (anyColorChanged || anyLengthChanged) {
        applySegmentColors();
        isBlinkingActive = false; // Stop blinking if segments are updated
    } else {
        Serial.println("No segment colors or lengths changed.");
    }
    request->send(200, "text/plain", "Segment colors and lengths updated!");
  });

  // --- NEW: Route to set all LEDs to a specific color ---
  server.on("/setAllColor", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setAllColor command.");
    if (request->hasArg("color")) {
      String hexColor = request->arg("color");
      CRGB newColor = colorFromHexString(hexColor);

      for(int i=0; i<NUM_LEDS; i++) {
        leds[i] = newColor; // Set all LEDs to the chosen color
      }
      FastLED.show();
      isBlinkingActive = false; // Stop blinking
      Serial.print("All LEDs set to color: #"); Serial.println(hexColor);
      request->send(200, "text/plain", "All LEDs ON to selected color!");
    } else {
      request->send(400, "text/plain", "Color value missing");
    }
  });


  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command");
    for(int i=0; i<NUM_LEDS; i++) {
      leds[i] = CRGB::Black;
    }
    FastLED.show();
    isBlinkingActive = false; // Stop blinking when turning all OFF
    request->send(200, "text/plain", "Lights OFF");
  });

  // --- Routes for Blinking Feature ---
  server.on("/setBlinkInterval", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setBlinkInterval command.");
    if (request->hasArg("value")) {
      String receivedValueStr = request->arg("value"); // Get as String
      Serial.print("Received 'value' string: "); Serial.println(receivedValueStr); // Debug: Check string received

      blinkIntervalSeconds = receivedValueStr.toFloat(); // Convert String to float
      Serial.print("Converted blinkIntervalSeconds: "); Serial.println(blinkIntervalSeconds, 3); // Debug: Check float value with 3 decimal places

      // Ensure interval is within bounds (0.25 to 2 seconds)
      if (blinkIntervalSeconds < 0.25) blinkIntervalSeconds = 0.25;
      if (blinkIntervalSeconds > 2.0) blinkIntervalSeconds = 2.0;

      blinkIntervalMillis = (unsigned long)(blinkIntervalSeconds * 1000.0); // Use 1000.0 for float multiplication
      Serial.print("Calculated blinkIntervalMillis: "); Serial.println(blinkIntervalMillis); // Debug: Check final millis value

      Serial.print("Blink interval set to: ");
      Serial.print(blinkIntervalSeconds);
      Serial.println(" seconds");
      request->send(200, "text/plain", "Blink interval set!");
    } else {
      request->send(400, "text/plain", "Blink interval value missing");
    }
  });

  server.on("/triggerBlink", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /triggerBlink command.");
    isBlinkingActive = true;
    currentBlinkLedState = true; // Start with LEDs ON
    lastBlinkTime = millis();
    // Initially set the state to ON with the current segment colors
    applySegmentColors(); // Use segment colors for the ON state
    Serial.println("Blinking started with segment colors!");
    request->send(200, "text/plain", "Blinking started!");
  });

  server.on("/stopBlink", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /stopBlink command.");
    isBlinkingActive = false; // Deactivate blinking
    for(int i=0; i<NUM_LEDS; i++) {
      leds[i] = CRGB::Black; // Turn off LEDs when stopping
    }
    FastLED.show();
    Serial.println("Blinking stopped!");
    request->send(200, "text/plain", "Blinking stopped!");
  });


  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  // Check if blinking is active and if the interval has passed
  if (isBlinkingActive) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastBlinkTime >= blinkIntervalMillis) {
      lastBlinkTime = currentMillis;
      currentBlinkLedState = !currentBlinkLedState; // Toggle LED state

      if (currentBlinkLedState) {
        // LEDs ON: Apply the currently set segment colors
        applySegmentColors(); // Use segment colors for the ON state
        Serial.println("LEDs ON (Blink with segment colors)");
      } else {
        // LEDs OFF: Turn all pixels off
        for(int i=0; i<NUM_LEDS; i++) {
          leds[i] = CRGB::Black;
        }
        FastLED.show();
        Serial.println("LEDs OFF (Blink)");
      }
    }
  }
}

// This function is present in the original code but not used by the web interface.
void colorWipe(CRGB color, int wait) {
  for(int i=0; i<NUM_LEDS; i++) {
    leds[i] = color;
    FastLED.show();
    delay(wait);
  }
}
