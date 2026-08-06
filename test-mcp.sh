#!/usr/bin/env bash
# Quick test harness for the ESP32's MCP endpoint (POST /mcp).
# Usage: ./test-mcp.sh [device-url]   (default: http://192.168.1.55)
set -u

DEVICE="${1:-http://192.168.1.55}"
ENDPOINT="$DEVICE/mcp"
PASS=0
FAIL=0

say()  { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m  %s\n' "$*"; }

mcp_call() { # id, method, extra json
  curl -s --max-time 5 -X POST "$ENDPOINT" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json, text/event-stream' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":$1,\"method\":\"$2\"${3:-}}"
}

expect_json() { # label, response
  if echo "$2" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    ok "$1 (valid JSON)"
  else
    fail "$1 (invalid JSON: $2)"
  fi
}

say "Device reachability"
INFO=$(curl -s --max-time 5 "$DEVICE/api/info")
if [ -n "$INFO" ]; then
  ok "device responds: $INFO"
else
  fail "no response from $DEVICE - is it powered and flashed with the new firmware?"
  exit 1
fi

say "initialize"
R=$(mcp_call 1 "initialize" ',"params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test-mcp.sh","version":"1.0"}}')
expect_json "initialize" "$R"
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["result"]["protocolVersion"]=="2025-03-26"; assert j["result"]["serverInfo"]["name"]=="pixels-string"; print("    -> protocolVersion:", j["result"]["protocolVersion"])' && ok "initialize result" || fail "initialize result"

say "tools/list"
R=$(mcp_call 2 "tools/list")
expect_json "tools/list" "$R"
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); tools=[t["name"] for t in j["result"]["tools"]]; assert "apply_animation" in tools and "set_color" in tools, tools; print("    -> tools:", ", ".join(tools))' && ok "both tools advertised" || fail "both tools advertised"

say "tools/call apply_animation"
R=$(mcp_call 3 "tools/call" ',"params":{"name":"apply_animation","arguments":{"name":"AURORA"}}')
expect_json "apply_animation" "$R"
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); t=j["result"]["content"][0]["text"]; assert j["result"]["isError"] is False; print("    ->", t); assert "AURORA" in t' && ok "animation applied" || fail "animation applied"

say "tools/call set_color"
R=$(mcp_call 4 "tools/call" ',"params":{"name":"set_color","arguments":{"color":"00FF00","brightness":128}}')
expect_json "set_color" "$R"
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); t=j["result"]["content"][0]["text"]; assert j["result"]["isError"] is False; print("    ->", t)' && ok "color set" || fail "color set"

say "error handling (unknown animation)"
R=$(mcp_call 5 "tools/call" ',"params":{"name":"apply_animation","arguments":{"name":"BOGUS"}}')
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["result"]["isError"] is True; print("    ->", j["result"]["content"][0]["text"])' && ok "unknown animation rejected" || fail "unknown animation rejected"

say "error handling (invalid color)"
R=$(mcp_call 6 "tools/call" ',"params":{"name":"set_color","arguments":{"color":"not-a-color"}}')
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["result"]["isError"] is True; print("    ->", j["result"]["content"][0]["text"])' && ok "invalid color rejected" || fail "invalid color rejected"

say "ping"
R=$(mcp_call 7 "ping")
echo "$R" | python3 -c 'import json,sys; j=json.load(sys.stdin); assert j["result"]=={}' && ok "ping" || fail "ping"

printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
