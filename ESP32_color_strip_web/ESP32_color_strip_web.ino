#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <LittleFS.h>

// --- WiFi Configuration (Access Point Mode) ---
const char* ssid = "NeoPixel_Controller";
const char* password = "123456789";

// --- Web Server Object ---
AsyncWebServer server(80);

// --- NeoPixel Configuration for FastLED ---
const int LED_PIN = 4;
const int NUM_LEDS = 99;
const int NUM_SEGMENTS = 6;

#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

// --- NeoPixel Segment Definitions ---
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19};
int segmentStartPixel[NUM_SEGMENTS];
int segmentEndPixel[NUM_SEGMENTS];
CRGB currentSegmentColors[NUM_SEGMENTS];

// --- Per-Segment Flashing Variables ---
bool segmentIsFlashing[NUM_SEGMENTS] = {false};
unsigned long segmentFlashIntervalMillis[NUM_SEGMENTS];
unsigned long segmentLastFlashTime[NUM_SEGMENTS];
bool segmentCurrentFlashState[NUM_SEGMENTS];

// --- Global variable for overall brightness ---
uint8_t currentBrightness = 153;

// --- Physical Button Configuration ---
const int BUTTON_PIN = 13;
volatile bool buttonToggleRequested = false;
volatile unsigned long lastButtonPressMillis = 0;
const unsigned long debounceDelay = 200;

// --- Global Strip State Variables ---
// Mode 0: OFF
// Mode 1: ON (Static/Segment Flashing/Random)
// Mode 2: ON (Global ON/OFF Cycling)
int currentStripMode = 0;

// --- Global ON/OFF Cycle Variables ---
unsigned long globalOnDurationMillis = 20000;
unsigned long globalOffDurationMillis = 2000;
unsigned long globalCycleLastToggleTime = 0;
bool globalCycleIsOnPhase = false;

// --- Per-Segment Random Mode Variables ---
bool segmentIsRandom[NUM_SEGMENTS] = {false};
unsigned long segmentRandomSpeedMillis[NUM_SEGMENTS];
unsigned long segmentLastRandomChangeTime[NUM_SEGMENTS];

// --- Helper function to recalculate segment pixel ranges ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        int effectiveLength = max(0, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;

        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1);
        if (currentPixel >= NUM_LEDS || effectiveLength == 0) {
            segmentStartPixel[i] = currentPixel;
            segmentEndPixel[i] = currentPixel - 1;
        }
        currentPixel = segmentEndPixel[i] + 1;
    }
}

// --- Helper function to convert hex string to CRGB color ---
CRGB colorFromHexString(const char* hexString) {
    if (strlen(hexString) != 6) {
        return CRGB::Black;
    }
    unsigned long hexValue = strtol(hexString, NULL, 16);
    uint8_t r = (hexValue >> 16) & 0xFF;
    uint8_t g = (hexValue >> 8) & 0xFF;
    uint8_t b = hexValue & 0xFF;
    return CRGB(r, g, b);
}

// --- Function to build the LED buffer based on current segment states ---
void updateLedBuffer() {
    FastLED.clear();
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
      if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
        for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
          if (i < NUM_LEDS) {
            if (segmentIsFlashing[seg] && !segmentCurrentFlashState[seg]) {
              leds[i] = CRGB::Black;
            } else {
              leds[i] = currentSegmentColors[seg];
            }
          }
        }
      }
    }
}

