// Include the FastLED library. You can download it from the Arduino Library Manager.
#include <FastLED.h>
// Included for ESP8266 compatibility, though often implicitly included
// Note: Teensy 3.5 is not an ESP8266, so <Arduino.h> is usually not explicitly needed,
// but it's harmless if present.
#include <Arduino.h>
// For ESP8266, ensure you have the LittleFS library installed for ESP8266.
// This is for file system operations to read BMP files.
#include <LittleFS.h>

// --- FastLED and NeoPixel Configuration ---
// Define the data pin connected to your Neopixel strip.
// The original code used NEOPIXEL_PIN 2 for NodeMCU D4.
// For Teensy 3.5, you can use any digital pin, e.g., pin 6 or 2.
// We'll use the NEOPIXEL_PIN defined in the original user code for consistency.
#define NEOPIXEL_PIN    2 // Corresponds to D4 on NodeMCU, adjust for Teensy if needed (e.g., 6)
#define NEOPIXEL_COUNT  149 // Set this to the ACTUAL number of LEDs on your strip

// Define the LED type and color order for FastLED.
// Most Neopixels are WS2812B, and GRB is the common color order.
// If your colors seem off (e.g., red is green), try changing GRB to RGB or BGR.
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

// Create an array of CRGB structures to hold the LED color data for FastLED.
CRGB leds[NEOPIXEL_COUNT];

// --- Button Configuration ---
// On NodeMCU v1: GPIO5 is D1
#define BUTTON_PIN      4 // Changed from 14 (D2 on Wemos) to 5 (D1 on NodeMCU)

// --- Global BMP Variables ---
int bmp_w, bmp_h;
uint32_t bmp_offset;
uint16_t bmp_bitDepth;
int bmp_row_padding;
File current_bmpFile;

// --- Helper Functions for Reading BMP File Data ---
// Reads a 16-bit unsigned integer from the file in little-endian format.
uint16_t read16(File f) {
  uint16_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  return result;
}

// Reads a 32-bit unsigned integer from the file in little-endian format.
uint32_t read32(File f) {
  uint32_t result;
  ((uint8_t *)&result)[0] = f.read();
  ((uint8_t *)&result)[1] = f.read();
  ((uint8_t *)&result)[2] = f.read();
  ((uint8_t *)&result)[3] = f.read();
  return result;
}

// --- Function to Load BMP Header Information ---
// Opens a BMP file and reads its header to get image dimensions, bit depth, and pixel data offset.
bool loadBMP(const char *filename) {
  current_bmpFile = LittleFS.open(filename, "r");
  if (!current_bmpFile) {
    Serial.println("Failed to open BMP file for reading!");
    return false;
  }

  // Check for the BMP magic number ('BM')
  if (read16(current_bmpFile) != 0x4D42) {
    Serial.println("Not a valid BMP file!");
    current_bmpFile.close();
    return false;
  }
  read32(current_bmpFile); // File size (unused)
  read32(current_bmpFile); // Reserved (unused)
  bmp_offset = read32(current_bmpFile); // Offset to pixel data

  read32(current_bmpFile); // DIB header size (unused)
  bmp_w = read32(current_bmpFile); // Image width
  bmp_h = read32(current_bmpFile); // Image height
  read16(current_bmpFile); // Color planes (unused)
  bmp_bitDepth = read16(current_bmpFile); // Bits per pixel

  Serial.printf("BMP Info: Width=%d, Height=%d, Bit Depth=%d, Offset=%d\n", bmp_w, bmp_h, bmp_bitDepth, bmp_offset);

  // Only support 24-bit or 32-bit uncompressed BMPs
  if (bmp_bitDepth != 24 && bmp_bitDepth != 32) {
    Serial.println("Unsupported BMP bit depth (only 24-bit or 32-bit uncompressed BMPs are supported).");
    current_bmpFile.close();
    return false;
  }

  // Calculate row padding for BMP files (rows are padded to 4-byte boundaries)
  bmp_row_padding = (4 - (bmp_w * (bmp_bitDepth / 8)) % 4) % 4;

  Serial.println("BMP header loaded successfully.");
  return true;
}

