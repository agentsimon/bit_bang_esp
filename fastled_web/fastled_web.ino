#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h> // Include the FastLED library

// --- WiFi Configuration (Access Point Mode) ---
const char* ssid = "NeoPixel_Controller"; // The name of the Wi-Fi network the NodeMCU will create
const char* password = "123456789";       // The password for your Wi-Fi network (min 8 characters)

// --- Web Server Object ---
AsyncWebServer server(80); // Create an AsyncWebServer object on port 80 (standard HTTP)

// --- NeoPixel Configuration for FastLED ---
#define LED_PIN           D2  // Digital pin connected to the NeoPixel data input (D2 on NodeMCU)
#define NUM_LEDS          99  // Total number of pixels in your strip

// Define the LED type and color order (most common for WS2812B is GRB)
#define LED_TYPE          WS2812B
#define COLOR_ORDER       GRB

// Create a FastLED CRGB array to hold the LED color data
CRGB leds[NUM_LEDS]; // This replaces the Adafruit_NeoPixel object

// --- NeoPixel Segment Definitions ---
const int NUM_SEGMENTS = 6;
// These arrays will now be dynamically updated
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19}; // Initial lengths, summing to 99
int segmentStartPixel[NUM_SEGMENTS]; // Removed 'const'
int segmentEndPixel[NUM_SEGMENTS];   // Removed 'const'

// Store current colors for each segment (initialized to black/off)
CRGB currentSegmentColors[NUM_SEGMENTS]; // Changed type to CRGB

// Global variable to store current brightness (0-255)
uint8_t currentBrightness = 153; // Initial brightness 60% (153 out of 255)

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

