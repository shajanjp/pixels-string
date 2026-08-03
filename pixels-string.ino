#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <Preferences.h>

#include "config.h"
#include "dashboard_html.h"

#define MAX_LEDS  300          // upper hardware/software limit
int numLeds = 50;              // actual number, read from NVS

#define PIN         2
#define BUTTON_PIN  9

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

// Forward declarations for static pixel pattern API (defined below)
extern uint32_t* staticPixelBuffer;
void freeStaticPixelBuffer();
uint32_t parseHexColor(const String& hex);
uint32_t lerpColor(uint32_t c1, uint32_t c2, float t);
void applyBrightnessToBuffer(uint8_t brightness);
void generateSolidPattern(const String& colorStr);
void generateStripedPattern(const String colors[], int numColors);
void generateGradientPattern(const String colors[], int numColors);
void updateStaticPixel();

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

void handleClient() {
  WiFiClient client = server.accept();
  if (!client) return;

  String req = "";
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      req += c;
      if (c == '\n') break;
    }
  }

  int methodEnd = req.indexOf(' ');
  if (methodEnd < 0) { client.stop(); return; }
  String method = req.substring(0, methodEnd);
  String pathAndParams = req.substring(methodEnd + 1);
  int pathEnd = pathAndParams.indexOf(' ');
  if (pathEnd < 0) pathEnd = pathAndParams.length();
  String fullPath = pathAndParams.substring(0, pathEnd);
  int queryIndex = fullPath.indexOf('?');
  String path = (queryIndex >= 0) ? fullPath.substring(0, queryIndex) : fullPath;

  while (client.available()) client.read();

  Serial.printf("[HTTP] %s %s\n", method.c_str(), path.c_str());

  String response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";

  if (path == "/api/effect") {
    String name = getParam(fullPath, "name");
    String idx  = getParam(fullPath, "index");
    if (name.length() > 0) {
      currentEffect = effectFromName(name);
      currentVariation = 0;
      response += "Effect set to " + effectName(currentEffect) + "\n";
    } else if (idx.length() > 0) {
      int i = idx.toInt();
      if (i >= 0 && i < NUM_EFFECTS) {
        currentEffect = (Effect)i;
        currentVariation = 0;
        response += "Effect index set to " + String(i) + " (" + effectName(currentEffect) + ")\n";
      }
    }
  }
  else if (path == "/api/variation") {
    String idx = getParam(fullPath, "index");
    if (idx.length() > 0) {
      int v = idx.toInt();
      if (v >= 0 && v < variationsCount[currentEffect]) {
        currentVariation = v;
        response += "Variation set to " + String(v) + "\n";
      }
    }
  }
  else if (path == "/api/power") {
    String state = getParam(fullPath, "state");
    if (state.equalsIgnoreCase("on")) { powerOn = true; response += "Power ON\n"; }
    else if (state.equalsIgnoreCase("off")) { powerOn = false; response += "Power OFF\n"; }
  }
  else if (path == "/api/brightness") {
    String valueStr = getParam(fullPath, "value");
    if (valueStr.length() > 0) {
      bool isNumeric = true;
      for (int i = 0; i < valueStr.length(); i++)
        if (!isdigit(valueStr.charAt(i))) { isNumeric = false; break; }
      int v = valueStr.toInt();
      if (isNumeric && v >= 0 && v <= 255) {
        setGlobalBrightness((uint8_t)v);
        prefs.putInt("brightness", globalBrightness);
        response += "Brightness set to " + String(globalBrightness) + "\n";
      } else {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                   "Connection: close\r\n\r\n"
                   "Invalid brightness. Must be 0-255";
        client.print(response);
        client.stop();
        return;
      }
    } else {
      response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                 "Connection: close\r\n\r\n"
                 "Missing 'value' parameter (0-255). Current brightness is " + String(globalBrightness) + ".";
      client.print(response);
      client.stop();
      return;
    }
  }
  else if (path == "/api/info") {
    response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n";
    response += "{";
    response += "\"effect\":\"" + effectName(currentEffect) + "\",";
    response += "\"effectIndex\":" + String(currentEffect) + ",";
    response += "\"variation\":" + String(currentVariation) + ",";
    response += "\"variationMax\":" + String(variationsCount[currentEffect]-1) + ",";
    response += "\"power\":\"" + String(powerOn?"on":"off") + "\",";
    response += "\"numEffects\":" + String(NUM_EFFECTS) + ",";
    response += "\"ledCount\":" + String(numLeds) + ",";
    response += "\"brightness\":" + String(globalBrightness) + ",";
    response += "\"colorOrder\":\"" + colorOrderName(pixelOrder) + "\"";
    response += "}";
  }
  else if (path == "/config") {
    // GET /config?colorOrder=GRB&ledCount=120 -> set + persist config values
    // GET /config                              -> return current config as JSON
    String countStr = getParam(fullPath, "ledCount");
    if (countStr.length() > 0) {
      bool isNumeric = true;
      for (int i = 0; i < countStr.length(); i++)
        if (!isdigit(countStr.charAt(i))) { isNumeric = false; break; }
      int newCount = countStr.toInt();
      if (!isNumeric || newCount < 1 || newCount > MAX_LEDS) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid ledCount. Must be 1-" +
                   String(MAX_LEDS) + "}";
        client.print(response);
        client.stop();
        return;
      }
      if (newCount != numLeds) {
        numLeds = newCount;
        prefs.putInt("ledCount", numLeds);
        strip.updateLength(numLeds);
        resetEffectState();          // re-init all effect variables
      }
    }

    String orderStr = getParam(fullPath, "colorOrder");
    if (orderStr.length() > 0) {
      neoPixelType order;
      bool valid = false;
      bool isNumeric = true;
      for (int i = 0; i < orderStr.length(); i++)
        if (!isdigit(orderStr.charAt(i))) { isNumeric = false; break; }
      if (isNumeric) {
        int v = orderStr.toInt();
        if (v == NEO_RGB || v == NEO_RBG || v == NEO_GRB || v == NEO_GBR ||
            v == NEO_BRG || v == NEO_BGR) {
          order = (neoPixelType)v;
          valid = true;
        }
      } else {
        valid = parseColorOrder(orderStr, order);
      }
      if (!valid) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid colorOrder. "
                   "Valid: RGB, RBG, GRB, GBR, BRG, BGR (or 0-5)\"}";
        client.print(response);
        client.stop();
        return;
      }
      setColorOrder(order);
    }

    // Both params (or neither) applied; return the full current config.
    response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Connection: close\r\n\r\n";
    response += "{";
    response += "\"colorOrder\":\"" + colorOrderName(pixelOrder) + "\",";
    response += "\"colorOrderValue\":" + String((int)pixelOrder) + ",";
    response += "\"ledCount\":" + String(numLeds) + ",";
    response += "\"brightness\":" + String(globalBrightness) + ",";
    response += "\"effect\":\"" + effectName(currentEffect) + "\",";
    response += "\"power\":\"" + String(powerOn ? "on" : "off") + "\"";
    response += "}";
  }
  else if (path == "/api/pixels/set") {
    String pattern = getParam(fullPath, "pattern");
    if (pattern.length() == 0) {
      response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                 "Connection: close\r\n\r\n"
                 "{\"status\":\"error\",\"message\":\"Missing 'pattern' parameter\"}";
      client.print(response);
      client.stop();
      return;
    }

    // Collect all color parameters (repeatable)
    String colors[10];
    int numColors = 0;
    int searchPos = 0;
    while (numColors < 10) {
      String key = "color=";
      int pos = fullPath.indexOf(key, searchPos);
      if (pos < 0) break;
      int valStart = pos + key.length();
      int valEnd = fullPath.indexOf('&', valStart);
      if (valEnd < 0) valEnd = fullPath.length();
      String val = fullPath.substring(valStart, valEnd);
      val.trim();
      if (val.length() > 0) {
        colors[numColors++] = val;
      }
      searchPos = valEnd;
    }

    if (numColors == 0) {
      response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                 "Connection: close\r\n\r\n"
                 "{\"status\":\"error\",\"message\":\"At least one 'color' parameter is required\"}";
      client.print(response);
      client.stop();
      return;
    }

    // Validate hex colors
    for (int i = 0; i < numColors; i++) {
      String h = colors[i];
      h.trim();
      if (h.length() != 3 && h.length() != 6) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid hex color: " + colors[i] + "\"}";
        client.print(response);
        client.stop();
        return;
      }
      for (int j = 0; j < h.length(); j++) {
        char c = h.charAt(j);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                     "Connection: close\r\n\r\n"
                     "{\"status\":\"error\",\"message\":\"Invalid hex color: " + colors[i] + "\"}";
          client.print(response);
          client.stop();
          return;
        }
      }
    }

    // Brightness is unified with the global brightness variable: an explicit
    // value updates the global brightness (persisted, applies to every effect).
    // The buffer stays at full color; strip.setBrightness() scales it at show
    // time, so there is no double-scaling. Omit to keep the current value.
    String brightnessStr = getParam(fullPath, "brightness");
    if (brightnessStr.length() > 0) {
      bool isNumeric = true;
      for (int i = 0; i < brightnessStr.length(); i++)
        if (!isdigit(brightnessStr.charAt(i))) { isNumeric = false; break; }
      if (!isNumeric) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid brightness value\"}";
        client.print(response);
        client.stop();
        return;
      }
      setGlobalBrightness((uint8_t)constrain(brightnessStr.toInt(), 0, 255));
      prefs.putInt("brightness", globalBrightness);
    }

    if (pattern.equalsIgnoreCase("solid")) {
      generateSolidPattern(colors[0]);
    } else if (pattern.equalsIgnoreCase("striped")) {
      generateStripedPattern(colors, numColors);
    } else if (pattern.equalsIgnoreCase("gradient")) {
      if (numColors < 2) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Gradient requires at least 2 colors\"}";
        client.print(response);
        client.stop();
        return;
      }
      generateGradientPattern(colors, numColors);
    } else {
      response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                 "Connection: close\r\n\r\n"
                 "{\"status\":\"error\",\"message\":\"Unsupported pattern: " + pattern + "\"}";
      client.print(response);
      client.stop();
      return;
    }

    if (!staticPixelBuffer) {
      response = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n"
                 "Connection: close\r\n\r\n"
                 "{\"status\":\"error\",\"message\":\"Failed to allocate pixel buffer\"}";
      client.print(response);
      client.stop();
      return;
    }

    // No per-pattern brightness bake: the global brightness (set above or the
    // current value) is applied by strip.setBrightness() at show time.

    // Switch to static pixel mode so the pattern persists
    currentEffect = STATIC_PIXEL;
    currentVariation = 0;

    response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Connection: close\r\n\r\n"
               "{\"status\":\"ok\"}";
  }
  else if (path == "/help") {
    String helpResponse = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
    helpResponse += "Neopixel LED Controller REST API\n";
    helpResponse += "================================\n\n";
    helpResponse += "Available endpoints:\n\n";
    helpResponse += "GET /api/effect?name=<effect_name>\n";
    helpResponse += "  Set the active effect by name.\n";
    helpResponse += "  Valid names: FIREFLIES, RAINBOW_SWIPE, AURORA, COMET, CHASING_DOTS,\n";
    helpResponse += "               CYLON, DUAL_COMET, SPARKLE_SWEEP, POLICE, PLASMA,\n";
    helpResponse += "               RAINBOW_GRADIENT, PULSE_WAVE, SINGLE_RUNNER,\n";
    helpResponse += "               AUDIO_VISUALIZER, HEARTBEAT, TWINKLE, FIRE_FLICKER,\n";
    helpResponse += "               BOUNCING_BALLS, LIGHTNING_STORM, KALEIDOSCOPE,\n";
    helpResponse += "               COLLIDING_FILL, PAINT_SPLAT, SNAKE\n\n";
    helpResponse += "GET /api/effect?index=<0-23>\n";
    helpResponse += "  Set the active effect by its numeric index (0 = FIREFLIES).\n\n";
    helpResponse += "GET /api/variation?index=<0-4>\n";
    helpResponse += "  Set the variation of the current effect (0-based).\n\n";
    helpResponse += "GET /api/power?state=<on|off>\n";
    helpResponse += "  Turn the LEDs on or off.\n\n";
    helpResponse += "GET /api/brightness?value=<0-255>\n";
    helpResponse += "  Set the global LED brightness (0 = off, 255 = max).\n";
    helpResponse += "  Applied to every effect and saved across reboots.\n";
    helpResponse += "  Read the current value from /api/info.\n\n";
    helpResponse += "GET /api/info\n";
    helpResponse += "  Returns a JSON object with current effect, variation, power state,\n";
    helpResponse += "  effect count, and led count.\n\n";
    helpResponse += "GET /config?colorOrder=<RGB|RBG|GRB|GBR|BRG|BGR>\n";
    helpResponse += "  Set the NeoPixel color order (wiring of the strip).\n";
    helpResponse += "  Applies immediately and is saved across reboots.\n";
    helpResponse += "GET /config?ledCount=<1-300>\n";
    helpResponse += "  Set the number of LEDs (1-300).\n";
    helpResponse += "  Parameters can be combined in one request.\n";
    helpResponse += "GET /config\n";
    helpResponse += "  Returns the current configuration as JSON (color order, LED count,\n";
    helpResponse += "  brightness, effect, power).\n\n";
    helpResponse += "GET /api/pixels/set?pattern=<solid|striped|gradient>&color=RRGGBB[&color=...][&brightness=0-255]\n";
    helpResponse += "  Set the entire pixel strip to a static pattern.\n";
    helpResponse += "  pattern: solid (one color), striped (colors alternate), gradient (interpolated).\n";
    helpResponse += "  color: hex color(s), 3 or 6 digits, case insensitive. Repeat for multiple.\n";
    helpResponse += "  brightness: optional, 0-255. When given, sets the global brightness\n";
    helpResponse += "              (persisted, applies to all effects). Omit to keep the\n";
    helpResponse += "              current global brightness.\n\n";
    helpResponse += "GET /help\n";
    helpResponse += "  This help page.\n\n";
    helpResponse += "All responses are plain text except /api/info and /config (JSON).\n";
    helpResponse += "The root path (/) redirects to an external control page.\n";

    client.print(helpResponse);
    client.stop();
    return;
  }
  else if (path == "/" || path == "") {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.print(dashboard_html);
    client.stop();
    return;
  }

  client.print(response);
  client.stop();
}

