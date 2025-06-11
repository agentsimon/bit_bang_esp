#include <Arduino.h>
#include <LittleFS.h>
#include <FastLED.h>

// --- FastLED Configuration ---
#define NEOPIXEL_PIN    2
#define NEOPIXEL_COUNT  144
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB

CRGB leds[NEOPIXEL_COUNT];

// --- Button Configuration ---
#define BUTTON_PIN      0

// --- Analog Pin Configuration ---
#define ANALOG_PIN      A0

// --- Global BMP Variables ---
int bmp_w, bmp_h;
uint32_t bmp_offset;
uint16_t bmp_bitDepth;
int bmp_row_padding;
File current_bmpFile;

// --- Global Analog Value Variables ---
int brightnessAnalogValue = 0;
int lastMonitoredSpeedAnalogValue = 0;
long totalAnimationDurationMs = 5000;

// --- State Flag ---
bool is_animating = false;

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
bool loadBMP(const char *filename) {
  current_bmpFile = LittleFS.open(filename, "r");
  if (!current_bmpFile) {
    // Serial.println("Failed to open BMP file for reading!"); // Essential, but keeping only critical errors
    return false;
  }

  if (read16(current_bmpFile) != 0x4D42) {
    // Serial.println("Not a valid BMP file!"); // Essential, but keeping only critical errors
    current_bmpFile.close();
    return false;
  }

  read32(current_bmpFile); // File size (unused)
  read32(current_bmpFile); // Reserved (unused)
  bmp_offset = read32(current_bmpFile); // Data offset

  read32(current_bmpFile); // Header size (unused)
  bmp_w = read32(current_bmpFile);  // Image width
  bmp_h = read32(current_bmpFile);  // Image height
  read16(current_bmpFile); // Color planes (unused)
  bmp_bitDepth = read16(current_bmpFile); // Bits per pixel

  // Serial.printf("BMP Info: Width=%d, Height=%d, Bit Depth=%d, Offset=%d\n", bmp_w, bmp_h, bmp_bitDepth, bmp_offset); // Removed for quiet operation

  if (bmp_bitDepth != 24 && bmp_bitDepth != 32) {
    // Serial.println("Unsupported BMP bit depth (only 24-bit or 32-bit uncompressed BMPs are supported)."); // Essential, but keeping only critical errors
    current_bmpFile.close();
    return false;
  }

  bmp_row_padding = (4 - (bmp_w * (bmp_bitDepth / 8)) % 4) % 4;

  // Serial.println("BMP header loaded successfully."); // Removed for quiet operation
  return true;
}

// --- Function to Draw a Specific Column of the BMP ---
void drawColumn(int col_x) {
  if (!current_bmpFile) {
    // Serial.println("Error: BMP file not open for drawing column."); // Removed for quiet operation
    return;
  }
  if (col_x >= bmp_w || col_x < 0) {
    // Serial.printf("Column %d out of image bounds (width %d).\n", col_x, bmp_w); // Removed for quiet operation
    return;
  }

  FastLED.clear();

  uint8_t r, g, b;
  uint32_t bytes_per_pixel = (bmp_bitDepth / 8);
  uint32_t total_bytes_per_row = (bmp_w * bytes_per_pixel) + bmp_row_padding;

  for (int y_img = 0; y_img < bmp_h; y_img++) {
    uint32_t pixel_byte_offset = bmp_offset + (y_img * total_bytes_per_row) + (col_x * bytes_per_pixel);
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

    if (y_img < NEOPIXEL_COUNT) {
      leds[y_img] = CRGB(r, g, b);
      yield();
    }
  }
  FastLED.show();
}