// --- HTML for the Web Page ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>NodeMCU NeoPixel Segments</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin: 20px; }
    h1 { color: #333; }
    p { font-size: 1.2em; }
    .status {
      padding: 10px;
      border: 1px solid #ccc;
      background-color: #f0f0f0;
      margin-top: 20px;
      margin-bottom: 20px;
    }
    .button {
      background-color: #4CAF50; /* Green */
      border: none;
      color: white;
      padding: 15px 32px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 16px;
      margin: 4px 2px;
      cursor: pointer;
      border-radius: 8px;
    }
    .button.off { background-color: #f44336; /* Red */ }
    .button.blue { background-color: #008CBA; /* Blue */ }
    .segment-control, .brightness-control {
      margin: 10px auto; /* Center segments */
      display: flex;
      flex-direction: column; /* Stack controls vertically within a segment */
      justify-content: center;
      align-items: center;
      border: 1px solid #eee;
      padding: 10px;
      margin-bottom: 15px;
      border-radius: 8px;
      max-width: 400px; /* Limit width for better appearance */
    }
    .segment-row { /* For horizontal alignment of label, color picker, length slider */
        display: flex;
        align-items: center;
        margin-bottom: 5px;
    }
    .segment-row label {
      margin-right: 10px;
      font-weight: bold;
      min-width: 150px; /* Align labels */
      text-align: left;
    }
    .length-control {
        display: flex;
        align-items: center;
        margin-top: 5px;
        width: 100%; /* Take full width of segment-control */
        justify-content: center;
    }
    .length-control label {
        margin-right: 10px;
        font-weight: bold;
        min-width: 50px; /* Smaller width for length label */
        text-align: right;
    }
    input[type="color"] {
      -webkit-appearance: none;
      border: none;
      width: 50px; /* Smaller color picker */
      height: 25px;
      cursor: pointer;
    }
    input[type="color"]::-webkit-color-swatch-wrapper {
      padding: 0;
    }
    input[type="color"]::-webkit-color-swatch {
      border: 1px solid #ccc;
      border-radius: 4px;
    }
    input[type="range"] {
        width: 180px; /* Adjust slider width */
        margin: 0 10px; /* Spacing for slider */
    }
    #message {
        margin-top: 10px;
        font-weight: bold;
        color: green;
    }
  </style>
</head>
<body>
  <h1>NodeMCU NeoPixel Segment Controller</h1>
  <p>Control your NeoPixel strip with dynamic segments.</p>

  <div class="status">
    <p>Status: <span id="wifiStatus">Access Point Mode</span></p>
    <p>IP Address: <span id="ipAddress">192.168.4.1</span></p>
  </div>
  <hr>

  <h2>Global Brightness</h2>
  <div class="brightness-control">
    <label for="brightnessSlider">Brightness:</label>
    <input type="range" id="brightnessSlider" min="0" max="100" value="60" oninput="sendBrightness(this.value)">
    <span id="brightnessValue">60%</span>
  </div>
  <hr>

  <h2>Segment Colour & Length Settings</h2>
  <div id="segmentControls">
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg0Color">Segment 1 (Pixels <span id="seg0Pixels">0-15</span>):</label>
            <input type="color" id="seg0Color" value="#FF0000">
        </div>
        <div class="length-control">
            <label for="seg0Length">Length:</label>
            <input type="range" id="seg0Length" min="1" max="99" value="16" oninput="updateLengthLabel(this.value, 0)">
            <span id="seg0LengthValue">16</span>
        </div>
    </div>
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg1Color">Segment 2 (Pixels <span id="seg1Pixels">16-31</span>):</label>
            <input type="color" id="seg1Color" value="#00FF00">
        </div>
        <div class="length-control">
            <label for="seg1Length">Length:</label>
            <input type="range" id="seg1Length" min="1" max="99" value="16" oninput="updateLengthLabel(this.value, 1)">
            <span id="seg1LengthValue">16</span>
        </div>
    </div>
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg2Color">Segment 3 (Pixels <span id="seg2Pixels">32-47</span>):</label>
            <input type="color" id="seg2Color" value="#0000FF">
        </div>
        <div class="length-control">
            <label for="seg2Length">Length:</label>
            <input type="range" id="seg2Length" min="1" max="99" value="16" oninput="updateLengthLabel(this.value, 2)">
            <span id="seg2LengthValue">16</span>
        </div>
    </div>
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg3Color">Segment 4 (Pixels <span id="seg3Pixels">48-63</span>):</label>
            <input type="color" id="seg3Color" value="#FFFF00">
        </div>
        <div class="length-control">
            <label for="seg3Length">Length:</label>
            <input type="range" id="seg3Length" min="1" max="99" value="16" oninput="updateLengthLabel(this.value, 3)">
            <span id="seg3LengthValue">16</span>
        </div>
    </div>
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg4Color">Segment 5 (Pixels <span id="seg4Pixels">64-79</span>):</label>
            <input type="color" id="seg4Color" value="#00FFFF">
        </div>
        <div class="length-control">
            <label for="seg4Length">Length:</label>
            <input type="range" id="seg4Length" min="1" max="99" value="16" oninput="updateLengthLabel(this.value, 4)">
            <span id="seg4LengthValue">16</span>
        </div>
    </div>
    <div class="segment-control">
        <div class="segment-row">
            <label for="seg5Color">Segment 6 (Pixels <span id="seg5Pixels">80-98</span>):</label>
            <input type="color" id="seg5Color" value="#FF00FF">
        </div>
        <div class="length-control">
            <label for="seg5Length">Length:</label>
            <input type="range" id="seg5Length" min="1" max="99" value="19" oninput="updateLengthLabel(this.value, 5)">
            <span id="seg5LengthValue">19</span>
        </div>
    </div>
  </div>

  <button class="button" onclick="setSegmentConfig()">Set Segment Colours & Lengths</button>
  <div id="message"></div>

  <hr>
  <h2>Quick Actions</h2>
  <button class="button" onclick="sendCmd('/on')">All ON (White)</button>
  <button class="button off" onclick="sendCmd('/off')">All OFF</button>
  
  <script>
    // Max LEDs is 99 (from NUM_LEDS define in C++). Used for dynamic label updates.
    const MAX_LEDS_TOTAL = 99; 
    let currentSegmentLengths = [16, 16, 16, 16, 16, 19]; // Keep track of current lengths in JS

    function sendCmd(cmd, callback) {
      var xhttp = new XMLHttpRequest();
      xhttp.onreadystatechange = function() {
        if (this.readyState == 4 && this.status == 200) {
          if (callback) {
            callback(this.responseText);
          }
        }
      };
      xhttp.open("GET", cmd, true);
      xhttp.send();
    }

    // Function to update the length label next to the slider
    function updateLengthLabel(value, segmentIndex) {
      document.getElementById(`seg${segmentIndex}LengthValue`).innerText = value;
      currentSegmentLengths[segmentIndex] = parseInt(value); // Update JS array
      updatePixelRangeLabels(); // Recalculate and update pixel range labels
    }

    // Function to update the (Pixels X-Y) labels for each segment
    function updatePixelRangeLabels() {
        let currentPixel = 0;
        for (let i = 0; i < 6; i++) {
            let length = currentSegmentLengths[i];
            let startPixel = currentPixel;
            let endPixel = Math.min(currentPixel + length - 1, MAX_LEDS_TOTAL - 1); // Cap at max LEDs

            let pixelRangeText = "";
            if (startPixel >= MAX_LEDS_TOTAL) { // If segment starts beyond total LEDs
                pixelRangeText = "N/A (out of range)";
            } else if (endPixel < startPixel) { // If segment is effectively empty
                pixelRangeText = "N/A (empty)";
            }
            else {
                pixelRangeText = `${startPixel}-${endPixel}`;
            }
            
            document.getElementById(`seg${i}Pixels`).innerText = pixelRangeText;
            currentPixel = endPixel + 1; // Prepare for next segment
        }
    }


    function setSegmentConfig() {
      let url = "/setSegments?"; // This endpoint now handles both colors and lengths
      for (let i = 0; i < 6; i++) {
        const colorInput = document.getElementById(`seg${i}Color`);
        const lengthInput = document.getElementById(`seg${i}Length`); 
        
        url += `s${i}=${colorInput.value.substring(1)}`; // Color parameter (s0, s1, etc.)
        url += `&l${i}=${lengthInput.value}`;             // Length parameter (l0, l1, etc.)
        if (i < 5) {
          url += "&";
        }
      }
      
      console.log("Sending URL:", url);
      sendCmd(url, function(response) {
        document.getElementById("message").innerText = response;
        setTimeout(() => {
          document.getElementById("message").innerText = "";
        }, 3000);
      });
    }

    function sendBrightness(value) {
      document.getElementById("brightnessValue").innerText = value + "%";
      const url = `/setBrightness?value=${value}`;
      console.log("Sending Brightness URL:", url);
      sendCmd(url); // No need for a specific response message here
    }

    // Initialize UI elements when the page loads
    window.onload = function() {
        sendBrightness(document.getElementById("brightnessSlider").value);
        // Set initial slider values and labels
        for (let i = 0; i < 6; i++) {
            const lengthSlider = document.getElementById(`seg${i}Length`);
            // Set slider value from the initial JS array (matches C++ initial values)
            lengthSlider.value = currentSegmentLengths[i]; 
            document.getElementById(`seg${i}LengthValue`).innerText = currentSegmentLengths[i];
        }
        updatePixelRangeLabels(); // Initial update of the (Pixels X-Y) labels
    };

  </script>
</body>
</html>
)rawliteral";


void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n");

  // --- FastLED Initialization ---
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  Serial.println("FastLED strip initialized.");

  // Set initial brightness. The web page will send its default on load.
  // This value (153) corresponds to the initial "value=60" in the HTML slider.
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
  // Changed this line to hide the SSID and specify channel 1
  WiFi.softAP(ssid, password, 1, true); 
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println("SSID is now hidden. You will need to manually connect to this network.");

  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Client requested root URL '/'");
    request->send_P(200, "text/html", index_html);
  });

  server.on("/setBrightness", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasArg("value")) {
      int brightnessPct = request->arg("value").toInt();
      currentBrightness = map(brightnessPct, 0, 100, 0, 255);
      FastLED.setBrightness(currentBrightness);
      FastLED.show();
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
      // Process Colors
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

      // Process Lengths
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
    } else {
        Serial.println("No segment colors or lengths changed.");
    }
    request->send(200, "text/plain", "Segment colors and lengths updated!");
  });

  server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /on command");
    for(int i=0; i<NUM_LEDS; i++) {
      leds[i] = CRGB::White;
    }
    FastLED.show();
    request->send(200, "text/plain", "Lights ON (White)");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /off command");
    for(int i=0; i<NUM_LEDS; i++) {
      leds[i] = CRGB::Black;
    }
    FastLED.show();
    request->send(200, "text/plain", "Lights OFF");
  });

  // Start the server
  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  // In an AsyncWebServer setup, the loop is mostly for background tasks,
  // like serial communication or non-HTTP related logic.
  // The web server handles requests asynchronously.
}

// --- FastLED Helper Function (Rewritten for FastLED) ---
// Fill the strip one pixel at a time with a given color.
void colorWipe(CRGB color, int wait) { // Changed color type to CRGB
  for(int i=0; i<NUM_LEDS; i++) { // Use NUM_LEDS
    leds[i] = color; // Set pixel directly
    FastLED.show(); // Send the update to the strip
    delay(wait);
  }
}