// ====================================================================
//  EFFECT ANIMATIONS  (state variables + update function, per effect)
// ====================================================================

// ----------------------- FIREFLIES -----------------------
#define MAX_FIREFLIES 10
#define MIN_DISTANCE 5
struct Firefly {
  bool active;
  int led;
  float brightness, maxBrightness;
  float fadeInSpeed, fadeOutSpeed;
  bool fadingIn;
};
Firefly fireflies[MAX_FIREFLIES];
unsigned long nextFireflySpawn = 0;

void updateFireflies() {
  // ----- Variation table -----
  // Variation 0 : 1 firefly,  slow
  // Variation 1 : 2 fireflies, medium
  // Variation 2 : 3 fireflies, fast
  // Variation 3 : 5 fireflies, very fast
  // Variation 4 : 8 fireflies, chaotic
  const int   maxActive[]   = { 1, 2, 3, 5, 8 };
  const float fiSpeedMin[]  = { 2.0, 5.0, 10.0, 15.0, 20.0 };
  const float fiSpeedMax[]  = { 4.0, 8.0, 14.0, 20.0, 30.0 };
  const float foSpeedMin[]  = { 1.0, 2.0, 4.0, 6.0, 8.0 };
  const float foSpeedMax[]  = { 2.0, 4.0, 7.0, 10.0, 15.0 };
  const unsigned long spawnMin[] = { 3000, 2000, 1000, 500, 200 };
  const unsigned long spawnMax[] = { 8000, 4000, 2000, 1000, 500 };

  int variation = constrain(currentVariation, 0, 4);

  int activeCount = 0;
  for (int i = 0; i < MAX_FIREFLIES; i++) {
    if (fireflies[i].active) activeCount++;
  }

  unsigned long now = millis();
  if (activeCount < maxActive[variation] && now >= nextFireflySpawn) {
    int slot = -1;
    for (int i = 0; i < MAX_FIREFLIES; i++) {
      if (!fireflies[i].active) { slot = i; break; }
    }
    if (slot != -1) {
      for (int tries = 0; tries < 30; tries++) {
        int led = random(numLeds);
        bool busy = false, tooClose = false;
        for (int j = 0; j < MAX_FIREFLIES; j++) {
          if (fireflies[j].active && fireflies[j].led == led) busy = true;
          if (fireflies[j].active && abs(fireflies[j].led - led) < MIN_DISTANCE) tooClose = true;
        }
        if (!busy && !tooClose) {
          fireflies[slot].active = true;
          fireflies[slot].led = led;
          fireflies[slot].brightness = 0;
          if (variation == 0) {
              fireflies[slot].maxBrightness = 25;   // 10 % of 255
          } else {
              fireflies[slot].maxBrightness = random(80, 160);
          }
          fireflies[slot].fadeInSpeed = random(fiSpeedMin[variation] * 10, fiSpeedMax[variation] * 10) / 10.0f;
          fireflies[slot].fadeOutSpeed = random(foSpeedMin[variation] * 10, foSpeedMax[variation] * 10) / 10.0f;
          fireflies[slot].fadingIn = true;
          break;
        }
      }
    }
    nextFireflySpawn = now + random(spawnMin[variation], spawnMax[variation]);
  }

  strip.clear();
  for (int i = 0; i < MAX_FIREFLIES; i++) {
    if (!fireflies[i].active) continue;

    if (fireflies[i].fadingIn) {
      fireflies[i].brightness += fireflies[i].fadeInSpeed;
      if (fireflies[i].brightness >= fireflies[i].maxBrightness) {
        fireflies[i].brightness = fireflies[i].maxBrightness;
        fireflies[i].fadingIn = false;
      }
    } else {
      fireflies[i].brightness -= fireflies[i].fadeOutSpeed;
      if (fireflies[i].brightness <= 0) {
        fireflies[i].active = false;
        continue;
      }
    }

    uint8_t b = (uint8_t)fireflies[i].brightness;
    uint32_t col = strip.Color(b, b / 2, 0);
    strip.setPixelColor(fireflies[i].led, col);
  }
  strip.show();
}

// ----------------------- RAINBOW SWIPE -----------------------
float rainbowOffset = 0;

void updateRainbowSwipe() {
  float speed = 0.2; int dir = 1;
  switch (currentVariation) {
    case 0: speed = 0.2; dir = 1; break;
    case 1: speed = 0.2; dir = -1; break;
    case 2: speed = 0.5; dir = 1; break;
    case 3: speed = 0.08; dir = 1; break;
    case 4: speed = 0.35; dir = 1; break;
  }
  rainbowOffset += speed * dir;
  if (rainbowOffset >= 256.0f) rainbowOffset -= 256.0f;
  if (rainbowOffset < 0) rainbowOffset += 256.0f;
  for (int i = 0; i < numLeds; i++) {
    byte hue = (byte)(((int)(i * 256L / numLeds) + (int)rainbowOffset) & 0xFF);
    strip.setPixelColor(i, wheel(hue));
  }
  strip.show();
}

// ----------------------- AURORA -----------------------
float auroraPhase = 0;

