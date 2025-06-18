#include <Arduino.h>
#include <LittleFS.h> // For file system operations
#include <FastLED.h>  // For controlling FastLED LEDs
#include <ESP8266WiFi.h>      // For WiFi.softAP()
#include <ESPAsyncTCP.h>      // Dependency for ESPAsyncWebServer
#include <ESPAsyncWebServer.h> // For creating the asynchronous web server

// --- WiFi Configuration (Soft AP Mode) ---
const char* ssid = "painter";     // Your desired AP SSID
const char* password = "123456789";   // Your desired AP Password (must be at least 8 characters)

// --- Web Server Object ---
AsyncWebServer server(80);

// --- FastLED Configuration ---
#define LED_PIN         D4 // D4 on NodeMCU (GPIO2) - connect your LED data line here
#define NUM_LEDS        150 // Set this to the ACTUAL number of LEDs on your strip
#define LED_TYPE        WS2812B // Most common NeoPixel type
#define COLOR_ORDER     GRB // Most common NeoPixel color order

CRGB leds[NUM_LEDS]; // Define the LED array

// --- Button Configuration ---
#define BUTTON_PIN D2 // - connect your push button here
volatile bool buttonPressed = false; // Flag set by interrupt, indicates button press
volatile unsigned long lastButtonPressMillis = 0; // For debouncing the button
const unsigned long debounceDelay = 200; // Debounce time in milliseconds

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


// --- Function to Draw a Specific Column of the BMP onto LEDs ---
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    return;
  }
  if (col_x >= bmp_w || col_x < 0) {
    FastLED.clear(); // Clear all LEDs
    FastLED.show();
    return;
  }

  FastLED.clear(); // Clear the LED buffer

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  // Iterate through BMP rows from TOP to BOTTOM (for visual display)
  for (int y_display = 0; y_display < bmp_h; y_display++) {
    // Calculate the corresponding row in the BMP file.
    // Since BMPs store data from bottom-up, to read from the visual top of the image first,
    // we need to access (bmp_h - 1 - y_display)th row in the file.
    int y_bmp_file_row = (bmp_h - 1) - y_display;

    // Calculate the pixel byte offset in the BMP file
    uint32_t pixel_byte_offset = bmp_offset + (y_bmp_file_row * total_bytes_per_row) + (col_x * bytes_per_pixel);
    current_bmpFile.seek(pixel_byte_offset);

    if (bmp_bitDepth == 24) {
      b = current_bmpFile.read();
      g = current_bmpFile.read();
      r = current_bmpFile.read();
    } else if (bmp_bitDepth == 32) {
      b = current_bmpFile.read();
      g = current_bmpFile.read();
      r = current_bmpFile.read();
      current_bmpFile.read(); // Skip Alpha channel
    } else {
      r = g = b = 0;
    }

    if (y_display < NUM_LEDS) {
      // Map y_display (0=image_top, bmp_h-1=image_bottom) to LED index.
      // If strip pixel 0 is at the bottom, then:
      // image_top (y_display=0) -> NUM_LEDS - 1
      // image_bottom (y_display=bmp_h-1) -> 0
      // So, pixel_index = NUM_LEDS - 1 - y_display
      leds[NUM_LEDS - 1 - y_display] = CRGB(r, g, b);
    }
  }
  FastLED.show(); // Push the LED data to the strip
}

// --- Function to display the selected BMP image on LEDs ---
void displayBMPOnNeoPixels(String filename) {
  Serial.printf("Attempting to display image: %s\n", filename.c_str());

  // Close any previously open file to prevent resource leaks
  if (current_bmpFile) {
    current_bmpFile.close();
  }

  // Load BMP header for the selected file to get dimensions
  if (!loadBMPHeader(filename, bmp_w, bmp_h, bmp_offset, bmp_bitDepth, bmp_row_padding)) {
    Serial.printf("Failed to load header for %s. Cannot display.\n", filename.c_str());
    FastLED.clear(); // Clear LEDs on error
    FastLED.show();
    return;
  }

  // Open the file again specifically for reading pixel data during the display loop
  current_bmpFile = LittleFS.open("/" + filename, "r");
  if (!current_bmpFile) {
    Serial.printf("Failed to open BMP file %s for pixel data!\n", filename.c_str());
    FastLED.clear(); // Clear LEDs on error
    FastLED.show();
    return;
  }

  // Define the total duration for the image to be "painted" across the strip
  int displayDurationMs = 5000; // Total time for image to be displayed in milliseconds (5 seconds)
  if (bmp_w == 0) bmp_w = 1; // Avoid division by zero if image width is zero
  int delay_per_column_ms = displayDurationMs / bmp_w; // Calculate delay between each column

  // Iterate through each column of the BMP image
  for (int current_column = 0; current_column < bmp_w; current_column++) {
    drawColumn(current_column); // Draw the current column on the LEDs
    delay(delay_per_column_ms); // Pause to control the painting speed

    // Crucial check: Allow interruption if a new selection or button press occurs
    if (newSelectionReady || displayTriggeredByButton) {
      break; // Exit the display loop immediately
    }
  }

  current_bmpFile.close(); // Close the file after the display sequence is complete
  FastLED.clear(); // Turn off all LEDs
  FastLED.show();
  Serial.println("Image display complete. Clearing strip. Waiting for next web selection/button press.");
}

