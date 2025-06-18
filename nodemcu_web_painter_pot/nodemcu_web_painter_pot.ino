#include <Arduino.h>
// --- ESP8266 Specific Includes ---
#include <ESP8266WiFi.h>       // ESP8266 WiFi library
#include <ESPAsyncTCP.h>       // ESP8266 Async TCP (for AsyncWebServer)
#include <ESPAsyncWebServer.h> // ESP8266 AsyncWebServer
#include <FS.h>                // ESP8266 File System base
#include <LittleFS.h>          // ESP8266 LittleFS specific for flash file system

#include <Adafruit_NeoPixel.h> // Adafruit NeoPixel library!

// --- WiFi Configuration (Soft AP Mode) ---
const char* ssid = "painter";
const char* password = "123456789";

// --- Web Server Object ---
AsyncWebServer server(80);

// --- NeoPixel Configuration ---
#define NEOPIXEL_PIN    D4 // NodeMCU: D4 is GPIO2, a common choice for NeoPixels.
#define NEOPIXEL_COUNT  144 // Set this to the ACTUAL number of LEDs on your strip

// Declare an Adafruit_NeoPixel object
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- Button Configuration ---
#define BUTTON_PIN D7 // NodeMCU: D7 is GPIO13.
volatile bool buttonPressed = false;
volatile unsigned long lastButtonPressMillis = 0;
const unsigned long debounceDelay = 200;

// --- Potentiometer (A0) Configuration ---
#define POT_PIN A0 // NodeMCU: A0 is the only analog input pin (GPIO17).

// Global variable for display duration, updated by pot, sampled on button press.
int currentDynamicDisplayDurationMs = 5000;

// --- Global BMP Variables ---
int bmp_w, bmp_h;
uint32_t bmp_offset;
uint16_t bmp_bitDepth;
int bmp_row_padding;
File current_bmpFile;

// --- Variables for communication between web/ISR and loop() ---
String selectedBmpFilename = "";
volatile bool newSelectionReady = false;
volatile bool displayTriggeredByButton = false;

// --- Helper Functions for Reading BMP File Data ---
uint16_t read16(File f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  return result;
}

uint32_t read32(File f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();
  return result;
}

// --- Function to Load BMP Header Information ---
bool loadBMPHeader(const String& filename, int& width, int& height, uint32_t& offset, uint16_t& bitDepth, int& row_padding) {
  File bmpFile = LittleFS.open("/" + filename, "r");
  if (!bmpFile) {
    Serial.printf("Failed to open BMP file %s for reading header!\n", filename.c_str());
    return false;
  }

  if (read16(bmpFile) != 0x4D42) {
    Serial.printf("File %s is not a valid BMP file!\n", filename.c_str());
    bmpFile.close();
    return false;
  }

  read32(bmpFile);
  read32(bmpFile);

  offset = read32(bmpFile);

  read32(bmpFile);
  width = read32(bmpFile);
  height = read32(bmpFile);

  read16(bmpFile);
  bitDepth = read16(bmpFile);

  if (bitDepth != 24 && bitDepth != 32) {
    Serial.printf("Unsupported BMP bit depth for %s (only 24-bit or 32-bit uncompressed BMPs are supported).\n", filename.c_str());
    bmpFile.close();
    return false;
  }

  row_padding = (4 - (width * (bitDepth / 8)) % 4) % 4;
  bmpFile.close();
  return true;
}

/// --- Function to Draw a Specific Column of the BMP onto NeoPixels ---
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    return;
  }
  if (col_x >= bmp_w || col_x < 0) {
    strip.clear();
    strip.show();
    return;
  }

  strip.clear();

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  // Iterate through BMP rows from TOP to BOTTOM (for visual display)
  // bmp_h - 1 is the top-most row in BMP's internal indexing
  // 0 is the bottom-most row in BMP's internal indexing
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

    // Now, map this pixel to the correct LED.
    // If your strip has pixel 0 at the physical bottom and increases upwards,
    // then 'y_display' (which goes from 0 to bmp_h-1, representing top-to-bottom of image)
    // should be mapped directly to the LED index.
    // However, if your strip's pixel 0 is at the physical TOP and increases downwards,
    // then you'd use 'y_display' directly.
    // If pixel 0 is at the physical BOTTOM and increases upwards,
    // then you'd use (NEOPIXEL_COUNT - 1 - y_display).

    // Let's assume your strip is mounted such that pixel 0 is at the physical bottom
    // and pixels increase upwards towards the top of your display area.
    // So, the 'y_display' (top-to-bottom image coordinate) should map to
    // the LED index (0 for bottom, NEOPIXEL_COUNT-1 for top).
    // This requires mapping the 'top' of the image (y_display=0) to NEOPIXEL_COUNT-1,
    // and the 'bottom' of the image (y_display=bmp_h-1) to pixel 0.

    if (y_display < NEOPIXEL_COUNT) {
      // Map y_display (0=image_top, bmp_h-1=image_bottom) to NeoPixel index.
      // If strip pixel 0 is at the bottom, then:
      // image_top (y_display=0) -> NEOPIXEL_COUNT - 1
      // image_bottom (y_display=bmp_h-1) -> 0
      // So, pixel_index = NEOPIXEL_COUNT - 1 - y_display
      strip.setPixelColor(NEOPIXEL_COUNT - 1 - y_display, r, g, b);
    }
  }
  strip.show();
}