void updateAurora() {
  for (int i = 0; i < numLeds; i++) {
    float pos = (float)i / numLeds;
    float w1 = sin(pos * 5.0f + auroraPhase) * 0.5f + 0.5f;
    float w2 = sin(pos * 3.0f - auroraPhase * 0.7f + 1.2f) * 0.5f + 0.5f;
    float w3 = sin(pos * 8.0f + auroraPhase * 1.3f + 2.5f) * 0.5f + 0.5f;
    float wave = w1 * 0.5f + w2 * 0.3f + w3 * 0.2f;
    uint8_t r, g, b;
    switch (currentVariation) {
      case 0:
        r = (uint8_t)((1.0f - wave) * wave * 50);
        g = (uint8_t)(wave * 100 + 20);
        b = (uint8_t)((1.0f - wave) * 130 + 20);
        break;
      case 1:
        r = (uint8_t)(wave * 200 + 30); g = (uint8_t)(wave * 80); b = 0; break;
      case 2:
        r = (uint8_t)(wave * 80); g = (uint8_t)(wave * 120); b = (uint8_t)(wave * 200 + 55); break;
      case 3:
        r = (uint8_t)(wave * 150 + 50); g = (uint8_t)(wave * 50); b = (uint8_t)(wave * 150 + 50); break;
      case 4: {
        uint8_t hue = (uint8_t)(wave * 255);
        uint32_t col = wheel(hue);
        r = (uint8_t)((col >> 16) & 0xFF) / 2 + 80;
        g = (uint8_t)((col >> 8) & 0xFF) / 2 + 80;
        b = (uint8_t)(col & 0xFF) / 2 + 80;
        break;
      }
    }
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
  auroraPhase += 0.012f;
}

// ----------------------- COMET -----------------------
struct CometState { float pos=0, speed=0.4, hueOffset=0; int tailLen=5; };
CometState comet;

void updateComet() {
  strip.clear();
  int headIdx = (int)comet.pos;
  uint8_t hue;
  if (currentVariation == 0) {
    hue = ((uint8_t)(comet.pos * 255 / numLeds) + (uint8_t)comet.hueOffset) & 0xFF;
  } else if (currentVariation == 1) {
    hue = (uint8_t)comet.hueOffset;
  } else if (currentVariation == 2) {
    hue = (uint8_t)(millis() / 50) & 0xFF;
  } else if (currentVariation == 3) {
    hue = 0;
  } else {
    hue = (uint8_t)comet.hueOffset;
  }
  uint32_t headColor = (currentVariation == 3) ? strip.Color(255,255,255) : wheel(hue);
  strip.setPixelColor(headIdx, headColor);
  for (int t = 1; t <= comet.tailLen; t++) {
    int idx = headIdx - t;
    if (idx < 0) continue;
    float brightness = 1.0f - (float)t / (comet.tailLen + 1);
    if (currentVariation == 4 && t > 1) {
      uint8_t tailHue = (hue + t * 8) % 256;
      uint32_t tailCol = wheel(tailHue);
      uint8_t r = (uint8_t)((tailCol >> 16 & 0xFF) * brightness);
      uint8_t g = (uint8_t)((tailCol >> 8 & 0xFF) * brightness);
      uint8_t b = (uint8_t)((tailCol & 0xFF) * brightness);
      strip.setPixelColor(idx, strip.Color(r, g, b));
    } else {
      uint8_t r = (uint8_t)((headColor >> 16 & 0xFF) * brightness);
      uint8_t g = (uint8_t)((headColor >> 8 & 0xFF) * brightness);
      uint8_t b = (uint8_t)((headColor & 0xFF) * brightness);
      strip.setPixelColor(idx, strip.Color(r, g, b));
    }
  }
  strip.show();
  comet.pos += comet.speed;
  comet.hueOffset += 0.3f;
  if (comet.pos >= numLeds) {
    comet.pos = 0;
    comet.speed = random(20,60)/100.0f;
    comet.tailLen = random(3,9);
    if (currentVariation == 1 || currentVariation == 4) comet.hueOffset = random(0,256);
  }
}

// ----------------------- CHASING DOTS -----------------------
#define MAX_CHASERS 6
struct Chaser { float pos; float speed; uint8_t hue; };
Chaser chasers[MAX_CHASERS];
int numChasers = 4;

void updateChasingDots() {
  strip.clear();
  int dotCount = 2 + currentVariation;
  if (dotCount > MAX_CHASERS) dotCount = MAX_CHASERS;
  if (numChasers != dotCount) {
    numChasers = dotCount;
    for (int i = 0; i < numChasers; i++) {
      chasers[i].pos = i * (numLeds / numChasers);
      chasers[i].speed = 0.25f;
      chasers[i].hue = i * (256 / numChasers);
    }
  }
  for (int d = 0; d < numChasers; d++) {
    int headIdx = (int)chasers[d].pos;
    uint32_t color = wheel(chasers[d].hue);
    strip.setPixelColor(headIdx, color);
    for (int t = 1; t <= 2; t++) {
      int idx = headIdx - t;
      if (idx < 0) idx += numLeds;
      float bright = 1.0f - t * 0.35f;
      uint8_t r = (uint8_t)((color >> 16 & 0xFF) * bright);
      uint8_t g = (uint8_t)((color >> 8 & 0xFF) * bright);
      uint8_t b = (uint8_t)((color & 0xFF) * bright);
      strip.setPixelColor(idx, strip.Color(r, g, b));
    }
    chasers[d].pos += chasers[d].speed;
    if (chasers[d].pos >= numLeds) chasers[d].pos -= numLeds;
  }
  strip.show();
}

// ----------------------- CYLON -----------------------
float cylonPhase=0, cylonSpeed=0.03;
uint8_t cylonHue=0;
bool cylonAtLeftEnd=false, cylonAtRightEnd=false;

void updateCylon() {
  strip.clear();
  float sineVal = sin(cylonPhase);
  float pos = (numLeds - 1) / 2.0f * (1.0f + sineVal);
  int center = (int)(pos + 0.5f);
  uint32_t color;
  if (currentVariation == 2) color = wheel((uint8_t)(millis() / 20) & 0xFF);
  else if (currentVariation == 3) color = strip.Color(255,255,255);
  else if (currentVariation == 4) color = strip.Color(255,0,0);
  else color = wheel(cylonHue);
  strip.setPixelColor(center, color);
  for (int i = 1; i <= 3; i++) {
    float bright = 1.0f - i * 0.3f;
    int idxL = center - i, idxR = center + i;
    if (idxL >= 0) {
      uint8_t r = (uint8_t)((color >> 16 & 0xFF) * bright);
      uint8_t g = (uint8_t)((color >> 8 & 0xFF) * bright);
      uint8_t b = (uint8_t)((color & 0xFF) * bright);
      strip.setPixelColor(idxL, strip.Color(r, g, b));
    }
    if (idxR < numLeds) {
      uint8_t r = (uint8_t)((color >> 16 & 0xFF) * bright);
      uint8_t g = (uint8_t)((color >> 8 & 0xFF) * bright);
      uint8_t b = (uint8_t)((color & 0xFF) * bright);
      strip.setPixelColor(idxR, strip.Color(r, g, b));
    }
  }
  strip.show();
  cylonPhase += cylonSpeed;
  bool nearLeft = (center <= 1), nearRight = (center >= numLeds - 2);
  if (nearLeft && !cylonAtLeftEnd) { cylonAtLeftEnd = true; if (currentVariation == 0 || currentVariation == 1) cylonHue += 43; }
  if (!nearLeft) cylonAtLeftEnd = false;
  if (nearRight && !cylonAtRightEnd) { cylonAtRightEnd = true; if (currentVariation == 0 || currentVariation == 1) cylonHue += 43; }
  if (!nearRight) cylonAtRightEnd = false;
}

// ----------------------- DUAL COMET -----------------------
struct DualComet { float leftPos=0, rightPos=numLeds-1, leftSpeed=0.3, rightSpeed=0.3; uint8_t leftHue=0, rightHue=128; int tailLen=5; bool collided=false; int flashFrames=0, collisionStyle=0; };
DualComet dualComet;

void updateDualComet() {
  strip.clear();
  if (dualComet.collided) {
    int center = numLeds / 2;
    for (int i = 0; i < numLeds; i++) {
      float dist = abs(i - center);
      if (dist < 8) {
        float bright = 1.0f - dist / 8.0f;
        uint32_t col;
        switch (dualComet.collisionStyle) {
          case 0: { uint8_t v = (uint8_t)(255 * bright); col = strip.Color(v,v,v); break; }
          case 1: col = wheel(random(256)); break;
          case 2: col = wheel((uint8_t)(dist * 20)); break;
          case 3: col = strip.Color(255,200,0); break;
          case 4: col = strip.Color(255,100,100); break;
        }
        strip.setPixelColor(i, col);
      }
    }
    strip.show();
    dualComet.flashFrames--;
    if (dualComet.flashFrames <= 0) {
      dualComet.collided = false;
      dualComet.leftPos = 0; dualComet.rightPos = numLeds - 1;
      dualComet.leftHue = random(0,256); dualComet.rightHue = (dualComet.leftHue + 128) % 256;
      dualComet.leftSpeed = random(20,45)/100.0f; dualComet.rightSpeed = random(20,45)/100.0f;
      dualComet.collisionStyle = currentVariation;
    }
    return;
  }
  uint32_t leftCol = wheel(dualComet.leftHue), rightCol = wheel(dualComet.rightHue);
  int leftHead = (int)dualComet.leftPos, rightHead = (int)dualComet.rightPos;
  strip.setPixelColor(leftHead, leftCol);
  strip.setPixelColor(rightHead, rightCol);
  for (int t = 1; t <= dualComet.tailLen; t++) {
    int idxL = leftHead - t, idxR = rightHead + t;
    if (idxL >= 0) {
      float bright = 1.0f - (float)t / (dualComet.tailLen + 1);
      uint8_t r = (uint8_t)((leftCol >> 16 & 0xFF) * bright);
      uint8_t g = (uint8_t)((leftCol >> 8 & 0xFF) * bright);
      uint8_t b = (uint8_t)((leftCol & 0xFF) * bright);
      strip.setPixelColor(idxL, strip.Color(r, g, b));
    }
    if (idxR < numLeds) {
      float bright = 1.0f - (float)t / (dualComet.tailLen + 1);
      uint8_t r = (uint8_t)((rightCol >> 16 & 0xFF) * bright);
      uint8_t g = (uint8_t)((rightCol >> 8 & 0xFF) * bright);
      uint8_t b = (uint8_t)((rightCol & 0xFF) * bright);
      strip.setPixelColor(idxR, strip.Color(r, g, b));
    }
  }
  strip.show();
  dualComet.leftPos += dualComet.leftSpeed;
  dualComet.rightPos -= dualComet.rightSpeed;
  if (dualComet.leftPos >= dualComet.rightPos) {
    dualComet.collided = true;
    dualComet.flashFrames = 15;
  }
}

// ----------------------- SPARKLE SWEEP -----------------------
struct SparkleSweep { float pos=0, speed=0.5, density=0.3; int halfWidth=7; };
SparkleSweep sparkle;

void updateSparkleSweep() {
  strip.clear();
  int center = (int)sparkle.pos;
  switch (currentVariation) {
    case 0: sparkle.halfWidth=5; sparkle.density=0.4; break;
    case 1: sparkle.halfWidth=12; sparkle.density=0.15; break;
    case 2: sparkle.halfWidth=10; sparkle.density=0.35; break;
    case 3: sparkle.halfWidth=7; sparkle.density=0.6; break;
    case 4: sparkle.halfWidth=15; sparkle.density=0.25; break;
  }
  for (int i=0; i<numLeds; i++) {
    if (abs(i - center) <= sparkle.halfWidth) {
      if (random(100) < (int)(sparkle.density*100)) {
        int bright = random(180,255);
        strip.setPixelColor(i, strip.Color(bright,bright,bright));
      }
    }
  }
  strip.show();
  sparkle.pos += sparkle.speed;
  if (sparkle.pos >= numLeds + sparkle.halfWidth) sparkle.pos = -sparkle.halfWidth;
}

// ----------------------- POLICE -----------------------
#define POLICE_BLINK_MS 400
unsigned long policeBlinkTimer = 0;
bool policeBlinkState = false;

void updatePolice() {
  if (millis() - policeBlinkTimer >= POLICE_BLINK_MS) {
    policeBlinkTimer = millis();
    policeBlinkState = !policeBlinkState;
  }

  switch (currentVariation) {
    case 0: {
      for (int i = 0; i < numLeds; i++) {
        bool isEven = (i % 2 == 0);
        if (policeBlinkState == false) {
          strip.setPixelColor(i, isEven ? strip.Color(255,0,0) : strip.Color(0,0,255));
        } else {
          strip.setPixelColor(i, isEven ? strip.Color(0,0,255) : strip.Color(255,0,0));
        }
      }
      break;
    }

    case 1: {
      int half = numLeds / 2;
      for (int i = 0; i < half; i++) {
        if (policeBlinkState == false) {
          strip.setPixelColor(i, 0, 0, 255);
          strip.setPixelColor(i + half, 255, 0, 0);
        } else {
          strip.setPixelColor(i, 255, 0, 0);
          strip.setPixelColor(i + half, 0, 0, 255);
        }
      }
      if (numLeds % 2 != 0) {
        int mid = half;
        strip.setPixelColor(mid, policeBlinkState ? strip.Color(255,0,0) : strip.Color(0,0,255));
      }
      break;
    }

    case 2: {
      int third = numLeds / 3;
      int rem   = numLeds % 3;
      int seg1End = third + (rem > 0 ? 1 : 0) - 1;
      int seg2End = seg1End + third + (rem > 1 ? 1 : 0);
      for (int i = 0; i < numLeds; i++) {
        bool inCenter = (i > seg1End && i <= seg2End);
        if (policeBlinkState == false) {
          strip.setPixelColor(i, inCenter ? strip.Color(0,0,255) : strip.Color(255,0,0));
        } else {
          strip.setPixelColor(i, inCenter ? strip.Color(255,0,0) : strip.Color(0,0,255));
        }
      }
      break;
    }

    case 3: {
      uint32_t col = policeBlinkState ? strip.Color(0,0,255) : strip.Color(255,0,0);
      for (int i = 0; i < numLeds; i++) {
        strip.setPixelColor(i, col);
      }
      break;
    }

    case 4: {
      static float sweepPos = 0;
      static bool sweepColor = false;
      static unsigned long lastSweepUpdate = 0;
      if (millis() - lastSweepUpdate >= 20) {
        lastSweepUpdate = millis();
        sweepPos += 0.6;
        if (sweepPos >= numLeds - 1) {
          sweepPos = 0;
          sweepColor = !sweepColor;
        }
      }

      strip.clear();
      uint32_t col = sweepColor ? strip.Color(0,0,255) : strip.Color(255,0,0);
      int head = (int)sweepPos;
      strip.setPixelColor(head, col);
      for (int t = 1; t <= 4; t++) {
        int idx = head - t;
        if (idx < 0) continue;
        float bright = 1.0f - t * 0.22f;
        uint8_t r = (uint8_t)((col >> 16 & 0xFF) * bright);
        uint8_t g = (uint8_t)((col >> 8 & 0xFF) * bright);
        uint8_t b = (uint8_t)((col & 0xFF) * bright);
        strip.setPixelColor(idx, strip.Color(r, g, b));
      }
      break;
    }
  }

  strip.show();
}

// ----------------------- PLASMA -----------------------
#define MAX_BLOBS 5
struct Blob { float phase, speed, amplitude, centerOffset, sigma; uint8_t hue; };
Blob blobs[MAX_BLOBS];
int numBlobs = 3;

void updatePlasma() {
  strip.clear();
  int blobCount = 2 + currentVariation;
  if (blobCount > MAX_BLOBS) blobCount = MAX_BLOBS;
  if (numBlobs != blobCount) {
    numBlobs = blobCount;
    for (int b=0; b<numBlobs; b++) {
      blobs[b].phase = random(0,628)/100.0f;
      blobs[b].speed = random(15,40)/1000.0f;
      blobs[b].amplitude = random(10, numLeds/2);
      blobs[b].centerOffset = random(numLeds/4, 3*numLeds/4);
      blobs[b].sigma = (currentVariation==4) ? random(8,15) : random(4,10);
      blobs[b].hue = random(0,256);
    }
  }
  for (int b=0; b<numBlobs; b++) blobs[b].phase += blobs[b].speed;
  for (int i=0; i<numLeds; i++) {
    int rAcc=0,gAcc=0,bAcc=0;
    for (int b=0; b<numBlobs; b++) {
      float center = blobs[b].centerOffset + blobs[b].amplitude * sin(blobs[b].phase);
      float dist = i - center;
      float intensity = exp(-(dist*dist)/(2*blobs[b].sigma*blobs[b].sigma));
      uint32_t col = wheel(blobs[b].hue);
      rAcc += (int)((col>>16&0xFF)*intensity);
      gAcc += (int)((col>>8&0xFF)*intensity);
      bAcc += (int)((col&0xFF)*intensity);
    }
    strip.setPixelColor(i, strip.Color(constrain(rAcc,0,255), constrain(gAcc,0,255), constrain(bAcc,0,255)));
  }
  strip.show();
}

// ----------------------- RAINBOW GRADIENT -----------------------
struct RainbowGradient { float pos=0, speed=0.6, hueOffset=0; int length=10; };
RainbowGradient rainbowGrad;

void updateRainbowGradient() {
  strip.clear();
  int startIdx = (int)rainbowGrad.pos;
  int lengths[5] = {6, 10, 14, 18, 22};
  int winLen = lengths[currentVariation];
  for (int i=0; i<winLen; i++) {
    int idx = startIdx + i;
    if (idx<0 || idx>=numLeds) continue;
    float rel = (float)i/(winLen-1);
    uint8_t hue = (uint8_t)(255.0f*rel) + (uint8_t)rainbowGrad.hueOffset;
    strip.setPixelColor(idx, wheel(hue));
  }
  strip.show();
  rainbowGrad.pos += rainbowGrad.speed;
  rainbowGrad.hueOffset += 0.15f;
  if (rainbowGrad.pos >= numLeds) rainbowGrad.pos = -winLen;
}

// ----------------------- PULSE WAVE -----------------------
struct PulseWave { float pos=-10, speed=0.4, sigma=4.0, hueOffset=0; int direction=1; };
PulseWave pulseWave;

void updatePulseWave() {
  strip.clear();
  switch (currentVariation) {
    case 0: pulseWave.sigma=3.0; pulseWave.speed=0.6; break;
    case 1: pulseWave.sigma=7.0; pulseWave.speed=0.25; break;
    case 2: pulseWave.sigma=10.0; pulseWave.speed=0.4; break;
    case 3: pulseWave.sigma=5.0; pulseWave.speed=0.8; break;
    case 4: pulseWave.sigma=2.5; pulseWave.speed=1.0; break;
  }
  for (int i=0; i<numLeds; i++) {
    float dist = i - pulseWave.pos;
    float intensity = exp(-(dist*dist)/(2*pulseWave.sigma*pulseWave.sigma));
    uint8_t hue = (uint8_t)(pulseWave.pos*255/numLeds + pulseWave.hueOffset);
    uint32_t col = wheel(hue);
    strip.setPixelColor(i, strip.Color(
      (uint8_t)((col>>16&0xFF)*intensity),
      (uint8_t)((col>>8&0xFF)*intensity),
      (uint8_t)((col&0xFF)*intensity)
    ));
  }
  strip.show();
  pulseWave.pos += pulseWave.speed;
  pulseWave.hueOffset += 0.4f;
  if (pulseWave.pos > numLeds + pulseWave.sigma*2) pulseWave.pos = -pulseWave.sigma*2;
}

// ----------------------- SINGLE PIXEL RUNNER -----------------------
struct RunnerState { float pos=0, speed=0.5; uint8_t hue=0; int direction=1; bool bounce=false; int tailLen=2; bool rainbowShift=false; };
RunnerState runner;

void updateSingleRunner() {
  strip.clear();
  runner.speed = 1.0f;   // one full LED per frame = fastest coherent runner

  if (runner.bounce) {
    runner.pos += runner.speed * runner.direction;
    if (runner.pos >= numLeds - 1) { runner.direction = -1; runner.pos = numLeds - 1; }
    if (runner.pos <= 0) { runner.direction = 1; runner.pos = 0; }
  } else {
    runner.pos += runner.speed;
    if (runner.pos >= numLeds) {
      runner.pos = 0;
      if (currentVariation == 0) runner.hue = random(0, 256);
    }
  }

  int head = (int)runner.pos;
  uint32_t color;

  if (currentVariation == 0) {
    color = wheel(runner.hue);
  } else if (currentVariation == 1) {
    uint8_t hue = (uint8_t)(runner.pos * 255 / numLeds) + (uint8_t)(millis()/50);
    color = wheel(hue);
  } else if (currentVariation == 2) {
    uint8_t hue = (uint8_t)(millis() / 100);
    color = wheel(hue);
  } else if (currentVariation == 3) {
    color = strip.Color(255, 255, 255);
  } else {
    uint8_t hue = (uint8_t)(millis() / 80);
    color = wheel(hue);
  }

  strip.setPixelColor(head, color);

  if (currentVariation == 4) {
    for (int t = 1; t <= runner.tailLen; t++) {
      int idx = head - t * runner.direction;
      if (idx < 0 || idx >= numLeds) continue;
      float brightness = 1.0f - (float)t / (runner.tailLen + 1);
      uint8_t r = (uint8_t)((color >> 16 & 0xFF) * brightness);
      uint8_t g = (uint8_t)((color >> 8 & 0xFF) * brightness);
      uint8_t b = (uint8_t)((color & 0xFF) * brightness);
      strip.setPixelColor(idx, strip.Color(r, g, b));
    }
  }

  strip.show();
}

// ----------------------- AUDIO VISUALIZER -----------------------
struct VisualizerBand { float amplitude, phase, speed; int startLed, endLed; };
VisualizerBand *bands = nullptr;
int numBands = 0;
float audioTime = 0;

void updateAudioVisualizer() {
  int desiredBands;
  switch (currentVariation) {
    case 0: desiredBands = 5; break;
    case 1: desiredBands = 10; break;
    case 2: desiredBands = 5; break;
    case 3: desiredBands = 10; break;
    case 4: desiredBands = numLeds; break;
    default: desiredBands = 5;
  }

  if (numBands != desiredBands) {
    delete[] bands;
    bands = new VisualizerBand[desiredBands];
    numBands = desiredBands;
    int ledsPerBand = numLeds / numBands;
    int remainder = numLeds % numBands;
    int start = 0;
    for (int i = 0; i < numBands; i++) {
      bands[i].startLed = start;
      int size = ledsPerBand + (i < remainder ? 1 : 0);
      bands[i].endLed = start + size - 1;
      start += size;
      bands[i].phase = random(0, 628) / 100.0f;
      bands[i].speed = random(20, 60) / 1000.0f;
    }
  }

  audioTime += 0.02f;
  for (int i = 0; i < numBands; i++) {
    bands[i].phase += bands[i].speed;
    float wave = sin(bands[i].phase) * 0.5f + 0.5f;
    float noise = sin(bands[i].phase * 5.3f + audioTime * 10.0f) * 0.2f;
    bands[i].amplitude = constrain(wave * 0.7f + noise + random(-5, 5) / 100.0f, 0.0f, 1.0f);
  }

  strip.clear();

  for (int i = 0; i < numBands; i++) {
    int ledCount = bands[i].endLed - bands[i].startLed + 1;
    int litCount = (int)(bands[i].amplitude * ledCount + 0.5f);

    for (int j = 0; j < ledCount; j++) {
      int ledIndex = bands[i].startLed + j;
      float brightness = (j < litCount) ? 1.0f : 0.0f;
      if (currentVariation == 2 && j == litCount - 1 && litCount > 0) {
        brightness = 2.0f;
      }
      if (brightness <= 0) continue;

      uint32_t col;
      switch (currentVariation) {
        case 0:
          col = wheel((uint8_t)(i * (256 / numBands))); break;
        case 1:
        case 2: {
          if (currentVariation == 1) {
            uint8_t r = 255, g = (uint8_t)(bands[i].amplitude * 200);
            col = strip.Color(r, g, 0);
          } else {
            uint8_t b = 200 + (uint8_t)(bands[i].amplitude * 55);
            col = strip.Color(180, 210, b);
          }
          break;
        }
        case 3:
          col = strip.Color(0, 255, 0); break;
        case 4:
          col = wheel((uint8_t)(bands[i].amplitude * 255)); break;
      }
      if (currentVariation != 2 || j != litCount - 1) {
        uint8_t r = (uint8_t)((col >> 16 & 0xFF) * brightness);
        uint8_t g = (uint8_t)((col >> 8 & 0xFF) * brightness);
        uint8_t b = (uint8_t)((col & 0xFF) * brightness);
        strip.setPixelColor(ledIndex, strip.Color(r, g, b));
      } else {
        strip.setPixelColor(ledIndex, strip.Color(255, 255, 255));
      }
    }
  }

  strip.show();
}

// ----------------------- HEARTBEAT -----------------------
// The rhythm follows a real heart: a strong "lub" pulse, a softer "dub"
// ~170ms later, then a rest sized by the heart rate (72 BPM) plus a little
// beat-to-beat jitter so it never feels metronomic.
enum HBPhase { HB_LUB, HB_DUB, HB_REST };
HBPhase hbPhase = HB_REST;
unsigned long hbLubTime = 0;      // onset of the strong "lub" pulse
unsigned long hbDubTime = 0;      // onset of the softer "dub" pulse
unsigned long hbRestStart = 0;    // when the resting glow began
unsigned long hbNextBeat = 0;     // when the next beat should fire
float hbBreathPhase = 0;
bool hbLeftActive = true;

#define HB_BPM        72      // resting heart rate
#define HB_LUB_DUB_MS 170     // S1 -> S2 interval
#define HB_DUB_VIS_MS 300     // how long the dub phase lasts
#define HB_TRAVEL_MS  500     // V3: time for the beat to cross the strip

// Pulse envelope: a sharp attack and an exponential decay, the shape of an
// arterial pulse waveform (fast rise, slower fall).
float hbEnvelope(float elapsedMs) {
  float t = elapsedMs * 0.001f;
  if (t <= 0.0f) return 0.0f;
  return (1.0f - expf(-t / 0.035f)) * expf(-t / 0.16f);
}

// Warm red that shifts from deep red at low intensity to a hot red-orange
// at the peak, like light glowing through tissue.
uint32_t hbMainColor(float env) {
  float t = constrain(env, 0.0f, 1.0f);
  uint8_t r = (uint8_t)(255.0f * t);
  uint8_t g = (uint8_t)((14.0f + 52.0f * t) * t);
  uint8_t b = (uint8_t)((10.0f + 28.0f * t) * t);
  return strip.Color(r, g, b);
}

void drawHeartPulse(float pos, float sigma, uint32_t col) {
  float s2 = 2.0f * sigma * sigma;
  for (int i = 0; i < numLeds; i++) {
    float dist = i - pos;
    float lum = expf(-(dist * dist) / s2);
    if (lum < 0.012f) continue;
    uint8_t r = (uint8_t)(((col >> 16) & 0xFF) * lum);
    uint8_t g = (uint8_t)(((col >> 8) & 0xFF) * lum);
    uint8_t b = (uint8_t)((col & 0xFF) * lum);
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
}

void updateHeartbeat() {
  unsigned long now = millis();

  // V0: natural breathing - inhale, brief hold, slower exhale, smoothstep eased
  if (currentVariation == 0) {
    hbBreathPhase += 0.02f;
    float period = 4.8f;
    float p = fmodf(hbBreathPhase, period);
    float t;
    if (p < 1.8f)      t = p / 1.8f;                 // inhale
    else if (p < 2.2f) t = 1.0f;                     // held breath
    else               t = 1.0f - (p - 2.2f) / 2.6f; // exhale
    t = constrain(t, 0.0f, 1.0f);
    float eased = t * t * (3.0f - 2.0f * t);
    float b = 15.0f + eased * 240.0f;
    uint32_t col = strip.Color((uint8_t)b, (uint8_t)(b * 0.07f), (uint8_t)(b * 0.05f));
    for (int i = 0; i < numLeds; i++) {
      strip.setPixelColor(i, col);
    }
    strip.show();
    return;
  }

  // V3: one beat per cycle glides along the strip (eased), then rests
  if (currentVariation == 3) {
    if (hbPhase == HB_REST) {
      if (now >= hbNextBeat) {
        hbPhase = HB_LUB;
        hbLubTime = now;
      }
    } else if (now - hbLubTime >= HB_TRAVEL_MS) {
      hbPhase = HB_REST;
      hbRestStart = now;
      hbNextBeat = now + 60000UL / HB_BPM + random(-45, 46);
    }

    float t = (float)(now - hbLubTime);
    float prog = constrain(t / HB_TRAVEL_MS, 0.0f, 1.0f);
    float eased = prog * prog * (3.0f - 2.0f * prog);
    float pos = eased * (numLeds - 1.0f);
    float sigma = 2.5f + 5.0f * eased;   // widens as it travels
    float env = hbEnvelope(t);

    strip.clear();
    float restElapsed = (hbPhase == HB_REST) ? (float)(now - hbRestStart) : 0.0f;
    float glow = 0.03f + 0.05f * expf(-restElapsed / 1100.0f);
    uint32_t glowCol = strip.Color((uint8_t)(255.0f * glow),
                                   (uint8_t)(18.0f * glow),
                                   (uint8_t)(12.0f * glow));
    for (int i = 0; i < numLeds; i++) strip.setPixelColor(i, glowCol);
    drawHeartPulse(pos, sigma, hbMainColor(env));
    strip.show();
    return;
  }

  // V1, V2, V4: physiological lub-dub-rest rhythm
  if (hbPhase == HB_REST) {
    if (now >= hbNextBeat) {
      hbPhase = HB_LUB;
      hbLubTime = now;
      if (currentVariation == 2) hbLeftActive = !hbLeftActive;
    }
  } else if (hbPhase == HB_LUB) {
    if (now - hbLubTime >= HB_LUB_DUB_MS) {
      hbPhase = HB_DUB;
      hbDubTime = now;
    }
  } else if (now - hbDubTime >= HB_DUB_VIS_MS) {
    hbPhase = HB_REST;
    hbRestStart = now;
    hbNextBeat = now + 60000UL / HB_BPM + random(-45, 46);
  }

  float lubElapsed = (float)(now - hbLubTime);
  float lubEnv = hbEnvelope(lubElapsed);
  // dub is only rendered after it has actually fired (phase entered)
  float dubEnv = 0.0f;
  if (hbPhase == HB_DUB || hbPhase == HB_REST) {
    dubEnv = 0.62f * hbEnvelope((float)(now - hbDubTime));
  }

  float sigmaMax = (currentVariation == 4) ? 15.0f
                 : (currentVariation == 1) ? 7.0f : 6.0f;
  float sigma = 2.5f + (1.0f - expf(-lubElapsed / 90.0f)) * sigmaMax;

  float center1 = numLeds * 0.5f;
  if (currentVariation == 2) {
    center1 = hbLeftActive ? numLeds * 0.25f : numLeds * 0.75f;
  }

  strip.clear();
  float restElapsed = (hbPhase == HB_REST) ? (float)(now - hbRestStart) : 0.0f;
  float glow = 0.03f + 0.05f * expf(-restElapsed / 1100.0f);
  uint32_t glowCol = strip.Color((uint8_t)(255.0f * glow),
                                 (uint8_t)(18.0f * glow),
                                 (uint8_t)(12.0f * glow));
  for (int i = 0; i < numLeds; i++) strip.setPixelColor(i, glowCol);

  drawHeartPulse(center1, sigma, hbMainColor(lubEnv));
  if (currentVariation == 4) {
    // echo pump: a softer pulse lagging the main one, spreading wider
    float echoEnv = 0.45f * hbEnvelope(lubElapsed - 130.0f);
    drawHeartPulse(center1, sigma * 1.25f, hbMainColor(echoEnv));
  } else {
    drawHeartPulse(center1, sigma * 0.85f, hbMainColor(dubEnv));
  }
  strip.show();
}

// ----------------------- TWINKLE (STARRY NIGHT) -----------------------
#define MAX_STARS 150
struct Star {
  int led;
  float phase, speed;
  uint8_t hue;
  uint8_t peak;
  float floor;
  int novaTimer;
};
Star stars[MAX_STARS];
int numStars = 0;
int twinkleVariation = -1;

void initTwinkle() {
  int v = constrain(currentVariation, 0, 4);
  int densities[] = { 10, 12, 14, 60, 25 };   // stars per 50 LEDs
  int count = numLeds * densities[v] / 50;
  if (count > MAX_STARS) count = MAX_STARS;
  numStars = count;
  const int speedMin[] = { 10, 12, 15, 40, 15 };
  const int speedMax[] = { 25, 30, 40, 100, 45 };
  for (int i = 0; i < numStars; i++) {
    stars[i].led = random(numLeds);
    stars[i].phase = random(0, 628) / 100.0f;
    stars[i].speed = random(speedMin[v], speedMax[v]) / 1000.0f;
    stars[i].hue = random(0, 256);
    stars[i].peak = random(60, 255);
    stars[i].floor = 0.05f + random(0, 20) / 100.0f;
    stars[i].novaTimer = 0;
  }
  twinkleVariation = v;
}

void updateTwinkle() {
  int v = constrain(currentVariation, 0, 4);
  if (v != twinkleVariation) initTwinkle();
  strip.clear();
  for (int i = 0; i < numStars; i++) {
    stars[i].phase += stars[i].speed;
    float tw = (sin(stars[i].phase) + 1.0f) / 2.0f;
    float b = stars[i].floor + (1.0f - stars[i].floor) * tw * tw;
    if (v == 4) {   // occasional supernova flashes
      if (stars[i].novaTimer > 0) { stars[i].novaTimer--; b = 1.0f; }
      else if (random(3000) < 2) stars[i].novaTimer = 8 + random(0, 8);
    }
    uint8_t br = (uint8_t)(b * stars[i].peak);
    uint32_t col;
    switch (v) {
      case 0: col = strip.Color(br, br, br); break;                      // white, slow
      case 1: col = strip.Color(br, (uint8_t)(br * 0.85f), (uint8_t)(br * 0.35f)); break; // golden
      case 2: {                                                          // multicolor
        uint32_t c = wheel(stars[i].hue);
        col = strip.Color((uint8_t)((c >> 16 & 0xFF) * b),
                          (uint8_t)((c >> 8 & 0xFF) * b),
                          (uint8_t)((c & 0xFF) * b));
        break;
      }
      case 3: col = strip.Color(br, br, br); break;                      // dense, fast
      default: col = strip.Color((uint8_t)(br * 0.6f), (uint8_t)(br * 0.8f), br); break; // cold blue
    }
    strip.setPixelColor(stars[i].led, col);
  }
  strip.show();
}

// ----------------------- FIRE / FLAME FLICKER -----------------------
#define MAX_FIRE_LEDS 300
uint8_t fireHeat[MAX_FIRE_LEDS];
int fireVariation = -1;

void initFireFlicker() {
  for (int i = 0; i < numLeds; i++) fireHeat[i] = 0;
  fireVariation = currentVariation;
}

uint32_t fireColor(int heat, int variation) {
  heat = constrain(heat, 0, 255);
  if (variation == 3) {   // gas-like blue flame
    if (heat < 85) {
      uint8_t b = (uint8_t)(heat * 2);
      return strip.Color(0, (uint8_t)(b * 0.4f), b);
    } else if (heat < 170) {
      uint8_t t = (uint8_t)((heat - 85) * 2);
      return strip.Color((uint8_t)(t * 0.4f), (uint8_t)(85 + t * 0.6f), 170);
    } else {
      uint8_t t = (uint8_t)((heat - 170) * 3);
      return strip.Color((uint8_t)(170 + t * 0.5f), 255, 255);
    }
  }
  if (variation == 4) {   // rainbow fire
    uint32_t c = wheel((uint8_t)(heat * 2));
    float s = 0.3f + heat / 255.0f * 0.7f;
    return strip.Color((uint8_t)((c >> 16 & 0xFF) * s),
                       (uint8_t)((c >> 8 & 0xFF) * s),
                       (uint8_t)((c & 0xFF) * s));
  }
  // classic fire palette: black -> dark red -> red -> orange -> yellow -> white
  if (heat < 60)  return strip.Color((uint8_t)(heat * 0.8f), 0, 0);
  if (heat < 120) return strip.Color(120, (uint8_t)((heat - 60) * 2), 0);
  if (heat < 200) return strip.Color((uint8_t)(120 + (heat - 120) * 1.7f),
                                     (uint8_t)(96 + (heat - 120) * 1.6f), 0);
  return strip.Color(255, 255, (uint8_t)((heat - 200) * 4.6f));
}

void updateFireFlicker() {
  int v = constrain(currentVariation, 0, 4);
  if (v != fireVariation) initFireFlicker();

  int base, sparking, cooling;
  switch (v) {
    case 0: base = numLeds / 3; sparking = 12; cooling = 8; break;    // candle
    case 1: base = numLeds / 2; sparking = 30; cooling = 10; break;   // campfire
    case 2: base = numLeds;     sparking = 55; cooling = 14; break;   // inferno
    case 3: base = numLeds / 2; sparking = 22; cooling = 9; break;    // blue flame
    default: base = numLeds / 2; sparking = 32; cooling = 10; break;  // rainbow fire
  }
  if (base < 3) base = 3;

  // cool everything
  for (int i = 0; i < numLeds; i++) {
    int cool = random(0, cooling + 2);
    fireHeat[i] = (fireHeat[i] > cool) ? (uint8_t)(fireHeat[i] - cool) : 0;
  }
  // heat drifts upward (LED 0 = bottom)
  for (int i = numLeds - 1; i >= 3; i--) {
    fireHeat[i] = (fireHeat[i-1] + fireHeat[i-2] + fireHeat[i-3]) / 3;
  }
  // ignite sparks near the base
  for (int i = 0; i < base; i++) {
    if (random(255) < sparking) {
      fireHeat[i] += random(40, 150);
      if (fireHeat[i] > 255) fireHeat[i] = 255;
    }
  }
  // keep the flame near its source (not for the inferno)
  if (v != 2) {
    for (int i = 3; i < numLeds; i++) {
      float atten = 1.0f - (float)(i - 2) / (base * 2.0f);
      if (atten <= 0) fireHeat[i] = 0;
      else fireHeat[i] = (uint8_t)(fireHeat[i] * atten);
    }
  }

  for (int i = 0; i < numLeds; i++) {
    strip.setPixelColor(i, fireColor(fireHeat[i], v));
  }
  strip.show();
}

// ----------------------- BOUNCING BALLS -----------------------
#define MAX_BALLS 5
#define BALL_TRAIL 6
struct Ball {
  float pos, vel;
  float radius;
  uint8_t hue;
  float trail[BALL_TRAIL];
};
Ball balls[MAX_BALLS];
int numBalls = 0;
int ballsVariation = -1;
float ballGravity = 0.1f;
int ballR[MAX_LEDS], ballG[MAX_LEDS], ballB[MAX_LEDS];
bool ballsSettled = false;         // true once all balls have come to rest
int ballsStillFrames = 0;          // consecutive frames of near-zero velocity
unsigned long ballsSettleTime = 0;
#define BALLS_STILL_FRAMES 30      // frames of near-zero velocity before "settled"
#define BALLS_RESTART_MS 5000      // pause after settling before restarting

void initBalls() {
  ballsSettled = false;
  ballsStillFrames = 0;
  int v = constrain(currentVariation, 0, 4);
  int counts[] = { 1, 2, 3, 4, 5 };
  numBalls = counts[v];
  ballGravity = (v == 0) ? 0.04f : 0.12f;
  for (int i = 0; i < numBalls; i++) {
    balls[i].pos = random(0, numLeds * 10) / 10.0f;
    balls[i].vel = random(-15, 15) / 10.0f;
    balls[i].radius = (v == 2) ? (1 + random(0, 3)) : 1.0f;
    balls[i].hue = (v == 4) ? (uint8_t)(i * 51) : (uint8_t)random(0, 256);
    for (int t = 0; t < BALL_TRAIL; t++) balls[i].trail[t] = balls[i].pos;
  }
  ballsVariation = v;
}

void updateBouncingBalls() {
  int v = constrain(currentVariation, 0, 4);
  if (v != ballsVariation) initBalls();

  // physics: gravity + bounces at both ends
  float maxPos = numLeds - 1.0f;
  for (int i = 0; i < numBalls; i++) {
    for (int t = BALL_TRAIL - 1; t > 0; t--) balls[i].trail[t] = balls[i].trail[t-1];
    balls[i].trail[0] = balls[i].pos;

    balls[i].vel += ballGravity;
    balls[i].pos += balls[i].vel;
    if (balls[i].pos < balls[i].radius) {
      balls[i].pos = balls[i].radius;
      balls[i].vel = -balls[i].vel * 0.85f;
    }
    if (balls[i].pos > maxPos - balls[i].radius) {
      balls[i].pos = maxPos - balls[i].radius;
      balls[i].vel = -balls[i].vel * 0.85f;
    }
    if (v == 0 && random(100) < 40) balls[i].hue += 1;   // slow hue cycle
  }

  // settle detection: once every ball has come to rest, hold the final frame
  // for a pause, then re-launch the balls for another round
  if (!ballsSettled) {
    bool allStill = true;
    for (int i = 0; i < numBalls; i++) {
      if (fabs(balls[i].vel) > 0.1f) { allStill = false; break; }
    }
    if (allStill) {
      if (++ballsStillFrames >= BALLS_STILL_FRAMES) {
        ballsSettled = true;
        ballsSettleTime = millis();
      }
    } else {
      ballsStillFrames = 0;
    }
  } else if (millis() - ballsSettleTime >= BALLS_RESTART_MS) {
    initBalls();
  }

  // elastic collisions (equal mass -> swap velocities)
  if (v == 4) {
    for (int i = 0; i < numBalls; i++) {
      for (int j = i + 1; j < numBalls; j++) {
        float dist = fabs(balls[i].pos - balls[j].pos);
        float minD = balls[i].radius + balls[j].radius;
        if (dist < minD) {
          float tmp = balls[i].vel; balls[i].vel = balls[j].vel; balls[j].vel = tmp;
          float mid = (balls[i].pos + balls[j].pos) / 2.0f;
          balls[i].pos = mid - minD / 2.0f;
          balls[j].pos = mid + minD / 2.0f;
        }
      }
    }
  }

  // render with additive blending (colors mix where balls overlap)
  for (int j = 0; j < numLeds; j++) { ballR[j] = 0; ballG[j] = 0; ballB[j] = 0; }
  for (int i = 0; i < numBalls; i++) {
    uint32_t col = (v == 3) ? strip.Color(255, 255, 255) : wheel(balls[i].hue);
    uint8_t cr = (col >> 16) & 0xFF, cg = (col >> 8) & 0xFF, cb = col & 0xFF;
    float sigma = (v == 3) ? 1.6f : (balls[i].radius + 0.5f);
    int win = (int)(sigma * 3.2f) + 2;
    int c0 = max(0, (int)balls[i].pos - win), c1 = min(numLeds - 1, (int)balls[i].pos + win);
    for (int j = c0; j <= c1; j++) {
      float dist = j - balls[i].pos;
      float intensity = exp(-(dist * dist) / (2.0f * sigma * sigma));
      ballR[j] += (int)(cr * intensity);
      ballG[j] += (int)(cg * intensity);
      ballB[j] += (int)(cb * intensity);
    }
    // motion blur trail for variation 3
    if (v == 3) {
      for (int t = 1; t < BALL_TRAIL; t++) {
        float br = (1.0f - t / (float)BALL_TRAIL) * 0.5f;
        int tp = (int)balls[i].trail[t];
        if (tp < 0 || tp >= numLeds) continue;
        ballR[tp] += (int)(255 * br);
        ballG[tp] += (int)(255 * br);
        ballB[tp] += (int)(255 * br);
      }
    }
  }
  for (int j = 0; j < numLeds; j++) {
    strip.setPixelColor(j, strip.Color(constrain(ballR[j], 0, 255),
                                       constrain(ballG[j], 0, 255),
                                       constrain(ballB[j], 0, 255)));
  }
  strip.show();
}

// ----------------------- LIGHTNING STORM -----------------------
enum LightningState { LS_IDLE, LS_FLASH, LS_ROLL, LS_AFTERGLOW };
LightningState ls = LS_IDLE;
unsigned long lsTimer = 0;
float lsRollPos = 0;
int lsSegStart = 0, lsSegLen = 0;

void updateLightning() {
  int v = constrain(currentVariation, 0, 4);
  unsigned long now = millis();

  switch (ls) {
    case LS_IDLE: {
      unsigned long pause;
      switch (v) {
        case 0: pause = random(1000, 3000); break;      // single flash, 1-3s pauses
        case 1: pause = random(250, 900); break;        // random segments
        case 2: pause = random(500, 1400); break;       // rolling
        case 3: pause = random(400, 1100); break;       // violet
        default: pause = random(60, 180); break;        // frequent storm
      }
      lsTimer = now;
      if (v == 2) {
        ls = LS_ROLL;
        lsRollPos = 0;
      } else {
        ls = LS_FLASH;
        if (v == 1) {
          lsSegLen = min(numLeds, (int)random(20, 41));
          lsSegStart = (lsSegLen >= numLeds) ? 0 : random(0, numLeds - lsSegLen + 1);
        } else {
          lsSegStart = 0; lsSegLen = numLeds;
        }
      }
      break;
    }
    case LS_FLASH:
      if (now - lsTimer > 70) { ls = LS_AFTERGLOW; lsTimer = now; }
      break;
    case LS_ROLL:
      if (now - lsTimer > 25) {
        lsTimer = now;
        lsRollPos += random(3, 8);
        if (lsRollPos >= numLeds + 3) { ls = LS_AFTERGLOW; lsTimer = now; }
      }
      break;
    case LS_AFTERGLOW:
      if (now - lsTimer > 500) ls = LS_IDLE;
      break;
  }

  strip.clear();
  switch (ls) {
    case LS_FLASH: {
      if (random(100) < 60) {   // flickering burst
        uint32_t col = (v == 3) ? strip.Color(150, 130, 255) : strip.Color(255, 255, 255);
        for (int i = lsSegStart; i < lsSegStart + lsSegLen; i++) {
          if (i < 0 || i >= numLeds) continue;
          strip.setPixelColor(i, col);
        }
      }
      break;
    }
    case LS_ROLL: {
      uint32_t col = (v == 3) ? strip.Color(160, 140, 255) : strip.Color(255, 255, 255);
      int h = (int)lsRollPos;
      for (int i = max(0, h - 3); i <= min(numLeds - 1, h + 3); i++) {
        float dist = abs(i - lsRollPos);
        float br = 1.0f - dist / 4.0f;
        if (br <= 0) continue;
        strip.setPixelColor(i, strip.Color((uint8_t)(((col >> 16) & 0xFF) * br),
                                           (uint8_t)(((col >> 8) & 0xFF) * br),
                                           (uint8_t)((col & 0xFF) * br)));
      }
      break;
    }
    case LS_AFTERGLOW: {
      float t = (now - lsTimer) / 500.0f;
      float glow = (1.0f - t);
      glow *= glow;
      uint8_t br = (uint8_t)(glow * 120);
      uint32_t col = (v == 3) ? strip.Color((uint8_t)(br * 0.6f), (uint8_t)(br * 0.5f), br)
                              : strip.Color(br / 2, br / 2, br);
      for (int i = 0; i < numLeds; i++) strip.setPixelColor(i, col);
      break;
    }
    default: break;
  }
  strip.show();
}

// ----------------------- KALEIDOSCOPE -----------------------
float kalPhase = 0;
int kalVariation = -1;
float kalStarPhase[MAX_LEDS];
uint8_t kalStarSpeed[MAX_LEDS];

void initKaleidoscope() {
  kalVariation = currentVariation;
  kalPhase = 0;
  for (int i = 0; i < numLeds; i++) {
    kalStarPhase[i] = random(0, 628) / 100.0f;
    kalStarSpeed[i] = random(20, 70);
  }
}

uint32_t kalBaseColor(int baseIdx, int segLen, int v) {
  float t = (float)baseIdx / segLen;   // 0..1 within the base segment
  switch (v) {
    case 0: {   // slowly moving rainbow grain
      return wheel((uint8_t)(t * 255 + kalPhase * 40));
    }
    case 1: {   // twinkling stars mirrored perfectly
      kalStarPhase[baseIdx] += kalStarSpeed[baseIdx] / 1000.0f;
      float tw = (sin(kalStarPhase[baseIdx]) + 1.0f) / 2.0f;
      uint8_t b = (uint8_t)(20 + tw * tw * 235);
      return strip.Color(b, b, (uint8_t)(b * 0.7f));
    }
    case 2: {   // plasma-like noise repeated
      float n = sin(t * 18.0f + kalPhase * 2.0f) * sin(t * 7.0f - kalPhase)
              + sin(t * 31.0f + kalPhase * 1.4f) * 0.5f;
      float nn = (n + 1.5f) / 3.0f;
      return wheel((uint8_t)(nn * 255));
    }
    case 3: {   // mirror + rotation, moving colour washes
      int rot = (int)(kalPhase * 20) % segLen;
      int idx = (baseIdx + rot) % segLen;
      return wheel((uint8_t)((float)idx / segLen * 255 + kalPhase * 80));
    }
    default: {  // laser-show: live pattern, mirrored outward
      return wheel((uint8_t)(t * 255 + kalPhase * 60));
    }
  }
}

void updateKaleidoscope() {
  int v = constrain(currentVariation, 0, 4);
  if (v != kalVariation) initKaleidoscope();
  kalPhase += 0.01f;
  if (kalPhase > 1000) kalPhase -= 1000;

  int fold = 4;
  switch (v) {
    case 0: fold = 2; break;
    case 1: fold = 4; break;
    case 2: fold = 8; break;
    case 3: fold = 4; break;
  }
  int segLen = numLeds / fold;
  if (segLen < 1) segLen = 1;

  strip.clear();
  for (int i = 0; i < numLeds; i++) {
    int b;
    if (v == 4) {
      // laser-show: the centre half is live, outer quarters mirror it
      int c = numLeds / 4;
      int ce = c + numLeds / 2 - 1;
      if (i < c)         b = 2 * c - 1 - i;
      else if (i <= ce)  b = i;
      else               b = 2 * (c + numLeds / 2) - 1 - i;
      segLen = ce - c + 1;
      b -= c;
    } else {
      int seg = i / segLen;
      int local = i - seg * segLen;
      if (seg % 2 == 1) local = segLen - 1 - local;   // alternate segments mirror
      b = local;
    }
    strip.setPixelColor(i, kalBaseColor(b, segLen, v));
  }
  strip.show();
}

// ----------------------- COLLIDING FILL -----------------------
// Game-like fill: two coloured pixels race from the outermost empty LEDs,
// collide in the middle, then park (per variation) until the whole strip is
// full, then the effect resets. One LED step every 50 ms.
//   Var 0 Classic Fill    - both return to their starting sides, fill ends inward
//   Var 1 Colour Swap     - colours exchange at the collision, then return
//   Var 2 Teleport Fill   - on collision they warp to the next empty edge slots
//   Var 3 Random Side     - each pixel picks a random side, parks at nearest free spot
//   Var 4 Center-Out Fill - they stop right at the collision point, block grows outward
#define CF_STEP_MS       50      // one LED step every 50 ms
#define CF_FLASH_TICKS   2       // white flash ticks at the collision point
#define CF_FINAL_TICKS   6       // blink ticks for the final single pixel
#define CF_FULL_HOLD_MS  2000    // hold the full strip before resetting

enum CFPhase { CF_RUN, CF_COLLIDE, CF_RETURN, CF_FINAL, CF_FULL_HOLD };

struct CollidingFill {
  bool parked[MAX_LEDS];
  uint32_t parkedCol[MAX_LEDS];
  int leftPos, rightPos;          // runner positions (-1 = inactive)
  int originL, originR;           // spawn (edge) slots of the current pair
  uint32_t leftCol, rightCol;     // runner colours
  int targetL, targetR;           // parking targets during CF_RETURN
  bool targetSetL, targetSetR;
  bool doneL, doneR;              // return-trip completion per runner
  CFPhase phase;
  int flashTicks;
  int mode;                       // cached variation
  unsigned long lastStep, fullSince;
  bool active;
};
CollidingFill cf;

int cfLeftmostEmpty() {
  for (int i = 0; i < numLeds; i++) if (!cf.parked[i]) return i;
  return -1;
}

int cfRightmostEmpty() {
  for (int i = numLeds - 1; i >= 0; i--) if (!cf.parked[i]) return i;
  return -1;
}

// First empty LED scanning from pos in the given direction (-1 left, +1 right).
int cfNearestFree(int pos, int dir) {
  for (int i = pos + dir; i >= 0 && i < numLeds; i += dir) {
    if (!cf.parked[i]) return i;
  }
  return -1;
}

void cfPark(int pos, uint32_t col) {
  if (pos < 0 || pos >= numLeds) return;
  cf.parked[pos] = true;
  cf.parkedCol[pos] = col;
}

void cfSpawnNextPair(bool keepColors) {
  int L = cfLeftmostEmpty();
  int R = cfRightmostEmpty();
  if (L < 0) {                    // strip completely full
    cf.phase = CF_FULL_HOLD;
    cf.fullSince = millis();
    return;
  }
  if (L == R) {                   // one LED left: final single-pixel round
    cf.phase = CF_FINAL;
    cf.leftPos = L;
    cf.rightPos = -1;
    cf.flashTicks = CF_FINAL_TICKS;
    cf.leftCol = wheel((uint8_t)random(0, 256));
    return;
  }
  cf.originL = L;
  cf.originR = R;
  cf.leftPos = L;
  cf.rightPos = R;
  if (!keepColors) {              // fresh pair, complementary colours
    uint8_t hue = (uint8_t)random(0, 256);
    cf.leftCol = wheel(hue);
    cf.rightCol = wheel((hue + 128) % 256);
  }
  cf.targetSetL = cf.targetSetR = false;
  cf.doneL = cf.doneR = false;
  cf.phase = CF_RUN;
}

void initCollidingFill() {
  for (int i = 0; i < numLeds; i++) {
    cf.parked[i] = false;
    cf.parkedCol[i] = 0;
  }
  cf.phase = CF_RUN;
  cf.flashTicks = 0;
  cf.mode = constrain(currentVariation, 0, 4);
  cf.lastStep = millis();
  cf.fullSince = 0;
  cf.active = true;
  cf.leftPos = cf.rightPos = -1;
  cf.targetSetL = cf.targetSetR = false;
  cf.doneL = cf.doneR = false;
  cfSpawnNextPair(false);
}

// Move one runner toward its parking target; parks it on arrival.
// Returns true once the runner is parked.
bool cfMoveToTarget(int &pos, uint32_t &col, int v, bool isLeft) {
  int &target = isLeft ? cf.targetL : cf.targetR;
  bool &set   = isLeft ? cf.targetSetL : cf.targetSetR;

  if (!set) {
    if (v == 3) {                 // random side: nearest free spot either way
      int dir = random(2) ? 1 : -1;
      target = cfNearestFree(pos, dir);
      if (target < 0) target = cfNearestFree(pos, -dir);
    } else {                      // classic / swap: back to the pixel's own side
      target = isLeft ? cfLeftmostEmpty() : cfRightmostEmpty();
    }
    set = true;
  }
  if (target < 0) return false;
  if (cf.parked[target]) { set = false; return false; }   // spot taken, re-pick
  if (pos == target) { cfPark(pos, col); return true; }

  int step = (target > pos) ? 1 : -1;
  if (cf.parked[pos + step]) pos = target;   // hop over parked LEDs (var 3)
  else pos += step;
  return false;
}

void cfStep(int v) {
  switch (cf.phase) {
    case CF_RUN: {
      int nextL = cf.leftPos + 1;
      int nextR = cf.rightPos - 1;
      bool blockedL = nextL < numLeds && cf.parked[nextL];
      bool blockedR = nextR >= 0 && cf.parked[nextR];
      if (nextL >= nextR || blockedL || blockedR) {
        cf.phase = CF_COLLIDE;
        cf.flashTicks = CF_FLASH_TICKS;
        if (v == 1) {             // colour swap at the moment of collision
          uint32_t t = cf.leftCol; cf.leftCol = cf.rightCol; cf.rightCol = t;
        }
      } else {
        cf.leftPos = nextL;
        cf.rightPos = nextR;
      }
      break;
    }
    case CF_COLLIDE:
      if (--cf.flashTicks <= 0) {
        if (v == 2) {             // teleport: park the edge slots, warp inward
          cfPark(cf.originL, cf.leftCol);
          cfPark(cf.originR, cf.rightCol);
          cfSpawnNextPair(true);   // same pair continues from the new edge slots
        } else if (v == 4) {      // centre-out: stop right where they are
          cfPark(cf.leftPos, cf.leftCol);
          cfPark(cf.rightPos, cf.rightCol);
          cfSpawnNextPair(false);
        } else {                  // classic / swap / random: return trip
          cf.phase = CF_RETURN;
          cf.targetSetL = cf.targetSetR = false;
          cf.doneL = cf.doneR = false;
        }
      }
      break;
    case CF_RETURN: {
      if (!cf.doneL) cf.doneL = cfMoveToTarget(cf.leftPos, cf.leftCol, v, true);
      if (!cf.doneR) cf.doneR = cfMoveToTarget(cf.rightPos, cf.rightCol, v, false);
      if (cf.doneL && cf.doneR) {
        cf.doneL = cf.doneR = false;
        cfSpawnNextPair(false);
      }
      break;
    }
    case CF_FINAL:
      if (--cf.flashTicks <= 0) {
        cfPark(cf.leftPos, cf.leftCol);
        cf.phase = CF_FULL_HOLD;
        cf.fullSince = millis();
      }
      break;
    case CF_FULL_HOLD:
      if (millis() - cf.fullSince >= CF_FULL_HOLD_MS) initCollidingFill();
      break;
  }
}

void cfRender() {
  strip.clear();
  for (int i = 0; i < numLeds; i++) {
    if (cf.parked[i]) strip.setPixelColor(i, cf.parkedCol[i]);
  }
  if (cf.phase == CF_RUN || cf.phase == CF_COLLIDE || cf.phase == CF_RETURN) {
    if (cf.leftPos >= 0) strip.setPixelColor(cf.leftPos, cf.leftCol);
    if (cf.rightPos >= 0) strip.setPixelColor(cf.rightPos, cf.rightCol);
  } else if (cf.phase == CF_FINAL) {
    if (cf.flashTicks % 2 == 1) strip.setPixelColor(cf.leftPos, cf.leftCol);  // blink
  }
  if (cf.phase == CF_COLLIDE) {   // white flash at the impact point
    int c = (cf.leftPos + cf.rightPos) / 2;
    for (int i = max(0, c - 1); i <= min(numLeds - 1, c + 1); i++) {
      strip.setPixelColor(i, strip.Color(255, 255, 255));
    }
  }
  strip.show();
}

void updateCollidingFill() {
  int v = constrain(currentVariation, 0, 4);
  if (!cf.active || v != cf.mode) initCollidingFill();

  unsigned long now = millis();
  if (now - cf.lastStep >= CF_STEP_MS) {
    cf.lastStep = now;
    cfStep(v);
  }
  cfRender();
}

// Set an LED to a colour scaled by a 0..1 factor. Used by the newer effects
// so their brightness falloff stays consistent with the older inline loops.
void setPixelScaled(int idx, uint32_t col, float scale) {
  if (idx < 0 || idx >= numLeds || scale <= 0.0f) return;
  if (scale > 1.0f) scale = 1.0f;
  uint8_t r = (uint8_t)(((col >> 16) & 0xFF) * scale);
  uint8_t g = (uint8_t)(((col >> 8) & 0xFF) * scale);
  uint8_t b = (uint8_t)((col & 0xFF) * scale);
  strip.setPixelColor(idx, strip.Color(r, g, b));
}

// ----------------------- PAINT SPLAT -----------------------
// Two paint blobs (small clusters of LEDs) start at the outermost empty
// pixels and move inward every 50 ms. On collision they merge into a single
// "splat" that expands outward until the strip is full, then the effect
// resets with a fresh pair of colours.
//   Var 0 Classic Mix     - blobs blend (average colour), even spread
//   Var 1 Splash Burst    - fast one-shot wave to the ends, then solid
//   Var 2 Lava Lamp Splat - organic, pulsating outward bursts (thick paint)
//   Var 3 Rainbow Splat   - original colours spread as alternating bands
//   Var 4 Galactic Splat  - dark sky with slowly filling stars, then hue
#define PS_STEP_MS      50
#define PS_FILL_HOLD_MS 1500
#define PS_CLUSTER      3

enum PSPhase { PS_INWARD, PS_COLLIDE, PS_OUTWARD, PS_FILLED };

struct PaintSplat {
  int leftHead, rightHead;         // inward-moving blob positions
  uint32_t leftCol, rightCol, mixedCol;
  PSPhase phase;
  int flashTicks;
  int mode;                        // cached variation
  unsigned long lastStep, filledSince, outwardSince;
  float outwardProgress;           // 0..1 while the splat spreads
  uint8_t starBright[MAX_LEDS];    // galactic variation: star fill levels
  uint8_t starTarget[MAX_LEDS];
  bool starPhase;
  bool active;
};
PaintSplat ps;

void initPaintSplat() {
  for (int i = 0; i < numLeds; i++) {
    ps.starBright[i] = 0;
    // ~1 in 4 pixels becomes a star with a random peak, rest stay dark
    ps.starTarget[i] = (random(4) == 0) ? (uint8_t)random(120, 256) : 0;
  }
  ps.mode = constrain(currentVariation, 0, 4);
  ps.phase = PS_INWARD;
  ps.flashTicks = 0;
  ps.lastStep = millis();
  ps.filledSince = 0;
  ps.outwardSince = 0;
  ps.outwardProgress = 0;
  ps.starPhase = true;
  ps.active = true;
  ps.leftHead = PS_CLUSTER - 1;
  ps.rightHead = numLeds - PS_CLUSTER;
  if (ps.rightHead < ps.leftHead + 1) ps.rightHead = ps.leftHead + 1;  // tiny strips
  uint8_t hue = (uint8_t)random(0, 256);
  ps.leftCol = wheel(hue);
  ps.rightCol = wheel((hue + 128) % 256);
  ps.mixedCol = lerpColor(ps.leftCol, ps.rightCol, 0.5f);
}

void psStep(int v) {
  switch (ps.phase) {
    case PS_INWARD:
      if (ps.leftHead + 1 >= ps.rightHead) {
        ps.phase = PS_COLLIDE;
        ps.flashTicks = 3;
      } else {
        ps.leftHead++;
        ps.rightHead--;
      }
      break;
    case PS_COLLIDE:
      if (--ps.flashTicks <= 0) {
        ps.phase = PS_OUTWARD;
        ps.outwardProgress = 0;
        ps.outwardSince = millis();
      }
      break;
    case PS_OUTWARD:
      if (v == 1) {
        ps.outwardProgress += 0.08f;                     // fast one-shot wave
      } else if (v == 2) {
        // organic advance: each tick only ~80% chance the paint creeps on
        if (random(100) < 80) ps.outwardProgress += 1.0f / (numLeds / 2.0f);
      } else if (v == 4) {
        if (ps.starPhase) {
          if (millis() - ps.outwardSince > 2200) ps.starPhase = false;
        } else {
          ps.outwardProgress += 0.06f;                   // hue sweep after stars
        }
      } else {
        ps.outwardProgress += 0.03f;                     // classic / rainbow
      }
      if (ps.outwardProgress >= 1.0f) {
        ps.outwardProgress = 1.0f;
        ps.phase = PS_FILLED;
        ps.filledSince = millis();
      }
      break;
    case PS_FILLED:
      if (millis() - ps.filledSince >= PS_FILL_HOLD_MS) initPaintSplat();
      break;
  }
}

void psRender() {
  strip.clear();
  int c = numLeds / 2;

  if (ps.phase == PS_INWARD || ps.phase == PS_COLLIDE) {
    // left blob: cluster tapers toward the tail, plus a faint trail
    for (int t = 0; t < PS_CLUSTER; t++) {
      int idx = ps.leftHead - (PS_CLUSTER - 1 - t);
      float b = (PS_CLUSTER == 1) ? 1.0f : 0.55f + 0.45f * (float)t / (PS_CLUSTER - 1);
      setPixelScaled(idx, ps.leftCol, b);
    }
    for (int t = 1; t <= 3; t++) {
      setPixelScaled(ps.leftHead - PS_CLUSTER - t + 1, ps.leftCol, 0.16f - t * 0.04f);
    }
    // right blob, mirrored
    for (int t = 0; t < PS_CLUSTER; t++) {
      int idx = ps.rightHead + (PS_CLUSTER - 1 - t);
      float b = (PS_CLUSTER == 1) ? 1.0f : 0.55f + 0.45f * (float)t / (PS_CLUSTER - 1);
      setPixelScaled(idx, ps.rightCol, b);
    }
    for (int t = 1; t <= 3; t++) {
      setPixelScaled(ps.rightHead + PS_CLUSTER - 1 + t, ps.rightCol, 0.16f - t * 0.04f);
    }
    if (ps.phase == PS_COLLIDE) {       // white flash at the impact point
      for (int i = max(0, c - 1); i <= min(numLeds - 1, c + 1); i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255));
      }
    }
  }
  else if (ps.phase == PS_OUTWARD) {
    if (ps.mode == 2) {
      // lava lamp: travelling brightness ripples inside the growing blob
      int span = (int)(ps.outwardProgress * (numLeds / 2.0f));
      int l0 = max(0, c - span), r1 = min(numLeds - 1, c + span);
      for (int i = l0; i <= r1; i++) {
        float ripple = 0.5f + 0.5f * sin(i * 0.7f - millis() * 0.012f);
        setPixelScaled(i, ps.mixedCol, 0.45f + 0.55f * ripple);
      }
      setPixelScaled(l0, ps.mixedCol, 1.0f);   // wet-paint edges glow brightest
      setPixelScaled(r1, ps.mixedCol, 1.0f);
    } else if (ps.mode == 4) {
      if (ps.starPhase) {   // dark background, stars slowly fill in
        for (int i = 0; i < numLeds; i++) {
          if (ps.starTarget[i] == 0) continue;
          if (ps.starBright[i] < ps.starTarget[i]) {
            ps.starBright[i] = (uint8_t)min((int)ps.starTarget[i], (int)(ps.starBright[i] + 1 + random(0, 3)));
          }
          uint8_t b = (random(400) == 0) ? ps.starTarget[i] : ps.starBright[i];  // twinkle
          strip.setPixelColor(i, strip.Color(b, (uint8_t)(b * 0.9f), (uint8_t)(b * 0.7f)));
        }
      } else {              // hue sweep fills the strip
        int span = (int)(ps.outwardProgress * (numLeds / 2.0f));
        int l0 = max(0, c - span), r1 = min(numLeds - 1, c + span);
        for (int i = l0; i <= r1; i++) strip.setPixelColor(i, ps.mixedCol);
      }
    } else {
      int span = (int)(ps.outwardProgress * (numLeds / 2.0f));
      int l0 = max(0, c - span), r1 = min(numLeds - 1, c + span);
      for (int i = l0; i <= r1; i++) {
        uint32_t col = (ps.mode == 3) ? ((((i - c) & 1) == 0) ? ps.leftCol : ps.rightCol)
                                      : ps.mixedCol;
        strip.setPixelColor(i, col);
      }
      if (ps.mode != 3) {   // soft leading edge on the spreading paint
        setPixelScaled(l0, ps.mixedCol, 0.9f);
        setPixelScaled(r1, ps.mixedCol, 0.9f);
      }
    }
  }
  else if (ps.phase == PS_FILLED) {
    for (int i = 0; i < numLeds; i++) {
      uint32_t col = (ps.mode == 3) ? ((((i - c) & 1) == 0) ? ps.leftCol : ps.rightCol)
                                    : ps.mixedCol;
      strip.setPixelColor(i, col);
    }
  }

  strip.show();
}