// --- Setup Function ---
void setup() {
  Serial.begin(115200); // KEEP: Essential for initial boot feedback if needed
  delay(1000); // Give time for serial monitor to connect

  // Serial.println("\nNodeMCU v1 FastLED BMP Light Painter (Analog Controlled)"); // Removed for quiet operation

#if defined(ESP32)
  if(!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)){
#elif defined(ESP8266)
  if(!LittleFS.begin()){
    Serial.println(F("LittleFS mount failed. Attempting to format...")); // KEEP: Critical error
    if (LittleFS.format()) {
      Serial.println(F("LittleFS formatted successfully. Re-mounting...")); // KEEP: Critical
      if (!LittleFS.begin()) {
        Serial.println(F("LittleFS re-mount failed. Halting...")); // KEEP: Critical error
        while(true);
      }
    } else {
      Serial.println(F("LittleFS format failed. Halting...")); // KEEP: Critical error
      while(true);
    }
  }
#else
    if (!LittleFS.begin()) {
        Serial.println(F("LittleFS mount failed. Halting...")); // KEEP: Critical error
        while(true);
    }
#endif
  // Serial.println("LittleFS Mounted Successfully."); // Removed for quiet operation

  if (!loadBMP("frame000.bmp")) {
    Serial.println("Failed to load BMP. Halting."); // KEEP: Critical error
    while (true);
  }

  if (bmp_w <= 0) {
    Serial.println("Error: BMP width is invalid (<=0). Halting."); // KEEP: Critical error
    while(true);
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ANALOG_PIN, INPUT);

  FastLED.addLeds<LED_TYPE, NEOPIXEL_PIN, COLOR_ORDER>(leds, NEOPIXEL_COUNT);
  FastLED.clear();
  FastLED.show();


  brightnessAnalogValue = analogRead(ANALOG_PIN);
  int brightness = map(brightnessAnalogValue, 0, 1023, 10, 255);
  FastLED.setBrightness(brightness);
  // Serial.printf("A0 value for brightness (startup): %d, setting brightness to: %d\n", brightnessAnalogValue, brightness); // Removed for quiet operation

  lastMonitoredSpeedAnalogValue = analogRead(ANALOG_PIN);
  totalAnimationDurationMs = map(lastMonitoredSpeedAnalogValue, 0, 1023, 2000, 8000);
  // Serial.printf("Initial A0 value for total animation duration (startup): %d, initial total duration: %ldms\n", lastMonitoredSpeedAnalogValue, totalAnimationDurationMs); // Removed for quiet operation

  // Serial.println("Ready. Press the button to start light painting."); // Removed for quiet operation
}

// --- Loop Function ---
void loop() {
  static bool button_pressed_prev = false;

  if (!is_animating) {
    int currentAnalogReading = analogRead(ANALOG_PIN);
    if (abs(currentAnalogReading - lastMonitoredSpeedAnalogValue) > 1) {
      lastMonitoredSpeedAnalogValue = currentAnalogReading;
      totalAnimationDurationMs = map(lastMonitoredSpeedAnalogValue, 0, 1023, 2000, 8000);
      // Serial.printf("A0 value for total duration changed to: %d, new total duration: %ldms\n", lastMonitoredSpeedAnalogValue, totalAnimationDurationMs); // Removed for quiet operation
    }
  }

  bool button_pressed_now = (digitalRead(BUTTON_PIN) == LOW);

  if (button_pressed_now && !button_pressed_prev && !is_animating) {
    is_animating = true;

    long delayPerColumnMs = 0;
    if (bmp_w > 0) {
      delayPerColumnMs = totalAnimationDurationMs / bmp_w;
      if (delayPerColumnMs < 1) delayPerColumnMs = 1;
    } else {
        // Serial.println("Error: BMP width is zero, cannot calculate column delay."); // Removed (already handled by setup halt)
    }

    // Serial.println("Button Pressed! Starting light painting sequence."); // Removed for quiet operation
    // Serial.printf("Animation will take ~%ldms total (A0 value: %d). Delay per column: %ldms\n",
    //               totalAnimationDurationMs, lastMonitoredSpeedAnalogValue, delayPerColumnMs); // Removed for quiet operation

    for (int current_column = 0; current_column < bmp_w; current_column++) {
      // Serial.printf("Displaying column %d of %d\n", current_column + 1, bmp_w); // Removed for quiet operation
      drawColumn(current_column);
      yield();
      delay(delayPerColumnMs);
    }

    FastLED.clear();
    FastLED.show();
    // Serial.println("Image display complete. Clearing strip."); // Removed for quiet operation
    delay(500);

    is_animating = false;
    // Serial.println("Waiting for next button press. A0 monitoring for speed resumed."); // Removed for quiet operation
  }

  button_pressed_prev = button_pressed_now;
}