// --- Function to display the selected BMP image on NeoPixels ---
void displayBMPOnNeoPixels(String filename, int durationMs) {
  Serial.printf("Attempting to display image: %s with fixed duration %dms\n", filename.c_str(), durationMs);

  if (current_bmpFile) {
    current_bmpFile.close();
  }

  if (!loadBMPHeader(filename, bmp_w, bmp_h, bmp_offset, bmp_bitDepth, bmp_row_padding)) {
    Serial.printf("Failed to load header for %s. Cannot display.\n", filename.c_str());
    strip.clear();
    strip.show();
    return;
  }

  current_bmpFile = LittleFS.open("/" + filename, "r");
  if (!current_bmpFile) {
    Serial.printf("Failed to open BMP file %s for pixel data!\n", filename.c_str());
    strip.clear();
    strip.show();
    return;
  }

  // Calculate delay per column based on the PASSED durationMs (fixed for this display cycle)
  int delay_per_column_ms = 0;
  if (bmp_w > 0) { // Prevent division by zero
      delay_per_column_ms = durationMs / bmp_w;
  }
  if (delay_per_column_ms < 1) delay_per_column_ms = 1; // Ensure a minimum delay of 1ms

  // Iterate through each column of the BMP image
  for (int current_column = 0; current_column < bmp_w; current_column++) {
    drawColumn(current_column);
    delay(delay_per_column_ms); // Use the fixed delay for the current image
    yield(); // Yield for ESP8266 background tasks (important!)

    // Crucial check: Allow interruption if a new selection or button press occurs
    if (newSelectionReady || displayTriggeredByButton) {
      break; // Exit the display loop immediately
    }
  }

  current_bmpFile.close();
  strip.clear();
  strip.show();
  Serial.println("Image display complete. Clearing strip. Waiting for next web selection/button press.");
}

// --- Function to handle reading static files (e.g., HTML, CSS, JS, Images) from LittleFS ---
bool handleFileRead(String path, AsyncWebServerRequest *request) {
    if (path.endsWith("/")) path += "index.html";

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

    if (LittleFS.exists(path)) {
        request->send(LittleFS, path, contentType);
        return true;
    } else {
        Serial.println("File Not Found (in handleFileRead): " + path);
        return false;
    }
}

