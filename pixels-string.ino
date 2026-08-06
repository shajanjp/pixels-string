#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <Preferences.h>

#include "config.h"
#include "dashboard_html.h"

// Module headers (single translation unit; globals are defined below).
// The Effect enum, MAX_LEDS and all shared globals are declared in globals.h.
#include "globals.h"
#include "effects.h"
#include "mcp.h"
#include "rest_api.h"

int numLeds = 50;              // actual number, read from NVS

#define PIN         2
#define BUTTON_PIN  9

Effect currentEffect = FIREFLIES;
int currentVariation = 0;
const int variationsCount[NUM_EFFECTS] = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,5,5,5,5,5,5,5,5};

bool powerOn = true;
uint8_t globalBrightness = 255;   // 0-255, applied globally via strip.setBrightness()
neoPixelType pixelOrder = NEO_RGB; // current color order, persisted in NVS

// ===========================  BUTTON STATE ===========================
bool buttonState = HIGH;
int  lastReading = HIGH;
unsigned long buttonPressTime = 0, lastDebounceTime = 0, lastClickTime = 0;
bool longPressTriggered = false;
bool singleClickPending = false;
unsigned long singleClickTime = 0;

// ===========================  WIFI RECONNECT ===========================
bool wifiConnected = false;
bool ipConfigured  = false;
unsigned long lastReconnectAttempt = 0;
#define RECONNECT_INTERVAL_MS 10000

#define DEBOUNCE_MS       50
#define LONG_PRESS_MS     2000
#define DOUBLE_CLICK_MS   500

Adafruit_NeoPixel strip(MAX_LEDS, PIN, NEO_RGB + NEO_KHZ800);
Preferences prefs;
// ===========================  UTILITY FUNCTIONS ===========================

uint32_t wheel(byte wheelPos) {
  wheelPos = 255 - wheelPos;
  if (wheelPos < 85) return strip.Color(255 - wheelPos*3, 0, wheelPos*3);
  if (wheelPos < 170) { wheelPos -= 85; return strip.Color(0, wheelPos*3, 255 - wheelPos*3); }
  wheelPos -= 170; return strip.Color(wheelPos*3, 255 - wheelPos*3, 0);
}

String getParam(const String& url, const String& param) {
  int start = url.indexOf(param + "=");
  if (start < 0) return "";
  start += param.length() + 1;
  int end = url.indexOf('&', start);
  if (end < 0) end = url.indexOf(' ', start);
  if (end < 0) end = url.length();
  return url.substring(start, end);
}

// Global brightness 0 (off) .. 255 (max). Applied at the hardware level via
// strip.setBrightness(), so it scales every effect on show(). Note: Adafruit's
// setBrightness(0) means "no scaling" (full brightness), so 0 is mapped to 1
// to actually turn the LEDs off.
void setGlobalBrightness(uint8_t b) {
  globalBrightness = constrain(b, 0, 255);
  strip.setBrightness(globalBrightness == 0 ? 1 : globalBrightness);
}

// Human-readable name for a NEO_* color order constant.
String colorOrderName(neoPixelType t) {
  switch (t) {
    case NEO_RGB: return "RGB";
    case NEO_RBG: return "RBG";
    case NEO_GRB: return "GRB";
    case NEO_GBR: return "GBR";
    case NEO_BRG: return "BRG";
    case NEO_BGR: return "BGR";
    default: return "RGB";
  }
}

// Parse a color order from its name (case-insensitive, e.g. "GRB").
// Returns false for unknown names.
bool parseColorOrder(const String& name, neoPixelType& out) {
  const neoPixelType orders[6] = {NEO_RGB, NEO_RBG, NEO_GRB,
                                  NEO_GBR, NEO_BRG, NEO_BGR};
  for (int i = 0; i < 6; i++) {
    if (name.equalsIgnoreCase(colorOrderName(orders[i]))) {
      out = orders[i];
      return true;
    }
  }
  return false;
}

// Apply a color order to the strip and persist it in NVS.
// Takes effect immediately on the next show(): setPixelColor() routes each
// logical RGB value through the order's channel offsets. Existing static
// patterns need no conversion because the packed buffer is always logical RGB.
void setColorOrder(neoPixelType t) {
  if (t == pixelOrder) return;
  pixelOrder = t;
  strip.updateType(t + NEO_KHZ800);
  prefs.putInt("colorOrder", (int)t);
}

