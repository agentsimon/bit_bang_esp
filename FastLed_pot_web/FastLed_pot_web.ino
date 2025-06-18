#include <Arduino.h>
#include <LittleFS.h> // For file system operations
#include <Adafruit_NeoPixel.h> // For controlling NeoPixel LEDs
#include <ESP8266WiFi.h>         // For WiFi.softAP()
#include <ESPAsyncTCP.h>         // Dependency for ESPAsyncWebServer
#include <ESPAsyncWebServer.h> // For creating the asynchronous web server

// --- WiFi Configuration (Soft AP Mode) ---
const char* ssid = "painter";          // Your desired AP SSID - NOT HIDDEN
const char* password = "123456789";    // Your desired AP Password (must be at least 8 characters)

// --- Web Server Object ---
AsyncWebServer server(80);

// --- NeoPixel Configuration ---
#define NEOPIXEL_PIN    D4 // D4 on NodeMCU (GPIO2) - connect your NeoPixel data line here
#define NEOPIXEL_COUNT  50 // Set this to the ACTUAL number of LEDs on your strip
#define NEOPIXEL_TYPE  NEO_GRB + NEO_KHZ800 // Make sure this matches your strip (most common is NEO_GRB)
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEOPIXEL_TYPE);

// --- Button Configuration ---
#define BUTTON_PIN D7 // D7 on NodeMCU  
volatile bool buttonPressed = false; // Flag set by interrupt, indicates button press
volatile unsigned long lastButtonPressMillis = 0; // For debouncing the button
const unsigned long debounceDelay = 200; // Debounce time in milliseconds

// --- Potentiometer (A0) Configuration ---
#define POT_PIN A0 // Analog input pin for potentiometer

// Global variable to hold the currently calculated display duration based on A0
int currentDynamicDisplayDurationMs = 5000; // Default for safety, updated dynamically

// --- Global BMP Variables ---
// These will store information about the currently loaded BMP image
int bmp_w, bmp_h;
uint32_t bmp_offset;
uint16_t bmp_bitDepth;
int bmp_row_padding;
File current_bmpFile; // Global file handle for the currently open BMP for drawing

// --- Variables for communication between web/ISR and loop() ---
String selectedBmpFilename = ""; // Stores the filename selected from the web interface
volatile bool newSelectionReady = false; // Flag: true when a new image is selected via web
volatile bool displayTriggeredByButton = false; // Flag: true when the physical button is pressed

// --- Helper Functions for Reading BMP File Data ---
// Reads a 16-bit unsigned integer from the file (little-endian)
uint16_t read16(File f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  return result;
}

// Reads a 32-bit unsigned integer from the file (little-endian)
uint32_t read32(File f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();
  return result;
}

// --- Function to Load BMP Header Information ---
// Parses the BMP header to extract image dimensions, pixel data offset, and bit depth.
bool loadBMPHeader(const String& filename, int& width, int& height, uint32_t& offset, uint16_t& bitDepth, int& row_padding) {
  File bmpFile = LittleFS.open("/" + filename, "r");
  if (!bmpFile) {
    Serial.printf("Failed to open BMP file %s for reading header!\n", filename.c_str());
    return false;
  }

  // Check for BMP magic number "BM" (0x4D42)
  if (read16(bmpFile) != 0x4D42) {
    Serial.printf("File %s is not a valid BMP file!\n", filename.c_str());
    bmpFile.close();
    return false;
  }
  
  // Skip file size (4 bytes) and reserved (4 bytes)
  read32(bmpFile);
  read32(bmpFile);
  
  offset = read32(bmpFile); // Pixel data offset

  // Read DIB header size (4 bytes)
  read32(bmpFile);
  width = read32(bmpFile); // Image width
  height = read32(bmpFile); // Image height
  
  // Skip color planes (2 bytes)
  read16(bmpFile);
  bitDepth = read16(bmpFile); // Bits per pixel

  // Only supports 24-bit (BGR) or 32-bit (BGRA) uncompressed BMPs
  if (bitDepth != 24 && bitDepth != 32) {
    Serial.printf("Unsupported BMP bit depth for %s (only 24-bit or 32-bit uncompressed BMPs are supported).\n", filename.c_str());
    bmpFile.close();
    return false;
  }

  // Calculate row padding for 4-byte alignment
  row_padding = (4 - (width * (bitDepth / 8)) % 4) % 4;
  bmpFile.close(); // Close the file after reading header
  return true;
}