// --- Interrupt Service Routine (ISR) for the Button ---
// IRAM_ATTR is crucial for ESP8266 ISRs!
void IRAM_ATTR handleButtonPress() {
    if (millis() - lastButtonPressMillis > debounceDelay) {
        displayTriggeredByButton = true;
        lastButtonPressMillis = millis();
    }
}

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println("\nESP8266 NodeMCU NeoPixel BMP Web Server (Soft AP Mode) with Button Trigger and Potentiometer Control"); // Updated for ESP8266

  // --- Initialize LittleFS ---
  // For ESP8266, LittleFS.begin() usually doesn't take 'true' for formatting.
  // Ensure your ESP8266 board configuration is set to use 'LittleFS' or 'SPIFFS'
  // and you have uploaded files correctly.
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Please ensure files are uploaded to LittleFS.");
    while (true); // Halt execution if LittleFS fails to mount (critical error)
  }
  Serial.println("LittleFS Mounted Successfully.");

  // --- WiFi Soft AP Configuration ---
  Serial.print("Setting up AP (Access Point)... ");
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(ssid, password, 6)) {
    Serial.println("Done.");
  } else {
    Serial.println("FAILED to set up AP!");
    Serial.println("Possible causes: not enough memory, previous WiFi config conflict, or hardware issue.");
    for(int i=0; i<NEOPIXEL_COUNT; i++) {
      strip.setPixelColor(i, strip.Color(255, 0, 0)); // Red to indicate error
    }
    strip.show();
    while(true);
  }

  Serial.print("AP SSID: ");
  Serial.println(ssid);
  Serial.print("AP Password: ");
  Serial.println(password);
  Serial.print("AP IP Address: ");
  Serial.println(WiFi.softAPIP());

  // --- Read Potentiometer on A0 for BRIGHTNESS (ONLY AT BOOT) ---
  // ESP8266 ADC is 10-bit (0-1023)
  int potBrightnessValue = analogRead(POT_PIN);
  // Map 0-1023 (ESP8266 ADC) to 10-255 (NeoPixel brightness)
  int brightness = map(potBrightnessValue, 0, 1023, 10, 255); // Adjusted for 10-bit ADC
  Serial.printf("Initial Brightness set to: %d\n", brightness);

  // --- Adafruit NeoPixel Initialization ---
  strip.begin();
  strip.setBrightness(brightness); // Set brightness based on A0 value at boot (once)
  strip.show(); // Turn off all LEDs initially

  // Visual cue that AP is ready (e.g., green for a second)
  for(int i=0; i<NEOPIXEL_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(0, 255, 0)); // Green
  }
  strip.show();
  delay(1000);
  strip.clear();
  strip.show();

  // --- Configure button pin and interrupt ---
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Use digitalPinToInterrupt for ESP8266
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  Serial.printf("Button configured on GPIO%d.\n", BUTTON_PIN);


  // --- Web Server Routes (Endpoints) ---
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

  // *** IMPORTANT: Changed for ESP8266 LittleFS directory listing ***
  server.on("/listbmp", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    Dir dir = LittleFS.openDir("/"); // Use openDir for ESP8266
    bool firstFile = true;
    while (dir.next()) { // Iterate through files
      String filename = dir.fileName();
      if (filename.endsWith(".bmp") || filename.endsWith(".BMP")) {
        if (!firstFile) {
          json += ",";
        }
        json += "\"" + filename + "\"";
        firstFile = false;
      }
    }
    json += "]";
    request->send(200, "application/json", json);
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
      request->send(200, "text/plain", "Image '" + selectedBmpFilename + "' selected. Press button on GPIO13 to display.");
    } else {
      request->send(400, "text/plain", "Missing filename parameter.");
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    if(!handleFileRead(request->url(), request)) {
        request->send(404, "text/plain", "Not Found");
    }
  });

  server.begin();
  Serial.println("HTTP server started.");
  Serial.printf("Ready. Connect to WiFi SSID: '%s' (Password: '%s') then open a browser to http://%s\n", ssid, password, WiFi.softAPIP().toString().c_str());
  Serial.printf("Free Heap after start: %d bytes\n", ESP.getFreeHeap());
}

// --- Loop Function ---
void loop() {
  // --- Continuously read potentiometer for display speed PRE-SET ---
  // This updates 'currentDynamicDisplayDurationMs' so it reflects the desired speed
  // for the *next* image display.
  // ESP8266 ADC is 10-bit (0-1023)
  int potSpeedValue = analogRead(POT_PIN);
  // Map 0-1023 (ESP8266 ADC) to 2000-8000ms (2 to 8 seconds)
  currentDynamicDisplayDurationMs = map(potSpeedValue, 0, 1023, 2000, 8000);

  // Optional: Print current desired duration for debugging (as you turn the pot)
  // Serial.printf("Current desired display duration (for next image): %d ms\n", currentDynamicDisplayDurationMs);

  // Check if a new image was selected from the web interface
  if (newSelectionReady) {
    newSelectionReady = false;
    Serial.printf("New image selected from web: %s\n", selectedBmpFilename.c_str());
  }

  // Check if the physical button was pressed AND a file has been selected via the web interface
  if (displayTriggeredByButton) {
    displayTriggeredByButton = false;

    if (selectedBmpFilename != "") {
        // Pass the CURRENT value of currentDynamicDisplayDurationMs when the button is pressed.
        // This value will then be fixed for the entire display cycle.
        displayBMPOnNeoPixels(selectedBmpFilename, currentDynamicDisplayDurationMs);
    } else {
        Serial.println("Button pressed, but no image is selected yet from the web interface.");
        for(int i=0; i<NEOPIXEL_COUNT; i++) {
          strip.setPixelColor(i, strip.Color(255, 0, 0)); // Flash red
        }
        strip.show();
        delay(1000);
        strip.clear();
        strip.show();
    }
  }
  delay(10); // Give the ESP8266 time for background tasks
}
