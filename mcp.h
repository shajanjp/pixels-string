// ====================================================================
//  mcp.h - minimal MCP (Model Context Protocol) server over HTTP
//
//  JSON-RPC 2.0 endpoint served at POST /mcp, plus the tiny string-
//  scanning JSON helpers it uses. See handleMCP() for details.
// ====================================================================
#pragma once

#include "globals.h"

// ===========================  MCP (MODEL CONTEXT PROTOCOL) ===========================
// Minimal MCP server over Streamable HTTP (JSON-RPC 2.0), served at POST /mcp.
// Lets AI clients (Claude Desktop, MCP Inspector, ...) control the LEDs via
// two tools, both backed by the existing effect / pattern / brightness code:
//   - apply_animation(name): switch to a built-in animation
//   - set_color(color, brightness): light the strip in a solid color
// The endpoint is stateless: every POST is answered with a JSON-RPC response
// and the connection is closed, so no session or SSE stream is required.

// -----------------------  tiny JSON helpers -----------------------
// Request bodies are small (<1 KB), so string scanning is enough; no JSON
// library is needed. All helpers are strict about quotes and whitespace.

// Value of a string key, e.g. ("{\"a\":\"hi\"}", "a") -> "hi".
String jsonStringValue(const String& json, const char* key, int from = 0) {
  String k = String("\"") + key + "\"";
  int pos = json.indexOf(k, from);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + k.length());
  if (pos < 0) return "";
  pos++;
  while (pos < (int)json.length() &&
         (json.charAt(pos) == ' ' || json.charAt(pos) == '\t')) pos++;
  if (pos >= (int)json.length() || json.charAt(pos) != '"') return "";
  pos++;
  String out = "";
  while (pos < (int)json.length() && json.charAt(pos) != '"') {
    if (json.charAt(pos) == '\\') {
      pos++;
      if (pos >= (int)json.length()) break;
    }
    out += json.charAt(pos);
    pos++;
  }
  return out;
}

// Integer value of a key; returns def when absent or not a number.
// Tolerates both bare numbers (128) and string-encoded numbers ("128").
int jsonIntValue(const String& json, const char* key, int from = 0, int def = 0) {
  String k = String("\"") + key + "\"";
  int pos = json.indexOf(k, from);
  if (pos < 0) return def;
  pos = json.indexOf(':', pos + k.length());
  if (pos < 0) return def;
  pos++;
  while (pos < (int)json.length() &&
         (json.charAt(pos) == ' ' || json.charAt(pos) == '\t')) pos++;
  if (pos >= (int)json.length()) return def;
  if (json.charAt(pos) == '"') pos++;   // string-encoded number
  bool neg = false;
  if (pos < (int)json.length() && json.charAt(pos) == '-') { neg = true; pos++; }
  long v = 0;
  bool any = false;
  while (pos < (int)json.length() && json.charAt(pos) >= '0' && json.charAt(pos) <= '9') {
    v = v * 10 + (json.charAt(pos) - '0');
    any = true;
    pos++;
  }
  if (!any) return def;
  return (int)(neg ? -v : v);
}

// Raw JSON value for a key (number, quoted string, or nested object/array).
// Used to echo the JSON-RPC "id" back to the client exactly as sent.
String jsonRawValue(const String& json, const char* key, int from = 0) {
  String k = String("\"") + key + "\"";
  int pos = json.indexOf(k, from);
  if (pos < 0) return "";
  pos = json.indexOf(':', pos + k.length());
  if (pos < 0) return "";
  pos++;
  while (pos < (int)json.length() &&
         (json.charAt(pos) == ' ' || json.charAt(pos) == '\t')) pos++;
  if (pos >= (int)json.length()) return "";
  char c = json.charAt(pos);
  if (c == '"') {                          // quoted string
    int start = pos;
    pos++;
    while (pos < (int)json.length()) {
      char d = json.charAt(pos);
      if (d == '\\') { pos += 2; continue; }
      pos++;
      if (d == '"') break;
    }
    return json.substring(start, pos);
  }
  if (c == '{' || c == '[') {              // nested object / array
    int start = pos, depth = 0;
    while (pos < (int)json.length()) {
      char d = json.charAt(pos);
      if (d == '"') {                      // skip string contents
        pos++;
        while (pos < (int)json.length()) {
          if (json.charAt(pos) == '\\') pos += 2;
          else if (json.charAt(pos) == '"') { pos++; break; }
          else pos++;
        }
        continue;
      }
      if (d == '{' || d == '[') depth++;
      else if (d == '}' || d == ']') { depth--; if (depth == 0) { pos++; break; } }
      pos++;
    }
    return json.substring(start, pos);
  }
  int start = pos;                          // bare number / true / false / null
  while (pos < (int)json.length()) {
    char d = json.charAt(pos);
    if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' || d == '\r' || d == '\n') break;
    pos++;
  }
  return json.substring(start, pos);
}

// Escape a string for embedding inside a JSON string literal.
String jsonEscape(const String& s) {
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
    }
  }
  return out;
}