void updatePaintSplat() {
  int v = constrain(currentVariation, 0, 4);
  if (!ps.active || v != ps.mode) initPaintSplat();

  unsigned long now = millis();
  if (now - ps.lastStep >= PS_STEP_MS) {
    ps.lastStep = now;
    psStep(v);
  }
  psRender();
}

// ----------------------- SNAKE -----------------------
// Autonomous 1-D snake: the head wraps around the strip ends, grows by
// eating food, and dies when it bites its own tail (or a wall in var 3).
// There is no user input - the head keeps moving in its current direction
// until the round ends, then the game flashes red and restarts.
//   Var 0 Classic Green    - green snake, red food
//   Var 1 Speed Boost      - eating food sometimes halves the step delay
//   Var 2 Rainbow Snake    - hue cycles continuously; food is white
//   Var 3 Obstacle Course  - static red walls; hitting one kills the snake
//   Var 4 Poison Food      - some pellets are dim blue poison (-3 segments)
#define SNAKE_STEP_MS      50
#define SNAKE_BOOST_MS     4000
#define SNAKE_DEATH_TICKS  8

struct Snake {
  int body[MAX_LEDS];          // head at [0], tail at [len-1]
  int len;
  int dir;                     // +1 right, -1 left
  int food;
  bool foodPoison;
  int walls[MAX_LEDS / 4];     // static red walls (var 3)
  int numWalls;
  bool speedBoost;
  unsigned long boostUntil;
  unsigned long lastStep;
  int deathTicks;              // >0: flashing red before respawn
  int eatFlash;                // >0: white flash at the eaten cell
  int eatenAt;
  uint8_t hueOffset;
  int mode;
  bool active;
};
Snake snake;

