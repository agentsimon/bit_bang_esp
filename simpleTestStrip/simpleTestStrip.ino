// NeoPixel Ring simple sketch (c) 2013 Shae Erisson
// Released under the GPLv3 license to match the rest of the
// Adafruit NeoPixel library

#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
 #include <avr/power.h> // Required for 16 MHz Adafruit Trinket
#endif

// Which pin on the Arduino is connected to the NeoPixels?
#define PIN        6// On Trinket or Gemma, suggest changing this to 1

// How many NeoPixels are attached to the Arduino?
#define NUMPIXELS 50 // Changed to 145 to light LEDs from 0 to 144

// When setting up the NeoPixel library, we tell it how many pixels,
// and which pin to use to send signals. Note that for older NeoPixel
// pixels you might need to change the third parameter -- see the
// strandtest example for more information on possible values.
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB);


#define DELAYVAL 50 // Time (in milliseconds) to pause between pixels. Adjusted for smoother animation.

void setup() {
  // These lines are specifically to support the Adafruit Trinket 5V 16 MHz.
  // Any other board, you can remove this part (but no harm leaving it):
#if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
  clock_prescale_set(clock_div_1);
#endif
  // END of Trinket-specific code.

  pixels.begin(); // INITIALIZE NeoPixel pixels object (REQUIRED)
}

void loop() {
  // Loop to light one LED at a time
  for(int i=0; i < NUMPIXELS; i++) {
    // Turn on the current pixel (you can change the color)
    pixels.setPixelColor(i, pixels.Color(255, 0, 0)); // Red light, full brightness
    pixels.show(); // Update the strip with the new pixel state
    delay(DELAYVAL); // Pause for a moment

    // Turn off the current pixel
    pixels.setPixelColor(i, pixels.Color(0, 0, 0)); // Turn off
    pixels.show(); // Update the strip
  }
  for(int i=0; i < NUMPIXELS; i++) {
    // Turn on the current pixel (you can change the color)
    pixels.setPixelColor(i, pixels.Color(0, 255, 0)); // Green light, full brightness
    pixels.show(); // Update the strip with the new pixel state
    delay(DELAYVAL); // Pause for a moment

    // Turn off the current pixel
    pixels.setPixelColor(i, pixels.Color(0, 0, 0)); // Turn off
    pixels.show(); // Update the strip
  }
  for(int i=0; i < NUMPIXELS; i++) {
    // Turn on the current pixel (you can change the color)
    pixels.setPixelColor(i, pixels.Color(0, 0, 255)); // Blue light, full brightness
    pixels.show(); // Update the strip with the new pixel state
    delay(DELAYVAL); // Pause for a moment

    // Turn off the current pixel
    pixels.setPixelColor(i, pixels.Color(0, 0, 0)); // Turn off
    pixels.show(); // Update the strip
  }
  for(int i=0; i < NUMPIXELS; i++) {
    // Turn on the current pixel (you can change the color)
    pixels.setPixelColor(i, pixels.Color(255, 255, 255)); // White light, full brightness
    pixels.show(); // Update the strip with the new pixel state
    delay(DELAYVAL); // Pause for a moment

    // Turn off the current pixel
    pixels.setPixelColor(i, pixels.Color(0, 0, 0)); // Turn off
    pixels.show(); // Update the strip
  }
}