// Resolve a built-in animation by name (case-insensitive). STATIC_PIXEL is an
// internal mode used by set_color, not an animation, so it is excluded here.
bool resolveAnimation(const String& name, Effect& out) {
  for (int i = 0; i < NUM_EFFECTS; i++) {
    if (i == STATIC_PIXEL) continue;
    if (name.equalsIgnoreCase(effectName((Effect)i))) {
      out = (Effect)i;
      return true;
    }
  }
  return false;
}

// Handles one JSON-RPC request received at POST /mcp and sends the response.
void handleMCP(WiFiClient& client, const String& body) {
  String method = jsonStringValue(body, "method");
  String id = jsonRawValue(body, "id");

  // JSON-RPC notifications carry no id and must not be answered with a body.
  if (method == "notifications/initialized" || method == "notifications/cancelled" ||
      method == "notifications/progress" || method == "notifications/roots/list_changed") {
    client.print("HTTP/1.1 202 Accepted\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: close\r\n\r\n");
    client.stop();
    return;
  }

  String out = "{\"jsonrpc\":\"2.0\",\"id\":" + (id.length() ? id : "null");

  if (method == "initialize") {
    // Echo the client's protocol version so any MCP client version is accepted.
    String clientVersion = jsonStringValue(body, "protocolVersion");
    String version = clientVersion.length() > 0 ? clientVersion : "2025-03-26";
    out += ",\"result\":{\"protocolVersion\":\"" + version + "\",";
    out += "\"capabilities\":{\"tools\":{\"listChanged\":false}},";
    out += "\"serverInfo\":{\"name\":\"pixels-string\",\"version\":\"1.0.0\"}}";
  }
  else if (method == "ping") {
    out += ",\"result\":{}";
  }
  else if (method == "tools/list") {
    out += ",\"result\":{\"tools\":[";
    out += "{\"name\":\"apply_animation\",";
    out += "\"description\":\"Switch the LED strip to a built-in pixel animation by name. ";
    out += "Valid names: FIREFLIES, RAINBOW_SWIPE, AURORA, COMET, CHASING_DOTS, CYLON, ";
    out += "DUAL_COMET, SPARKLE_SWEEP, POLICE, PLASMA, RAINBOW_GRADIENT, PULSE_WAVE, ";
    out += "SINGLE_RUNNER, AUDIO_VISUALIZER, HEARTBEAT, TWINKLE, FIRE_FLICKER, ";
    out += "BOUNCING_BALLS, LIGHTNING_STORM, KALEIDOSCOPE, COLLIDING_FILL, PAINT_SPLAT, SNAKE.\",";
    out += "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",";
    out += "\"description\":\"Name of the animation to apply.\"}},\"required\":[\"name\"]}},";
    out += "{\"name\":\"set_color\",";
    out += "\"description\":\"Light the LED strip in a custom solid color at the given brightness.\",";
    out += "\"inputSchema\":{\"type\":\"object\",\"properties\":{";
    out += "\"color\":{\"type\":\"string\",\"description\":\"Hex color, 3 or 6 hex digits, case-insensitive (e.g. FF0000 or f00).\"},";
    out += "\"brightness\":{\"type\":\"integer\",\"description\":\"Brightness 0-255 (0 = off, 255 = max). Optional; keeps the current brightness when omitted.\"}},";
    out += "\"required\":[\"color\"]}}";
    out += "]}";
  }
  else if (method == "tools/call") {
    int paramsPos = body.indexOf("\"params\"");
    String toolName = jsonStringValue(body, "name", paramsPos);
    int argsPos = body.indexOf("\"arguments\"");

    String text;
    bool isError = false;

    if (toolName == "apply_animation") {
      String name = argsPos >= 0 ? jsonStringValue(body, "name", argsPos) : "";
      name.trim();
      Effect e;
      if (name.length() == 0) {
        text = "Missing 'name' argument.";
        isError = true;
      } else if (!resolveAnimation(name, e)) {
        text = "Unknown animation: " + name;
        isError = true;
      } else {
        currentEffect = e;
        currentVariation = 0;
        text = "Animation set to " + effectName(e);
      }
    }
    else if (toolName == "set_color") {
      String color = argsPos >= 0 ? jsonStringValue(body, "color", argsPos) : "";
      color.trim();
      if (!isValidHexColor(color)) {
        text = "Invalid color '" + color + "'. Expected 3 or 6 hex digits (e.g. FF0000).";
        isError = true;
      } else {
        int brightness = argsPos >= 0 ? jsonIntValue(body, "brightness", argsPos, -1) : -1;
        if (brightness >= 0) {
          setGlobalBrightness((uint8_t)constrain(brightness, 0, 255));
          prefs.putInt("brightness", globalBrightness);
        }
        generateSolidPattern(color);
        if (!staticPixelBuffer) {
          text = "Failed to allocate pixel buffer.";
          isError = true;
        } else {
          currentEffect = STATIC_PIXEL;
          currentVariation = 0;
          text = "Set solid color " + color + " at brightness " + String(globalBrightness);
        }
      }
    }
    else {
      text = "Unknown tool: " + toolName;
      isError = true;
    }

    out += ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + jsonEscape(text) + "\"}],";
    out += "\"isError\":" + String(isError ? "true" : "false") + "}";
  }
  else {
    out += ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}";
  }

  out += "}";

  client.print("HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Connection: close\r\n\r\n");
  client.print(out);
  client.stop();
}

