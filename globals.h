// ====================================================================
//  globals.h - shared declarations for the pixels-string sketch
//
//  All sketch code compiles into a single translation unit: pixels-
//  string.ino includes globals.h, effects.h, mcp.h and rest_api.h and
//  owns the actual definitions of the globals declared here (see the
//  comment blocks in the .ino). Headers use these externs + prototypes
//  so the sections can be edited in isolation.
// ====================================================================
#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>

#define MAX_LEDS  300          // upper hardware/software limit

// ===========================  EFFECTS ===========================
enum Effect {
  FIREFLIES = 0,
  RAINBOW_SWIPE,
  AURORA,
  COMET,
  CHASING_DOTS,
  CYLON,
  DUAL_COMET,
  SPARKLE_SWEEP,
  POLICE,
  PLASMA,
  RAINBOW_GRADIENT,
  PULSE_WAVE,
  SINGLE_RUNNER,
  AUDIO_VISUALIZER,
  HEARTBEAT,
  STATIC_PIXEL,
  TWINKLE,
  FIRE_FLICKER,
  BOUNCING_BALLS,
  LIGHTNING_STORM,
  KALEIDOSCOPE,
  COLLIDING_FILL,
  PAINT_SPLAT,
  SNAKE,
  NUM_EFFECTS
};

// ---- shared state (defined in pixels-string.ino) ----
extern int numLeds;
extern Adafruit_NeoPixel strip;
extern Preferences prefs;
extern WiFiServer server;

extern Effect currentEffect;
extern int currentVariation;
extern const int variationsCount[NUM_EFFECTS];
extern bool powerOn;
extern uint8_t globalBrightness;
extern neoPixelType pixelOrder;

// ---- shared utilities (defined in pixels-string.ino) ----
uint32_t wheel(byte wheelPos);
String getParam(const String& url, const String& param);
void setGlobalBrightness(uint8_t b);
String colorOrderName(neoPixelType t);
bool parseColorOrder(const String& name, neoPixelType& out);
void setColorOrder(neoPixelType t);
String effectName(Effect e);
Effect effectFromName(const String& name);

// ---- static pixel pattern API (defined in effects.h) ----
extern uint32_t* staticPixelBuffer;
void freeStaticPixelBuffer();
uint32_t parseHexColor(const String& hex);
uint32_t lerpColor(uint32_t c1, uint32_t c2, float t);
void applyBrightnessToBuffer(uint8_t brightness);
void applyPercentageToBuffer(int percentage);
void generateSolidPattern(const String& colorStr);
void generateStripedPattern(const String colors[], int numColors);
void generateGradientPattern(const String colors[], int numColors);
void updateStaticPixel();
bool isValidHexColor(const String& hex);

// ---- animation lifecycle (defined in effects.h) ----
void initAllEffects();
void renderEffect();
void resetEffectState();

// ---- server handlers (defined in mcp.h / rest_api.h) ----
void handleMCP(WiFiClient& client, const String& body);
void handleClient();
