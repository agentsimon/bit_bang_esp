#include <FastLED.h>

// Which pin on the Arduino is connected to the NeoPixels?
#define LED_PIN     6

// How many NeoPixels are attached to the Arduino?
#define NUM_LEDS    50

// Define the array of leds
CRGB leds[NUM_LEDS];

#define DELAYVAL 50 // Time (in milliseconds) to pause between pixels.

void setup() {
  // Initialize FastLED
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255); // Set global brightness (0-255)
}

void loop() {
  // Loop to light one LED at a time with Red
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Red; // Set current pixel to Red
    FastLED.show();      // Update the strip
    delay(DELAYVAL);     // Pause for a moment
    leds[i] = CRGB::Black; // Turn off current pixel
    FastLED.show();      // Update the strip
  }

  // Loop to light one LED at a time with Green
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Green; // Set current pixel to Green
    FastLED.show();        // Update the strip
    delay(DELAYVAL);       // Pause for a moment
    leds[i] = CRGB::Black; // Turn off current pixel
    FastLED.show();        // Update the strip
  }

  // Loop to light one LED at a time with Blue
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Blue; // Set current pixel to Blue
    FastLED.show();       // Update the strip
    delay(DELAYVAL);      // Pause for a moment
    leds[i] = CRGB::Black; // Turn off current pixel
    FastLED.show();       // Update the strip
  }

  // Loop to light one LED at a time with White
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::White; // Set current pixel to White
    FastLED.show();        // Update the strip
    delay(DELAYVAL);       // Pause for a moment
    leds[i] = CRGB::Black; // Turn off current pixel
    FastLED.show();        // Update the strip
  }
}