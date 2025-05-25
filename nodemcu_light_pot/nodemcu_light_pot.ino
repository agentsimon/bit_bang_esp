#include <Arduino.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>

// --- NeoPixel Configuration ---
#define NEOPIXEL_PIN    D4
#define NEOPIXEL_COUNT  144 // Set this to the ACTUAL number of LEDs on your strip
#define NEOPIXEL_TYPE   NEO_GRB + NEO_KHZ800

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEOPIXEL_TYPE);

// --- Button Configuration ---
#define BUTTON_PIN      D3 // Digital pin connected to the button

// --- Potentiometer Configuration ---
#define POT_PIN         A0 // Analog pin connected to the potentiometer's wiper (middle pin)
#define MIN_TOTAL_PAINT_TIME_MS 1000 // Minimum total time for painting sequence (1 second)
#define MAX_TOTAL_PAINT_TIME_MS 30000 // Maximum total time for painting sequence (30 seconds)


// --- Global BMP Variables ---
// These will store the image dimensions and other info after loading the header
int bmp_w, bmp_h;
uint32_t bmp_offset;
uint16_t bmp_bitDepth;
int bmp_row_padding;
File current_bmpFile; // Keep the file object global and open

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
// This runs once to get the image dimensions etc.
bool loadBMP(const char *filename) {
  current_bmpFile = LittleFS.open(filename, "r");
  if (!current_bmpFile) {
    Serial.println("Failed to open BMP file for reading!");
    return false;
  }

  // Read BMP header (14 bytes)
  if (read16(current_bmpFile) != 0x4D42) { // 'BM' signature
    Serial.println("Not a valid BMP file!");
    current_bmpFile.close();
    return false;
  }
  read32(current_bmpFile); // Skip file size
  read32(current_bmpFile); // Skip reserved fields
  bmp_offset = read32(current_bmpFile); // Offset to pixel data

  // Read DIB header (40 bytes for BITMAPINFOHEADER)
  read32(current_bmpFile); // Skip DIB header size
  bmp_w = read32(current_bmpFile); // Image width
  bmp_h = read32(current_bmpFile); // Image height
  read16(current_bmpFile); // Skip color planes
  bmp_bitDepth = read16(current_bmpFile); // Bits per pixel

  Serial.printf("BMP Info: Width=%d, Height=%d, Bit Depth=%d, Offset=%d\n", bmp_w, bmp_h, bmp_bitDepth, bmp_offset);

  if (bmp_bitDepth != 24 && bmp_bitDepth != 32) {
    Serial.println("Unsupported BMP bit depth (only 24-bit or 32-bit uncompressed BMPs are supported).");
    current_bmpFile.close();
    return false;
  }

  bmp_row_padding = (4 - (bmp_w * (bmp_bitDepth / 8)) % 4) % 4;

  Serial.println("BMP header loaded successfully.");
  return true;
}

// --- Function to Draw a Specific Column of the BMP ---
// This is the core "light painting" function
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    Serial.println("Error: BMP file not open for drawing column.");
    return;
  }
  if (col_x >= bmp_w || col_x < 0) { // Ensure column is within image bounds
    Serial.printf("Column %d out of image bounds (width %d).\n", col_x, bmp_w);
    return;
  }

  strip.clear(); // Clear the strip before drawing the new column

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  // Iterate through image rows to get pixels for the current column (col_x)
  // BMPs store pixels from bottom-up, so image row 0 is the bottom-most row.
  // We'll map image row 0 to strip pixel 0, image row 1 to strip pixel 1, etc.
  for (int y_img = 0; y_img < bmp_h; y_img++) {
    // Calculate the exact file position for the pixel (col_x, y_img)
    // pixel_byte_offset = (start of pixel data) + (offset to row y) + (offset to col x within row)
    uint32_t pixel_byte_offset = bmp_offset + (y_img * total_bytes_per_row) + (col_x * bytes_per_pixel);

    // Seek to that exact pixel's location in the file
    current_bmpFile.seek(pixel_byte_offset);

    // Read pixel components (BMP stores in BGR order)
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
        // This case should ideally not happen if loadBMP validates bitDepth
        r = g = b = 0; // Default to black
    }

    // Set the pixel on the NeoPixel strip.
    // Ensure we don't try to set pixels beyond the length of our physical strip.
    // If NEOPIXEL_COUNT < bmp_h, the image will be cropped from the top.
    if (y_img < NEOPIXEL_COUNT) {
      strip.setPixelColor(y_img, strip.Color(r, g, b));
    }
  }
  strip.show(); // Update the physical NeoPixel strip
}


// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  Serial.println("\nESP8266 NeoPixel BMP Light Painter (Button Triggered)");

  if (!LittleFS.begin()) {
    Serial.println("LittleFS Mount Failed! Please ensure files are uploaded to LittleFS.");
    while (true); // Halt if LittleFS fails
  }
  Serial.println("LittleFS Mounted Successfully.");

  // Load BMP header information once
  if (!loadBMP("frame000.bmp")) {
    Serial.println("Failed to load BMP. Halting.");
    while (true); // Halt if BMP loading fails
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP); // Initialize the button pin with internal pull-up

  strip.begin();
  strip.show();
  strip.setBrightness(50); // Adjust brightness (0-255) as needed
  Serial.println("Ready. Press the button to start light painting.");
}

// --- Loop Function ---
void loop() {
  static bool button_pressed_prev = false; // For debouncing

  bool button_pressed_now = (digitalRead(BUTTON_PIN) == LOW); // Button is LOW when pressed

  // Detect a transition from not pressed to pressed
  if (button_pressed_now && !button_pressed_prev) {
    Serial.println("Button Pressed! Starting light painting sequence.");

    // Read potentiometer value (0-1023)
    int pot_value = analogRead(POT_PIN);
    // Map potentiometer value to desired total painting time (e.g., 1 to 30 seconds)
    long total_paint_time_ms = map(pot_value, 0, 1023, MIN_TOTAL_PAINT_TIME_MS, MAX_TOTAL_PAINT_TIME_MS);

    // Calculate delay per column based on mapped total time
    // Ensure bmp_w is not zero to avoid division by zero
    int delay_per_column_ms = (bmp_w > 0) ? (total_paint_time_ms / bmp_w) : 10; // Default to 10ms if width is zero
    // Ensure minimum delay to prevent freezing or too fast updates
    if (delay_per_column_ms < 1) delay_per_column_ms = 1;


    Serial.printf("Potentiometer value: %d, Total painting time: %ldms, Delay per column: %dms\n", 
                  pot_value, total_paint_time_ms, delay_per_column_ms);

    // Iterate through all columns of the image, ensuring the loop completes
    for (int current_column = 0; current_column < bmp_w; current_column++) {
      Serial.printf("Displaying column %d of %d\n", current_column, bmp_w);
      drawColumn(current_column); // Draw the current column on the strip
      delay(delay_per_column_ms); // Wait for the calculated time
    }

    // After displaying all columns, clear the strip and wait for the next button press
    strip.clear();
    strip.show();
    Serial.println("Image display complete. Clearing strip. Waiting for next button press.");
    delay(500); // Short delay after clearing to debounce the button release
  }
  
  button_pressed_prev = button_pressed_now; // Update button state for next loop iteration
}