// --- Interrupt Service Routine (ISR) for the Button ---
void IRAM_ATTR handleButtonPress() {
    if (millis() - lastButtonPressMillis > debounceDelay) {
        buttonToggleRequested = true;
        lastButtonPressMillis = millis();
    }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- ESP32 NeoPixel Controller Boot ---");

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(currentBrightness);
  updateSegmentPixels();

  randomSeed(analogRead(A0));

  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black;
      segmentLastFlashTime[i] = millis();
      segmentCurrentFlashState[i] = true;
      segmentFlashIntervalMillis[i] = 1000;
      segmentRandomSpeedMillis[i] = 1000;
      segmentLastRandomChangeTime[i] = 0;
  }
  FastLED.clear();
  FastLED.show();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);

  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP IP address: %s\n", IP.toString().c_str());

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed or Formatted! Cannot continue without filesystem.");
    while(true);
  } else {
    Serial.println("LittleFS mounted successfully.");
  }

  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
    } else {
        request->send(404, "text/plain", "index.html not found on filesystem!");
    }
  });

  server.on("/setBrightness", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasArg("value")) {
      int brightnessPct = request->arg("value").toInt();
      currentBrightness = map(brightnessPct, 0, 100, 0, 255);
      FastLED.setBrightness(currentBrightness);
      FastLED.show();
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });

  server.on("/setAllConfig", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Received /setAllConfig command.");
    
    // Parse Segment Data
    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        CRGB newColor = colorFromHexString(request->arg(paramColorName).c_str());
        currentSegmentColors[i] = newColor;
      }

      String paramLengthName = "l" + String(i);
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        if (newLength < 0) newLength = 0;
        if (newLength > NUM_LEDS) newLength = NUM_LEDS;
        segmentLengths[i] = newLength;
      }

      String paramFlashName = "f" + String(i);
      if (request->hasArg(paramFlashName)) {
          bool newIsFlashing = (request->arg(paramFlashName) == "true");
          if (newIsFlashing != segmentIsFlashing[i]) {
              segmentIsFlashing[i] = newIsFlashing;
              segmentCurrentFlashState[i] = true;
              segmentLastFlashTime[i] = millis();
          }
      }

      String paramFlashRateName = "fr" + String(i);
      if (request->hasArg(paramFlashRateName)) {
          float newFlashRateSeconds = request->arg(paramFlashRateName).toFloat();
          if (newFlashRateSeconds < 0.25) newFlashRateSeconds = 0.25;
          if (newFlashRateSeconds > 2.0) newFlashRateSeconds = 2.0;
          unsigned long newFlashRateMillis = (unsigned long)(newFlashRateSeconds * 1000.0);
          if (newFlashRateMillis != segmentFlashIntervalMillis[i]) {
              segmentFlashIntervalMillis[i] = newFlashRateMillis;
              segmentLastFlashTime[i] = millis();
              segmentCurrentFlashState[i] = true;
          }
      }
      
      String paramRandomName = "r" + String(i);
      if (request->hasArg(paramRandomName)) {
          segmentIsRandom[i] = (request->arg(paramRandomName) == "true");
      }
      
      String paramRandomSpeedName = "rs" + String(i);
      if (request->hasArg(paramRandomSpeedName)) {
          float newSpeed = request->arg(paramRandomSpeedName).toFloat();
          if (newSpeed < 0.5) newSpeed = 0.5;
          if (newSpeed > 10.0) newSpeed = 10.0;
          segmentRandomSpeedMillis[i] = (unsigned long)(1000.0 / newSpeed);
      }
    }
    
    updateSegmentPixels();
    currentStripMode = 1;
    updateLedBuffer();
    FastLED.show();

    request->send(200, "text/plain", "All configurations updated!");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    FastLED.clear();
    FastLED.show();
    currentStripMode = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentIsRandom[i] = false;
        segmentIsFlashing[i] = false;
    }
    request->send(200, "text/plain", "Lights OFF");
  });

  server.on("/setAllColor", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasArg("color")) {
        request->send(400, "text/plain", "Color value missing");
        return;
    }
    String hexColor = request->arg("color");
    CRGB newColor = colorFromHexString(hexColor.c_str());
    for (int i = 0; i < NUM_LEDS; ++i) {
      leds[i] = newColor;
    }
    if (currentStripMode == 1 || currentStripMode == 2) {
        FastLED.show();
    }
    
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
        segmentIsFlashing[i] = false;
        segmentIsRandom[i] = false;
    }
    request->send(200, "text/plain", "All LEDs colour updated");
  });
    
  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  unsigned long currentMillis = millis();

  if (buttonToggleRequested) {
    buttonToggleRequested = false;
    currentStripMode = (currentStripMode + 1) % 3; // Swapped to 3 modes.

    if (currentStripMode == 0) { // OFF
      FastLED.clear();
      FastLED.show();
      for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentIsRandom[i] = false;
      }
    } else if (currentStripMode == 1) { // ON (Static/Flashing/Random)
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis;
          segmentIsRandom[i] = false;
          segmentIsFlashing[i] = false;
      }
      updateLedBuffer();
      FastLED.show();
    } else if (currentStripMode == 2) { // Global Cycle
      globalCycleIsOnPhase = true;
      globalCycleLastToggleTime = currentMillis;
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis;
          segmentIsRandom[i] = false;
          segmentIsFlashing[i] = false;
      }
      updateLedBuffer();
      FastLED.show();
    }
  }

  // Handle Per-Segment Random Color Changes
  bool needsUpdate = false;
  for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsRandom[i] && currentMillis - segmentLastRandomChangeTime[i] >= segmentRandomSpeedMillis[i]) {
          segmentLastRandomChangeTime[i] = currentMillis;
          currentSegmentColors[i] = CHSV(random8(), 255, 255);
          needsUpdate = true;
      }
  }
  // Handle Per-Segment Flashing
  for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsFlashing[i]) {
        if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
          segmentLastFlashTime[i] = currentMillis;
          segmentCurrentFlashState[i] = !segmentCurrentFlashState[i];
          needsUpdate = true;
        }
      }
  }

  if (needsUpdate) {
    updateLedBuffer();
    FastLED.show();
  }
  
  // Handle Global ON/OFF Cycle
  if (currentStripMode == 2) {
    if (globalCycleIsOnPhase) {
      if (currentMillis - globalCycleLastToggleTime >= globalOnDurationMillis) {
        globalCycleIsOnPhase = false;
        globalCycleLastToggleTime = currentMillis;
        FastLED.clear();
        FastLED.show();
      }
    } else {
      if (currentMillis - globalCycleLastToggleTime >= globalOffDurationMillis) {
        globalCycleIsOnPhase = true;
        globalCycleLastToggleTime = currentMillis;
        updateLedBuffer();
        FastLED.show();
      }
    }
  }
}
