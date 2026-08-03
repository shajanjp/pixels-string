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
  NUM_EFFECTS
};

Effect currentEffect = FIREFLIES;
int currentVariation = 0;
const int variationsCount[NUM_EFFECTS] = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,1,5,5,5,5,5};

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
    helpResponse += "               BOUNCING_BALLS, LIGHTNING_STORM, KALEIDOSCOPE\n\n";
    helpResponse += "GET /api/effect?index=<0-20>\n";
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
enum HBPhase { FIRST_BEAT, SECOND_BEAT, PAUSE };
HBPhase hbPhase = PAUSE;
unsigned long hbTimer = 0;
float hbSigma = 2.0;
float hbPulse1Brightness = 0, hbPulse2Brightness = 0;
float hbPulse1Pos = 0, hbPulse2Pos = 0;
uint8_t hbHue = 0;
bool hbDual = false, hbEcho = false;
unsigned long hbNextTrigger = 0;
float hbBreathPhase = 0;
bool hbLeftActive = true;
float hbTravelPos = 0;
bool hbTravelPause = false;
unsigned long hbTravelPauseStart = 0;

void updateHeartbeat() {
  unsigned long now = millis();

  // V0: Mac breathing - smooth brightness pulse across all LEDs
  if (currentVariation == 0) {
    hbBreathPhase += 0.02f;
    float brightness = (sin(hbBreathPhase) + 1.0f) / 2.0f;
    uint8_t b = (uint8_t)(20 + brightness * 235);
    uint32_t col = strip.Color(b, 0, 0);
    for (int i = 0; i < numLeds; i++) {
      strip.setPixelColor(i, col);
    }
    strip.show();
    return;
  }

  // V3: Travelling beat - single pulse moving left to right
  if (currentVariation == 3) {
    if (!hbTravelPause) {
      hbTravelPos += 0.7f;
      if (hbTravelPos >= numLeds - 1) {
        hbTravelPos = numLeds - 1;
        hbTravelPause = true;
        hbTravelPauseStart = now;
      }
    } else {
      if (now - hbTravelPauseStart >= 800) {
        hbTravelPause = false;
        hbTravelPos = 0;
      }
    }

    strip.clear();
    int head = (int)hbTravelPos;
    uint32_t col = strip.Color(255, 0, 0);
    strip.setPixelColor(head, col);
    for (int t = 1; t <= 3; t++) {
      int idx = head - t;
      if (idx < 0) continue;
      float bright = 1.0f - t * 0.3f;
      uint8_t r = (uint8_t)(255 * bright);
      strip.setPixelColor(idx, strip.Color(r, 0, 0));
    }
    strip.show();
    return;
  }

  // V1, V2, V4: Realistic heartbeat rhythm (lub-dub-pause)
  if (now >= hbNextTrigger) {
    switch (hbPhase) {
      case PAUSE:
        hbPhase = FIRST_BEAT;
        hbTimer = now;
        hbPulse1Brightness = 1.0;
        hbPulse2Brightness = 0.0;
        if (currentVariation == 2) hbLeftActive = !hbLeftActive;
        break;

      case FIRST_BEAT:
        hbPhase = SECOND_BEAT;
        hbTimer = now;
        hbPulse1Brightness = 1.0;
        hbPulse2Brightness = 0.8;
        break;

      case SECOND_BEAT:
        hbPhase = PAUSE;
        hbTimer = now;
        hbNextTrigger = now + 700;
        break;
    }
  }

  float elapsed = (now - hbTimer) / 1000.0f;
  float pulseDuration = 0.35f;

  if (hbPhase == FIRST_BEAT || hbPhase == SECOND_BEAT) {
    float progress = constrain(elapsed / pulseDuration, 0.0f, 1.0f);
    float sigmaGrowth = 0;
    switch (currentVariation) {
      case 1: sigmaGrowth = 6.0; break;
      case 2: sigmaGrowth = 5.0; break;
      case 4: sigmaGrowth = 16.0; break;
    }
    hbSigma = 2.0 + progress * sigmaGrowth;
    float fade = (1.0f - progress) * 0.9f + 0.1f;
    if (hbPhase == FIRST_BEAT) {
      hbPulse1Brightness = fade;
    } else {
      hbPulse1Brightness = fade;
      hbPulse2Brightness = fade * 0.8f;
    }

    if (elapsed > pulseDuration) {
      if (hbPhase == FIRST_BEAT) {
        hbPhase = SECOND_BEAT;
        hbTimer = now;
        hbPulse1Brightness = 1.0;
        hbPulse2Brightness = 0.8;
        hbSigma = 2.0;
      } else {
        hbPhase = PAUSE;
        hbTimer = now;
        hbNextTrigger = now + 700;
      }
    }
  }

  float center1 = 0, center2 = 0;
  bool dual = false;
  switch (currentVariation) {
    case 1:
      center1 = numLeds / 2.0f;
      break;
    case 2:
      if (hbLeftActive) center1 = numLeds / 4.0f;
      else              center1 = 3.0f * numLeds / 4.0f;
      break;
    case 4:
      center1 = numLeds / 2.0f;
      dual = true;
      center2 = center1;
      break;
  }

  strip.clear();
  auto drawPulse = [&](float pos, float sigma, float brightness, uint32_t baseCol) {
    for (int i = 0; i < numLeds; i++) {
      float dist = i - pos;
      float intensity = brightness * exp(- (dist * dist) / (2.0f * sigma * sigma));
      if (intensity < 0.01f) continue;
      uint8_t r = (uint8_t)((baseCol >> 16 & 0xFF) * intensity);
      uint8_t g = (uint8_t)((baseCol >> 8 & 0xFF) * intensity);
      uint8_t b = (uint8_t)((baseCol & 0xFF) * intensity);
      strip.setPixelColor(i, strip.Color(r, g, b));
    }
  };

  uint32_t mainColor = strip.Color(255, 20, 20);
  uint32_t echoColor = strip.Color(255, 60, 60);

  drawPulse(center1, hbSigma, hbPulse1Brightness, mainColor);

  if ((currentVariation == 4 && dual) || (hbPhase == SECOND_BEAT && hbPulse2Brightness > 0.01f)) {
    float echoSigma = hbSigma * 0.85f;
    drawPulse(center2, echoSigma, hbPulse2Brightness, echoColor);
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
  hbPhase = PAUSE;
  hbNextTrigger = millis();
  hbPulse1Brightness = 0;
  hbPulse2Brightness = 0;
  hbBreathPhase = 0;
  hbTravelPos = 0;
  hbTravelPause = false;

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
  hbPhase=PAUSE; hbNextTrigger=millis(); hbPulse1Brightness=0; hbPulse2Brightness=0;
  hbBreathPhase = 0; hbLeftActive = true; hbTravelPos = 0; hbTravelPause = false;

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