bool snakeOccupied(int p) {
  for (int i = 0; i < snake.len; i++) if (snake.body[i] == p) return true;
  for (int i = 0; i < snake.numWalls; i++) if (snake.walls[i] == p) return true;
  return false;
}

// Food is placed on a random empty LED, biased toward the arc ahead of the
// head so the snake actually meets it on its current lap.
void snakePlaceFood() {
  for (int tries = 0; tries < 20; tries++) {
    int p;
    if (random(100) < 65) {
      int offset = 1 + random(max(1, numLeds / 2));
      p = ((snake.body[0] + offset * snake.dir) % numLeds + numLeds) % numLeds;
    } else {
      p = random(numLeds);
    }
    if (!snakeOccupied(p)) {
      snake.food = p;
      snake.foodPoison = (snake.mode == 4 && random(100) < 25);
      return;
    }
  }
  for (int i = 0; i < numLeds; i++) {      // fallback: first empty LED
    if (!snakeOccupied(i)) {
      snake.food = i;
      snake.foodPoison = (snake.mode == 4 && random(100) < 25);
      return;
    }
  }
  snake.food = -1;                          // strip full, game ends soon
}

void initSnake() {
  snake.mode = constrain(currentVariation, 0, 4);
  snake.dir = random(2) ? 1 : -1;
  snake.len = min(4, max(1, numLeds / 4));
  int start = numLeds / 2;
  for (int i = 0; i < snake.len; i++) {
    snake.body[i] = ((start - i * snake.dir) % numLeds + numLeds) % numLeds;
  }
  snake.numWalls = 0;
  if (snake.mode == 3) {                    // scatter a few red walls
    int want = min(3, max(0, numLeds / 8));
    for (int w = 0; w < want; w++) {
      int p = random(numLeds), tries = 0;
      while ((snakeOccupied(p) || p == snake.body[0]) && tries++ < 30) p = random(numLeds);
      snake.walls[snake.numWalls++] = p;
    }
  }
  snake.speedBoost = false;
  snake.boostUntil = 0;
  snake.deathTicks = 0;
  snake.eatFlash = 0;
  snake.hueOffset = (uint8_t)random(0, 256);
  snake.lastStep = millis();
  snake.active = true;
  snakePlaceFood();
}