// --- Function to Draw a Specific Column of the BMP ---
// Reads pixel data for a given column from the BMP file and displays it on the LED strip.
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    Serial.println("Error: BMP file not open for drawing column.");
    return;
  }
  if (col_x >= bmp_w || col_x < 0) {
    Serial.printf("Column %d out of image bounds (width %d).\n", col_x, bmp_w);
    return;
  }

  // Clear the LED strip before drawing the new column
  FastLED.clear();

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  // Iterate through each pixel in the column (from bottom to top in BMP, but we'll map to strip order)
  for (int y_img = 0; y_img < bmp_h; y_img++) {
    // Calculate the byte offset for the current pixel in the BMP file
    uint32_t pixel_byte_offset = bmp_offset + (y_img * total_bytes_per_row) + (col_x * bytes_per_pixel);
    current_bmpFile.seek(pixel_byte_offset);

    // Read pixel color components (BMP stores BGR)
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
      r = g = b = 0; // Should not happen due to earlier check
    }

    // Map the BMP pixel to the corresponding LED on the strip
    // Assuming the strip is oriented such that y_img corresponds to the LED index
    if (y_img < NEOPIXEL_COUNT) {
      // Set the LED color using FastLED's CRGB structure
      leds[y_img] = CRGB(r, g, b);
    }
  }
  // Show the updated colors on the LED strip
  FastLED.show();
}


// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial a moment to connect
  Serial.println("\nNodeMCU v1 NeoPixel BMP Light Painter (Button Triggered) - Using FastLED"); // Changed board name and added FastLED

  // Initialize LittleFS file system
  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Please ensure files are uploaded to LittleFS.");
    while (true); // Halt if LittleFS fails
  }
  Serial.println("LittleFS Mounted Successfully.");

  // Load the initial BMP file. Make sure "frame000.bmp" is uploaded to your LittleFS.
  if (!loadBMP("frame000.bmp")) {
    Serial.println("Failed to load BMP. Halting.");
    while (true); // Halt if BMP loading fails
  }

  // Configure the button pin as input with internal pull-up resistor
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Initialize FastLED for the LED strip
  FastLED.addLeds<LED_TYPE, NEOPIXEL_PIN, COLOR_ORDER>(leds, NEOPIXEL_COUNT);
  
  // Set the global brightness for the strip
  FastLED.setBrightness(40);
  // Turn off all LEDs initially
  FastLED.clear();
  FastLED.show();
  Serial.println("Ready. Press the button to start light painting.");
}

// --- Loop Function ---
void loop() {
  static bool button_pressed_prev = false;

  // Read the current state of the button (LOW when pressed due to INPUT_PULLUP)
  bool button_pressed_now = (digitalRead(BUTTON_PIN) == LOW);

  // Detect a button press (rising edge)
  if (button_pressed_now && !button_pressed_prev) {
    Serial.println("Button Pressed! Starting light painting sequence.");

    // Calculate delay per column to display the entire image in 5 seconds
    // bmp_w is the width of the BMP image, which is the total number of columns
    int delay_per_column_ms = 5000 / bmp_w; // 5000 milliseconds for 5 seconds

    Serial.printf("Delay per column: %dms\n", delay_per_column_ms);

    // Iterate through each column of the BMP image
    for (int current_column = 0; current_column < bmp_w; current_column++) {
      Serial.printf("Displaying column %d of %d\n", current_column, bmp_w);
      drawColumn(current_column); // Draw the current column on the LED strip
      delay(delay_per_column_ms); // Pause for the calculated duration
    }

    // Clear the strip after displaying the entire image
    FastLED.clear();
    FastLED.show();
    Serial.println("Image display complete. Clearing strip. Waiting for next button press.");
    delay(500); // Short delay before allowing another button press
  }
  
  // Update the previous button state for the next loop iteration
  button_pressed_prev = button_pressed_now;
}