String effectName(Effect e) {
  switch (e) {
    case FIREFLIES: return "FIREFLIES";
    case RAINBOW_SWIPE: return "RAINBOW_SWIPE";
    case AURORA: return "AURORA";
    case COMET: return "COMET";
    case CHASING_DOTS: return "CHASING_DOTS";
    case CYLON: return "CYLON";
    case DUAL_COMET: return "DUAL_COMET";
    case SPARKLE_SWEEP: return "SPARKLE_SWEEP";
    case POLICE: return "POLICE";
    case PLASMA: return "PLASMA";
    case RAINBOW_GRADIENT: return "RAINBOW_GRADIENT";
    case PULSE_WAVE: return "PULSE_WAVE";
    case SINGLE_RUNNER: return "SINGLE_RUNNER";
    case AUDIO_VISUALIZER: return "AUDIO_VISUALIZER";
    case HEARTBEAT: return "HEARTBEAT";
    case STATIC_PIXEL: return "STATIC_PIXEL";
    case TWINKLE: return "TWINKLE";
    case FIRE_FLICKER: return "FIRE_FLICKER";
    case BOUNCING_BALLS: return "BOUNCING_BALLS";
    case LIGHTNING_STORM: return "LIGHTNING_STORM";
    case KALEIDOSCOPE: return "KALEIDOSCOPE";
    case COLLIDING_FILL: return "COLLIDING_FILL";
    case PAINT_SPLAT: return "PAINT_SPLAT";
    case SNAKE: return "SNAKE";
    default: return "UNKNOWN";
  }
}

Effect effectFromName(const String& name) {
  for (int i = 0; i < NUM_EFFECTS; i++)
    if (name.equalsIgnoreCase(effectName((Effect)i)))
      return (Effect)i;
  return currentEffect;
}

// ===========================  BUTTON HANDLER ===========================

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        buttonPressTime = millis();
        longPressTriggered = false;
      } else {
        if (!longPressTriggered) {
          if (millis() - lastClickTime < DOUBLE_CLICK_MS && lastClickTime != 0) {
            singleClickPending = false;
            currentVariation = (currentVariation + 1) % variationsCount[currentEffect];
            lastClickTime = 0;
          } else {
            singleClickPending = true;
            singleClickTime = millis();
            lastClickTime = millis();
          }
        }
      }
    }
    if (buttonState == LOW && !longPressTriggered && (millis() - buttonPressTime >= LONG_PRESS_MS)) {
      longPressTriggered = true;
      singleClickPending = false;
      powerOn = !powerOn;
    }
  }
  lastReading = reading;
}

WiFiServer server(80);

// ===========================  WIFI / HTTP ===========================

void configureStaticIP() {
  IPAddress local_IP = STATIC_IP;
  IPAddress gateway  = GATEWAY;
  IPAddress subnet   = SUBNET;

  Serial.println("[WiFi] Setting static IP...");

  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);

  if (WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("[WiFi] Static IP configured.");
    ipConfigured = true;
  } else {
    Serial.println("[WiFi] ERROR: Could not set static IP.");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  if (!ipConfigured) {
    configureStaticIP();
  }

  Serial.printf("[WiFi] Connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString());
    wifiConnected = true;
    if (!MDNS.begin(MDNS_HOSTNAME)) {
      Serial.println("[mDNS] Failed \u2013 use IP directly.");
    }
    server.begin();
  } else {
    Serial.printf("[WiFi] Failed (status %d)\n", WiFi.status());
    wifiConnected = false;
  }
}

// ===========================  SETUP ===========================
void setup() {
  strip.begin(); strip.clear(); strip.show();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  randomSeed(esp_random());

  prefs.begin("neopixel", false);
  numLeds = prefs.getInt("ledCount", 50);
  if (numLeds < 1)  numLeds = 1;
  if (numLeds > MAX_LEDS) numLeds = MAX_LEDS;
  strip.updateLength(numLeds);   // apply the stored length

  // Global brightness (0-255), persisted in NVS
  int storedBrightness = prefs.getInt("brightness", 255);
  setGlobalBrightness((uint8_t)constrain(storedBrightness, 0, 255));

  // Color order (RGB/GRB/BGR/...), persisted in NVS. Fixed by the physical
  // wiring of the strip, so only meaningful when swapping strips.
  int storedOrder = prefs.getInt("colorOrder", NEO_RGB);
  if (storedOrder != NEO_RBG && storedOrder != NEO_GRB && storedOrder != NEO_GBR &&
      storedOrder != NEO_BRG && storedOrder != NEO_BGR) {
    storedOrder = NEO_RGB;
  }
  pixelOrder = (neoPixelType)storedOrder;
  strip.updateType(pixelOrder + NEO_KHZ800);

  initAllEffects();

  // General
  currentVariation=0; singleClickPending=false;

  connectWiFi();
}

// ===========================  LOOP ===========================
void loop() {
  handleButton();

  if (singleClickPending && (millis() - singleClickTime > DOUBLE_CLICK_MS)) {
    singleClickPending = false;
    currentEffect = (Effect)((currentEffect + 1) % NUM_EFFECTS);
    currentVariation = 0;
  }

  if (!powerOn) {
    strip.clear();
    strip.show();
  } else {
    renderEffect();
  }

  if (wifiConnected) {
    handleClient();
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("[WiFi] Connection lost. Will try to reconnect.");
    }
  }

  if (!wifiConnected && millis() - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = millis();
    connectWiFi();
  }

  // Run the SINGLE_RUNNER effect at high speed: only a 10ms frame delay.
  // Frames are then limited by strip.show() + loop overhead + a 10ms pause.
  delay(powerOn && currentEffect == SINGLE_RUNNER ? 15 : 20);
}

