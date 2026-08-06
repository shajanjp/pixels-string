// ====================================================================
//  rest_api.h - REST endpoints for the web dashboard
//
//  handleClient() reads one HTTP request and routes it: the REST API
//  (/api/*, /config, /help, /), the dashboard at /, and the MCP
//  endpoint at /mcp (delegated to handleMCP() in mcp.h).
// ====================================================================
#pragma once

#include "globals.h"
#include "dashboard_html.h"

void handleClient() {
  WiFiClient client = server.accept();
  if (!client) return;

  String req = "";
  unsigned long lineTimeout = millis() + 2000;
  while (client.connected() && millis() < lineTimeout) {
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

  // Read the remaining headers so a POST body (e.g. MCP JSON-RPC) can be read.
  String headers = "";
  unsigned long headerTimeout = millis() + 2000;
  while (client.connected() && millis() < headerTimeout &&
         headers.indexOf("\r\n\r\n") < 0 && headers.indexOf("\n\n") < 0) {
    if (client.available()) headers += (char)client.read();
  }

  int contentLength = 0;
  String lowerHeaders = headers;
  lowerHeaders.toLowerCase();
  int clPos = lowerHeaders.indexOf("content-length:");
  if (clPos >= 0) {
    int valStart = clPos + 15;   // strlen("content-length:")
    while (valStart < (int)lowerHeaders.length() && lowerHeaders.charAt(valStart) == ' ') valStart++;
    for (int i = valStart; i < (int)lowerHeaders.length() && isdigit(lowerHeaders.charAt(i)); i++) {
      contentLength = contentLength * 10 + (lowerHeaders.charAt(i) - '0');
    }
  }

  String body = "";
  if (contentLength > 0) {
    unsigned long bodyTimeout = millis() + 3000;
    while ((int)body.length() < contentLength && client.connected() && millis() < bodyTimeout) {
      if (client.available()) body += (char)client.read();
    }
  }

  while (client.available()) client.read();

  Serial.printf("[HTTP] %s %s\n", method.c_str(), path.c_str());

  if (path == "/mcp" && method == "POST") {
    handleMCP(client, body);
    return;
  }
  if (path == "/mcp" && method == "OPTIONS") {
    // CORS preflight for browser-based MCP clients (e.g. MCP Inspector).
    client.print("HTTP/1.1 204 No Content\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id, Authorization\r\n"
                 "Connection: close\r\n\r\n");
    client.stop();
    return;
  }
  if (path == "/mcp") {
    // No server-initiated messages here (no SSE): clients that open a GET
    // stream fall back to POST-only mode when they get a 405.
    client.print("HTTP/1.1 405 Method Not Allowed\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n");
    client.stop();
    return;
  }

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

    // Validate hex colors (shared helper, also used by the MCP set_color tool)
    for (int i = 0; i < numColors; i++) {
      if (!isValidHexColor(colors[i])) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid hex color: " + colors[i] + "\"}";
        client.print(response);
        client.stop();
        return;
      }
    }

    // Percentage limits the pattern to the first N% of the strip; the
    // remaining LEDs are turned off. Omit for 100%.
    String percentageStr = getParam(fullPath, "percentage");
    int percentage = -1; // -1 = not specified
    if (percentageStr.length() > 0) {
      bool isNumeric = true;
      for (int i = 0; i < percentageStr.length(); i++)
        if (!isdigit(percentageStr.charAt(i))) { isNumeric = false; break; }
      if (!isNumeric || percentageStr.length() > 3) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Invalid percentage value\"}";
        client.print(response);
        client.stop();
        return;
      }
      int v = percentageStr.toInt();
      if (v < 0 || v > 100) {
        response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                   "Connection: close\r\n\r\n"
                   "{\"status\":\"error\",\"message\":\"Percentage must be between 0 and 100\"}";
        client.print(response);
        client.stop();
        return;
      }
      percentage = v;
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

    // Limit the pattern to the given percentage of the strip (first LEDs).
    if (percentage >= 0) {
      applyPercentageToBuffer(percentage);
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
    helpResponse += "GET /api/pixels/set?pattern=<solid|striped|gradient>&color=RRGGBB[&color=...][&brightness=0-255][&percentage=0-100]\n";
    helpResponse += "  Set the entire pixel strip to a static pattern.\n";
    helpResponse += "  pattern: solid (one color), striped (colors alternate), gradient (interpolated).\n";
    helpResponse += "  color: hex color(s), 3 or 6 digits, case insensitive. Repeat for multiple.\n";
    helpResponse += "  brightness: optional, 0-255. When given, sets the global brightness\n";
    helpResponse += "              (persisted, applies to all effects). Omit to keep the\n";
    helpResponse += "              current global brightness.\n";
    helpResponse += "  percentage: optional, 0-100. Applies the pattern to only the first\n";
    helpResponse += "              N% of LEDs; the remaining LEDs are turned off.\n";
    helpResponse += "              Omit or use 100 for the whole strip.\n\n";
    helpResponse += "POST /mcp\n";
    helpResponse += "  Model Context Protocol (MCP) endpoint: JSON-RPC 2.0 over HTTP.\n";
    helpResponse += "  Lets AI assistants control the LEDs. Exposes two tools:\n";
    helpResponse += "    - apply_animation(name): switch to a built-in animation\n";
    helpResponse += "    - set_color(color, brightness): solid color + brightness (0-255)\n\n";
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