void snakeStep(int v) {
  if (snake.deathTicks > 0) {               // death flash countdown
    if (--snake.deathTicks <= 0) initSnake();
    return;
  }
  if (snake.eatFlash > 0) snake.eatFlash--;

  int head = snake.body[0];
  int nh = head + snake.dir;
  nh = ((nh % numLeds) + numLeds) % numLeds;   // wrap around the strip

  bool wallHit = false;
  if (v == 3) for (int i = 0; i < snake.numWalls; i++) if (snake.walls[i] == nh) wallHit = true;

  // Self-collision: the tail vacates its cell unless we grow this step.
  bool growing = (nh == snake.food);
  int limit = growing ? snake.len : snake.len - 1;
  bool bite = false;
  for (int i = 0; i < limit; i++) if (snake.body[i] == nh) { bite = true; break; }

  if (wallHit || bite) {
    snake.deathTicks = SNAKE_DEATH_TICKS;
    return;
  }

  // move: new head in front, tail follows (or stays when growing)
  for (int i = snake.len - 1; i > 0; i--) snake.body[i] = snake.body[i - 1];
  snake.body[0] = nh;
  if (growing) {
    if (snake.len < numLeds - 1) snake.len++;
    snake.eatFlash = 2;
    snake.eatenAt = nh;
    if (v == 1 && random(100) < 30) {       // occasional speed boost
      snake.speedBoost = true;
      snake.boostUntil = millis() + SNAKE_BOOST_MS;
    }
    if (v == 4 && snake.foodPoison) {       // poison shrinks the snake
      int remove = min(3, snake.len - 1);
      snake.len -= remove;
      if (snake.len <= 1) {                 // too short -> reset
        snake.deathTicks = SNAKE_DEATH_TICKS;
        return;
      }
    }
    snakePlaceFood();
  }
}