// --- Function to Draw a Specific Column of the BMP onto NeoPixels ---
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    return;
  }
  if (col_x >= bmp_w || col_x < 0) {
    // Column out of bounds, clear strip and return
    strip.clear();
    strip.show();
    return;
  }

  strip.clear(); // Clear the strip before drawing a new column

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  for (int y_img = 0; y_img < bmp_h; y_img++) {
    // Calculate the byte offset for the start of the pixel data for this column and row
    uint32_t pixel_byte_offset = bmp_offset + (y_img * total_bytes_per_row) + (col_x * bytes_per_pixel);
    current_bmpFile.seek(pixel_byte_offset); // Move file pointer to the pixel's data

    // Read BGR (or BGRA for 32-bit) pixel data
    if (bmp_bitDepth == 24) {
      b = current_bmpFile.read();
      g = current_bmpFile.read();
      r = current_bmpFile.read();
    } else if (bmp_bitDepth == 32) {
      b = current_bmpFile.read();
      g = current_bmpFile.read();
      r = current_bmpFile.read();
      current_bmpFile.read(); // Skip Alpha channel for 32-bit BMPs
    } else {
      r = g = b = 0; // Should not happen due to header check, but a fallback
    }

    // Map image Y-coordinate to NeoPixel index (NeoPixels are 0-indexed, BMP is bottom-up)
    if (y_img < NEOPIXEL_COUNT) {
      strip.setPixelColor(NEOPIXEL_COUNT - 1 - y_img, strip.Color(r, g, b));
    }
  }
  strip.show(); // Update the NeoPixel strip with the new column
}

// --- Function to display the selected BMP image on NeoPixels ---
// Now accepts displayDurationMs as a parameter
void displayBMPOnNeoPixels(String filename, int durationMs) {
  Serial.printf("Attempting to display image: %s with duration %dms\n", filename.c_str(), durationMs);

  // Close any previously open file to prevent resource leaks
  if (current_bmpFile) {
    current_bmpFile.close();
  }

  // Load BMP header for the selected file to get dimensions
  if (!loadBMPHeader(filename, bmp_w, bmp_h, bmp_offset, bmp_bitDepth, bmp_row_padding)) {
    Serial.printf("Failed to load header for %s. Cannot display.\n", filename.c_str());
    strip.clear(); // Clear LEDs on error
    strip.show();
    return;
  }

  // Open the file again specifically for reading pixel data during the display loop
  current_bmpFile = LittleFS.open("/" + filename, "r");
  if (!current_bmpFile) {
    Serial.printf("Failed to open BMP file %s for pixel data!\n", filename.c_str());
    strip.clear(); // Clear LEDs on error
    strip.show();
    return;
  }
  
  // Calculate delay per column based on the passed durationMs
  if (bmp_w == 0) bmp_w = 1; // Avoid division by zero if image width is zero
  int delay_per_column_ms = durationMs / bmp_w; // Calculate delay between each column

  // Iterate through each column of the BMP image
  for (int current_column = 0; current_column < bmp_w; current_column++) {
    drawColumn(current_column); // Draw the current column on the NeoPixels
    delay(delay_per_column_ms); // Pause to control the painting speed

    // Crucial check: Allow interruption if a new selection or button press occurs
    if (newSelectionReady || displayTriggeredByButton) {
      break; // Exit the display loop immediately
    }
  }

  current_bmpFile.close(); // Close the file after the display sequence is complete
  strip.clear(); // Turn off all LEDs
  strip.show();
  Serial.println("Image display complete. Clearing strip. Waiting for next web selection/button press.");
}