// --- Function to handle reading static files (e.g., HTML, CSS, JS, Images) from LittleFS ---
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
  Serial.println("\nNodeMCU v1 NeoPixel BMP Web Server (Soft AP Mode) with Button Trigger (FastLED)");

  // --- Initialize LittleFS ---
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Please ensure files are uploaded to LittleFS.");
    while (true); // Halt execution if LittleFS fails to mount (critical error)
  }
  Serial.println("LittleFS Mounted Successfully.");

  // --- WiFi Soft AP Configuration ---
  Serial.print("Setting up AP (Access Point)... ");
  WiFi.mode(WIFI_AP); // Set WiFi to Access Point mode
  WiFi.softAP(ssid, password); // Start the Soft AP with specified SSID and password
  Serial.println("Done.");
  Serial.print("AP SSID: ");
  Serial.println(ssid);
  Serial.print("AP Password: ");
  Serial.println(password);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());

  // --- FastLED Initialization ---
  // This line is crucial for FastLED. It adds your LED strip to the FastLED controller.
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(120); // Set a moderate brightness (0-255)
  FastLED.show(); // Turn off all LEDs initially

  // --- Configure button pin and interrupt ---
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Set button pin as input with internal pull-up resistor
  // Attach the interrupt: call handleButtonPress on a falling edge (button press)
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  Serial.println("Button configured on D2 (GPIO4)."); // Note: D2 on NodeMCU is GPIO4

  // --- Web Server Routes (Endpoints) ---

  // Route for the root HTML page (when client accesses http://192.168.4.1/)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  // Route to list available BMP files on LittleFS
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

  // Route to serve thumbnail/preview of a BMP image
  server.on("/bmpthumb", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("filename")) { // Check if 'filename' parameter is present
      String filename = request->getParam("filename")->value(); // Get filename from parameter
      // Directly send the BMP file. AsyncWebServer's send(FS, path) handles file existence.
      request->send(LittleFS, "/" + filename, "image/bmp");
    } else {
      request->send(400, "text/plain", "Missing filename parameter for thumbnail."); // Bad request if no filename
    }
  });

  // Route to select an image from the web, but not display it immediately
  server.on("/select_image", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("filename")) {
      selectedBmpFilename = request->getParam("filename")->value(); // Store the selected filename
      newSelectionReady = true; // Signal that a new image has been selected
      request->send(200, "text/plain", "Image '" + selectedBmpFilename + "' selected. Press button on D2 to display.");
    } else {
      request->send(400, "text/plain", "Missing filename parameter.");
    }
  });

  // Handle requests for files not explicitly defined in routes (e.g., CSS, JS, etc.)
  server.onNotFound([](AsyncWebServerRequest *request){
    // Try to serve the file from LittleFS using the generic handleFileRead function
    if(!handleFileRead(request->url(), request)) {
        request->send(404, "text/plain", "Not Found"); // Send 404 if file not found
    }
  });

  server.begin(); // Start the web server
  Serial.println("HTTP server started.");
  Serial.println("Ready. Connect to WiFi SSID: 'painter' (Password: '123456789') then open a browser to http://192.168.4.1");
}

// --- Loop Function ---
void loop() {
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
        displayBMPOnNeoPixels(selectedBmpFilename); // Trigger the display function
    } else {
        Serial.println("Button pressed, but no image is selected yet from the web interface.");
        // Visually alert the user that no image is selected
        fill_solid(leds, NUM_LEDS, CRGB::Red); // Fill all LEDs red
        FastLED.show();
        delay(1000); // Keep red for 1 second
        FastLED.clear(); // Turn off
        FastLED.show();
    }
  }
}