void snakeRender() {
  strip.clear();
  int v = snake.mode;

  if (snake.food >= 0) {
    if (v == 2)            strip.setPixelColor(snake.food, strip.Color(255, 255, 255));
    else if (snake.foodPoison) strip.setPixelColor(snake.food, strip.Color(20, 20, 150));
    else                   strip.setPixelColor(snake.food, strip.Color(255, 40, 40));
  }
  if (v == 3) {
    for (int i = 0; i < snake.numWalls; i++) {
      strip.setPixelColor(snake.walls[i], strip.Color(200, 30, 30));
    }
  }

  if (snake.deathTicks > 0) {               // whole snake flashes red
    for (int i = 0; i < snake.len; i++) strip.setPixelColor(snake.body[i], strip.Color(255, 0, 0));
    strip.show();
    return;
  }

  for (int i = 0; i < snake.len; i++) {
    uint32_t col = (v == 2) ? wheel((uint8_t)(millis() / 25 + snake.hueOffset))
                            : strip.Color(0, 255, 0);
    if (i == 0) strip.setPixelColor(snake.body[0], col);       // bright head
    else        setPixelScaled(snake.body[i], col, 0.5f);      // dimmer body
  }
  if (snake.eatFlash > 0) strip.setPixelColor(snake.eatenAt, strip.Color(255, 255, 255));

  strip.show();
}