// --- Function to handle reading static files (e.g., HTML, CSS, JS, Images) from LittleFS ---
// This function is adapted for ESPAsyncWebServer
bool handleFileRead(String path, AsyncWebServerRequest *request) {
    if (path.endsWith("/")) path += "index.html"; // If a directory is requested, serve index.html

    // Determine the content type based on file extension
    String contentType = "text/plain";
    if (path.endsWith(".html")) contentType = "text/html";
    else if (path.endsWith(".css")) contentType = "text/css";
    else if (path.endsWith(".js")) contentType = "application/javascript";
    else if (path.endsWith(".png")) contentType = "image/png";
    else if (path.endsWith(".gif")) contentType = "image/gif";
    else if (path.endsWith(".jpg")) contentType = "image/jpeg";
    else if (path.endsWith(".ico")) contentType = "image/x-icon";
    else if (path.endsWith(".xml")) contentType = "text/xml";
    else if (path.endsWith(".pdf")) contentType = "application/x-pdf";
    else if (path.endsWith(".zip")) contentType = "application/x-zip";
    else if (path.endsWith(".gz")) contentType = "application/x-gzip";
    else if (path.endsWith(".bmp")) contentType = "image/bmp";

    // Check if the file exists in LittleFS and send it
    if (LittleFS.exists(path)) {
        request->send(LittleFS, path, contentType); // AsyncWebServer's method to send files
        return true;
    } else {
        Serial.println("File Not Found (in handleFileRead): " + path);
        return false;
    }
}

// --- Interrupt Service Routine (ISR) for the Button ---
// IRAM_ATTR is crucial for functions called by interrupts on ESP8266
IRAM_ATTR void handleButtonPress() {
    // Debounce logic: only trigger if enough time has passed since the last press
    if (millis() - lastButtonPressMillis > debounceDelay) {
        displayTriggeredByButton = true; // Set the flag to signal the loop to process the press
        lastButtonPressMillis = millis(); // Update the last press time
    }
}

// --- Setup Function ---
void setup() {
  Serial.begin(115200); // Start serial communication for debugging
  delay(1000); // Give time for serial monitor to connect
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap()); // Good addition! Check this value.
  Serial.println("\nNodeMCU v1 NeoPixel BMP Web Server (Soft AP Mode) with Button Trigger and Potentiometer Control");

  // --- Initialize LittleFS ---
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Please ensure files are uploaded to LittleFS.");
    while (true); // Halt execution if LittleFS fails to mount (critical error)
  }
  Serial.println("LittleFS Mounted Successfully.");


  // --- WiFi Soft AP Configuration ---
  Serial.print("Setting up AP (Access Point)... ");
  WiFi.mode(WIFI_AP); // Set WiFi to Access Point mode
  // !!! CRITICAL CHECK HERE !!!
  // Add a success/failure check for WiFi.softAP()
  if (WiFi.softAP(ssid, password, 6)) { // Start the Soft AP with specified SSID and password on channal 6
    Serial.println("Done.");
  } else {
    Serial.println("FAILED to set up AP!"); // <--- IMPORTANT: Look for this in serial monitor
    Serial.println("Possible causes: not enough memory, previous WiFi config conflict, or hardware issue.");
    // Optionally, halt or provide a visual error on LEDs
    strip.fill(strip.Color(255, 0, 0)); // Red to indicate error
    strip.show();
    while(true); // Halt, as AP is critical
  }

  Serial.print("AP SSID: ");
  Serial.println(ssid); // Explicitly prints the SSID to Serial Monitor
  Serial.print("AP Password: ");
  Serial.println(password);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP()); // Print the IP address clients can connect to


  // --- Read Potentiometer on A0 for BRIGHTNESS (on boot) ---
  int potBrightnessValue = analogRead(POT_PIN);
  
  // Map the potentiometer value (0-1023) to NeoPixel brightness (e.g., 10 to 255)
  // 10 is chosen as a minimum to ensure some visibility even at lowest setting
  int brightness = map(potBrightnessValue, 0, 1023, 10, 255);
  
  // --- NeoPixel Initialization ---
  strip.begin(); // Initialize NeoPixel library
  strip.setBrightness(brightness); // Set brightness based on A0 value at boot
  strip.show(); // Turn off all LEDs initially

  // Visual cue that AP is ready (e.g., green for a second)
  for(int i=0; i<NEOPIXEL_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(0, 255, 0)); // Green
  }
  strip.show();
  delay(1000); // Show green for 1 second
  strip.clear(); // Turn off
  strip.show();

  // --- Configure button pin and interrupt ---
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Set button pin as input with internal pull-up resistor
  // Attach the interrupt: call handleButtonPress on a falling edge (button press)
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  Serial.println("Button configured on D1 (GPIO5).");


  // --- Web Server Routes (Endpoints) ---

  // All these routes seem standard for AsyncWebServer and LittleFS.
  // The structure is fine, but the *content* of the files served might be an issue.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  server.on("/network_info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"ssid\":\"" + String(ssid) + "\",";
    json += "\"password\":\"" + String(password) + "\",";
    json += "\"ip\":\"" + WiFi.softAPIP().toString() + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/listbmp", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    Dir dir = LittleFS.openDir("/"); // Open the root directory of LittleFS
    bool firstFile = true;
    while (dir.next()) { // Iterate through files
      String filename = dir.fileName();
      if (filename.endsWith(".bmp") || filename.endsWith(".BMP")) { // Check for BMP extension (case-insensitive)
        if (!firstFile) {
          json += ","; // Add comma for subsequent files
        }
        json += "\"" + filename + "\""; // Add filename to JSON array
        firstFile = false;
      }
    }
    json += "]"; // Close JSON array
    request->send(200, "application/json", json); // Send the JSON response
  });

  server.on("/bmpthumb", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("filename")) {
      String filename = request->getParam("filename")->value();
      request->send(LittleFS, "/" + filename, "image/bmp");
    } else {
      request->send(400, "text/plain", "Missing filename parameter for thumbnail.");
    }
  });

  server.on("/select_image", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("filename")) {
      selectedBmpFilename = request->getParam("filename")->value();
      newSelectionReady = true;
      request->send(200, "text/plain", "Image '" + selectedBmpFilename + "' selected. Press button on D1 to display.");
    } else {
      request->send(400, "text/plain", "Missing filename parameter.");
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    if(!handleFileRead(request->url(), request)) {
        request->send(404, "text/plain", "Not Found");
    }
  });

  server.begin(); // Start the web server
  Serial.println("HTTP server started.");
  Serial.println("Ready. Connect to WiFi SSID: 'painter' (Password: '123456789') then open a browser to http://192.168.4.1");
  Serial.printf("Free Heap after start: %d bytes\n", ESP.getFreeHeap()); // Good addition! Check this value too.
}

// --- Loop Function ---
void loop() {
  //Serial.printf("Free Heap in loop: %d bytes\n", ESP.getFreeHeap());
  // --- Continuously read potentiometer for dynamic display speed ---
  // A0 is only read here, not within displayBMPOnNeoPixels, as per requirement
  int potSpeedValue = analogRead(POT_PIN);
  // Map the potentiometer value (0-1023) to a display duration (2000ms to 8000ms)
  // Ensure minimums/maximums are respected even if pot is slightly outside range
  currentDynamicDisplayDurationMs = map(potSpeedValue, 0, 1023, 2000, 8000);

  // Check if a new image was selected from the web interface
  if (newSelectionReady) {
    newSelectionReady = false; // Reset the flag immediately
    Serial.printf("New image selected from web: %s\n", selectedBmpFilename.c_str());
    // (Optional) Add a subtle LED blink or color to confirm selection visually
  }

  // Check if the physical button was pressed AND a file has been selected via the web interface
  if (displayTriggeredByButton) {
    displayTriggeredByButton = false; // Reset the button flag immediately

    if (selectedBmpFilename != "") {
        Serial.printf("Button pressed. Displaying currently selected image: %s\n", selectedBmpFilename.c_str());
        // Pass the dynamically calculated duration to the display function
        displayBMPOnNeoPixels(selectedBmpFilename, currentDynamicDisplayDurationMs);
    } else {
        Serial.println("Button pressed, but no image is selected yet from the web interface.");
        // Visually alert the user that no image is selected
        strip.fill(strip.Color(255, 0, 0), 0, NEOPIXEL_COUNT); // Flash red
        strip.show();
        delay(1000); // Keep red for 1 second
        strip.clear(); // Turn off
        strip.show();
    }
  }
}