void updateSnake() {
  int v = constrain(currentVariation, 0, 4);
  if (!snake.active || v != snake.mode) initSnake();

  unsigned long stepMs = SNAKE_STEP_MS;
  if (snake.speedBoost && millis() < snake.boostUntil) stepMs /= 2;

  unsigned long now = millis();
  if (now - snake.lastStep >= stepMs) {
    snake.lastStep = now;
    snakeStep(v);
  }
  snakeRender();
}

void resetEffectState() {
  freeStaticPixelBuffer();
  // Fireflies: clear all fireflies, they will respawn naturally
  for (int i = 0; i < MAX_FIREFLIES; i++) fireflies[i].active = false;
  nextFireflySpawn = millis() + 1000;

  // Comet
  comet.pos = 0;
  comet.speed = 0.4;
  comet.tailLen = 5;
  comet.hueOffset = 0;

  // Chasing Dots
  numChasers = 2 + currentVariation; // will be corrected in next update
  if (numChasers > MAX_CHASERS) numChasers = MAX_CHASERS;
  for (int i = 0; i < numChasers; i++) {
    chasers[i].pos = i * (numLeds / numChasers);
    chasers[i].hue = i * (256 / numChasers);
  }

  // Cylon
  cylonPhase = 0;

  // Dual Comet
  dualComet.leftPos = 0;
  dualComet.rightPos = numLeds - 1;
  dualComet.collided = false;
  dualComet.flashFrames = 0;

  // Sparkle Sweep
  sparkle.pos = 0;

  // Rainbow Gradient
  rainbowGrad.pos = -6;

  // Pulse Wave
  pulseWave.pos = -10;

  // Single Runner
  runner.pos = 0;
  runner.direction = 1;

  // Audio Visualizer - free and recreate next cycle
  if (bands) { delete[] bands; bands = nullptr; numBands = 0; }

  // Heartbeat - reset all phases
  hbPhase = HB_REST;
  hbLubTime = 0;
  hbDubTime = 0;
  hbRestStart = 0;
  hbNextBeat = millis();
  hbBreathPhase = 0;
  hbLeftActive = true;

  // Twinkle
  twinkleVariation = -1;
  numStars = 0;

  // Fire Flicker
  fireVariation = -1;
  for (int i = 0; i < numLeds; i++) fireHeat[i] = 0;

  // Bouncing Balls
  ballsVariation = -1;
  numBalls = 0;

  // Lightning Storm
  ls = LS_IDLE;
  lsTimer = 0;

  // Kaleidoscope
  kalVariation = -1;
  kalPhase = 0;

  // Colliding Fill
  cf.active = false;

  // Paint Splat
  ps.active = false;

  // Snake
  snake.active = false;
}

// ===========================  STATIC PIXEL PATTERN API ===========================

uint32_t* staticPixelBuffer = nullptr;

void freeStaticPixelBuffer() {
  if (staticPixelBuffer) {
    free(staticPixelBuffer);
    staticPixelBuffer = nullptr;
  }
}

uint32_t parseHexColor(const String& hex) {
  String h = hex;
  h.trim();
  if (h.length() == 3) {
    char r = h.charAt(0);
    char g = h.charAt(1);
    char b = h.charAt(2);
    h = String(r) + r + g + g + b + b;
  }
  long num = strtol(h.c_str(), NULL, 16);
  return strip.Color((num >> 16) & 0xFF, (num >> 8) & 0xFF, num & 0xFF);
}

uint32_t lerpColor(uint32_t c1, uint32_t c2, float t) {
  uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
  uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
  return strip.Color(
    (uint8_t)(r1 + (r2 - r1) * t),
    (uint8_t)(g1 + (g2 - g1) * t),
    (uint8_t)(b1 + (b2 - b1) * t)
  );
}

void applyBrightnessToBuffer(uint8_t brightness) {
  if (brightness >= 255) return;
  float scale = brightness / 255.0f;
  for (int i = 0; i < numLeds; i++) {
    uint32_t c = staticPixelBuffer[i];
    uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * scale);
    uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * scale);
    uint8_t b = (uint8_t)((c & 0xFF) * scale);
    staticPixelBuffer[i] = strip.Color(r, g, b);
  }
}

void generateSolidPattern(const String& colorStr) {
  freeStaticPixelBuffer();
  staticPixelBuffer = (uint32_t*)malloc(numLeds * sizeof(uint32_t));
  if (!staticPixelBuffer) return;
  uint32_t col = parseHexColor(colorStr);
  for (int i = 0; i < numLeds; i++) {
    staticPixelBuffer[i] = col;
  }
}

void generateStripedPattern(const String colors[], int numColors) {
  freeStaticPixelBuffer();
  staticPixelBuffer = (uint32_t*)malloc(numLeds * sizeof(uint32_t));
  if (!staticPixelBuffer || numColors < 1) return;
  uint32_t cols[10];
  for (int i = 0; i < numColors && i < 10; i++) {
    cols[i] = parseHexColor(colors[i]);
  }
  for (int i = 0; i < numLeds; i++) {
    staticPixelBuffer[i] = cols[i % numColors];
  }
}

void generateGradientPattern(const String colors[], int numColors) {
  freeStaticPixelBuffer();
  staticPixelBuffer = (uint32_t*)malloc(numLeds * sizeof(uint32_t));
  if (!staticPixelBuffer || numColors < 2) return;

  uint32_t cols[10];
  for (int i = 0; i < numColors && i < 10; i++) {
    cols[i] = parseHexColor(colors[i]);
  }

  int segments = numColors - 1;
  int ledsPerSegment = numLeds / segments;
  int remainder = numLeds % segments;
  int ledIdx = 0;

  for (int seg = 0; seg < segments; seg++) {
    int segLen = ledsPerSegment + (seg < remainder ? 1 : 0);
    uint32_t c1 = cols[seg];
    uint32_t c2 = cols[seg + 1];
    for (int j = 0; j < segLen; j++) {
      float t = (segLen > 1) ? (float)j / (segLen - 1) : 0.0f;
      if (ledIdx < numLeds) {
        staticPixelBuffer[ledIdx++] = lerpColor(c1, c2, t);
      }
    }
  }
}

void updateStaticPixel() {
  if (!staticPixelBuffer) return;
  for (int i = 0; i < numLeds; i++) {
    strip.setPixelColor(i, staticPixelBuffer[i]);
  }
  strip.show();
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

  // Fireflies
  for (int i=0; i<MAX_FIREFLIES; i++) fireflies[i].active=false;
  nextFireflySpawn=millis()+random(1000,10000);

  // Rainbow Swipe
  rainbowOffset = 0;

  // Aurora
  auroraPhase = 0;

  // Comet
  comet.pos=0; comet.speed=0.4; comet.tailLen=5; comet.hueOffset=0;

  // Chasing Dots
  numChasers=4;
  for (int i=0; i<numChasers; i++) {
    chasers[i].pos = i * (numLeds/numChasers);
    chasers[i].speed=0.25; chasers[i].hue = i*(256/numChasers);
  }

  // Cylon
  cylonPhase=0; cylonSpeed=0.03; cylonHue=0; cylonAtLeftEnd=cylonAtRightEnd=false;

  // Dual Comet
  dualComet.leftPos=0; dualComet.rightPos=numLeds-1; dualComet.leftSpeed=0.3; dualComet.rightSpeed=0.3;
  dualComet.tailLen=5; dualComet.collided=false; dualComet.flashFrames=0; dualComet.collisionStyle=0;

  // Sparkle Sweep
  sparkle.pos=-sparkle.halfWidth; sparkle.speed=0.5; sparkle.halfWidth=7; sparkle.density=0.3;

  // Police
  policeBlinkTimer = millis();

  // Plasma
  numBlobs=3;
  for (int i=0; i<numBlobs; i++) {
    blobs[i].phase=random(0,628)/100.0; blobs[i].speed=random(15,40)/1000.0;
    blobs[i].amplitude=random(10,numLeds/2); blobs[i].centerOffset=random(numLeds/4,3*numLeds/4);
    blobs[i].sigma=random(4,10); blobs[i].hue=random(0,256);
  }

  // Rainbow Gradient
  rainbowGrad.pos=-6; rainbowGrad.speed=0.6; rainbowGrad.hueOffset=0;

  // Pulse Wave
  pulseWave.pos=-10; pulseWave.sigma=4.0; pulseWave.speed=0.4; pulseWave.hueOffset=0;

  // Single Runner
  runner.pos=0; runner.speed=0.5; runner.direction=1; runner.bounce=false; runner.tailLen=2;

  // Audio Visualizer
  delete[] bands; bands=nullptr; numBands=0;

  // Heartbeat
  hbPhase=HB_REST; hbLubTime=0; hbDubTime=0; hbRestStart=0; hbNextBeat=millis();
  hbBreathPhase = 0; hbLeftActive = true;

  // Twinkle
  twinkleVariation = -1; numStars = 0;

  // Fire Flicker
  fireVariation = -1;
  for (int i = 0; i < numLeds; i++) fireHeat[i] = 0;

  // Bouncing Balls
  ballsVariation = -1; numBalls = 0;
  ballsSettled = false; ballsStillFrames = 0;

  // Lightning Storm
  ls = LS_IDLE; lsTimer = 0;

  // Kaleidoscope
  kalVariation = -1; kalPhase = 0;
  for (int i = 0; i < numLeds; i++) { kalStarPhase[i] = 0; kalStarSpeed[i] = 50; }

  // Colliding Fill
  cf.active = false;

  // Paint Splat
  ps.active = false;

  // Snake
  snake.active = false;

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
    switch (currentEffect) {
      case FIREFLIES:        updateFireflies(); break;
      case RAINBOW_SWIPE:    updateRainbowSwipe(); break;
      case AURORA:           updateAurora(); break;
      case COMET:            updateComet(); break;
      case CHASING_DOTS:     updateChasingDots(); break;
      case CYLON:            updateCylon(); break;
      case DUAL_COMET:       updateDualComet(); break;
      case SPARKLE_SWEEP:    updateSparkleSweep(); break;
      case POLICE:           updatePolice(); break;
      case PLASMA:           updatePlasma(); break;
      case RAINBOW_GRADIENT: updateRainbowGradient(); break;
      case PULSE_WAVE:       updatePulseWave(); break;
      case SINGLE_RUNNER:    updateSingleRunner(); break;
      case AUDIO_VISUALIZER: updateAudioVisualizer(); break;
      case HEARTBEAT:        updateHeartbeat(); break;
      case STATIC_PIXEL:       updateStaticPixel(); break;
      case TWINKLE:            updateTwinkle(); break;
      case FIRE_FLICKER:       updateFireFlicker(); break;
      case BOUNCING_BALLS:     updateBouncingBalls(); break;
      case LIGHTNING_STORM:    updateLightning(); break;
      case KALEIDOSCOPE:       updateKaleidoscope(); break;
      case COLLIDING_FILL:     updateCollidingFill(); break;
      case PAINT_SPLAT:        updatePaintSplat(); break;
      case SNAKE:              updateSnake(); break;
    }
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
