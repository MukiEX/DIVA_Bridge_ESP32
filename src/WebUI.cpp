#include "WebUI.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "BleBridge.h"
#include "ControllerHost.h"
#include "DivaState.h"
#include "NetBridge.h"
#include "Settings.h"
#include "WebPage.h"

using namespace diva;

namespace WebUI {

namespace {

WebServer *server = nullptr;

const char *radioName(Settings::Radio r) {
  return r == Settings::Radio::WifiOnly ? "wifi" : r == Settings::Radio::BleOnly ? "ble" : "both";
}

/** JSON string escape (names and SSIDs can contain anything). */
String jsonStr(const String &s) {
  String o = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += c;
    }
  }
  return o + "\"";
}

void sendJson(int code, const String &body) {
  server->sendHeader("Cache-Control", "no-store");
  server->send(code, "application/json", body);
}

void handleRoot() {
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "no-cache");
  server->send_P(200, "text/html; charset=utf-8", (const char *)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

void handleState() {
  uint8_t pkt[EXT_PACKET_LEN];
  const Profile *p = DivaState::controller();
  bool present = DivaState::readExt(pkt);
  if (!present) {
    idlePacket(pkt);
    pkt[24] = pkt[25] = 0;
  }
  Decoded d = decode(pkt);
  uint16_t vid, pid;
  DivaState::controllerIds(vid, pid);
  char hex[2 * PACKET_LEN + 1];
  for (size_t i = 0; i < PACKET_LEN; i++) snprintf(hex + 2 * i, 3, "%02x", pkt[i]);
  auto &s = Settings::get();
  char head[512];
  snprintf(head, sizeof(head),
           "{\"present\":%s,\"kind\":%d,\"vid\":\"%04X\",\"pid\":\"%04X\",\"pressed\":%u,"
           "\"sensors\":%lu,\"ext\":%u,\"pkt\":\"%s\",\"reports\":%lu,\"native\":%d,"
           "\"tcp\":%d,\"ble\":%d,\"rssi\":%d,\"port\":%u,\"uptime\":%lu,\"heap\":%lu,"
           "\"radio\":\"%s\",\"otg_board\":%s,",
           present ? "true" : "false", p ? (int)p->kind : 0, vid, pid, d.pressed,
           (unsigned long)d.sensors, (unsigned)(pkt[24] | pkt[25] << 8), hex,
           (unsigned long)ControllerHost::reportCount(), DivaState::nativeClients(),
           NetBridge::clients(), BleBridge::connections(), (int)WiFi.RSSI(), s.port,
           (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), radioName(s.radio),
#ifdef BOARD_ESP32_S3_USB_OTG
           "true"
#else
           "false"
#endif
  );
  String body = head;
  body += "\"name\":" + jsonStr(p ? p->name : "") + ",";
  body += "\"ip\":" + jsonStr(WiFi.localIP().toString()) + ",";
  body += "\"host\":" + jsonStr(NetBridge::hostName()) + ",";
  body += "\"ssid\":" + jsonStr(s.ssid) + ",";
  body += "\"btname\":" + jsonStr(s.name) + ",";
  body += "\"fw\":" + jsonStr(__DATE__ " " __TIME__) + "}";
  sendJson(200, body);
}

void handleGetSettings() {
  String lines = Settings::toLines(false), body = "{";
  int start = 0;
  while (start < (int)lines.length()) {
    int end = lines.indexOf('\n', start);
    if (end < 0) end = lines.length();
    String l = lines.substring(start, end);
    start = end + 1;
    int eq = l.indexOf('=');
    if (eq <= 0) continue;
    body += jsonStr(l.substring(0, eq)) + ":" + jsonStr(l.substring(eq + 1)) + ",";
  }
  body += "\"pass_set\":" + String(Settings::passwordSet() ? "true" : "false") + "}";
  sendJson(200, body);
}

void handlePostSettings() {
  String error;
  if (!Settings::applyAndSave(server->arg("plain"), error)) {
    sendJson(400, "{\"ok\":false,\"error\":" + jsonStr(error) + "}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
  Serial.println("[Web] settings saved from the web page - restarting");
  Settings::restartSoon(800);
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void handleLeds() {
  const String &h = server->arg("plain");
  uint8_t buf[MAX_DEVICE_OUT];
  size_t n = 0;
  for (size_t i = 0; i + 1 < h.length() && n < sizeof(buf); i += 2) {
    int a = hexNibble(h[i]), b = hexNibble(h[i + 1]);
    if (a < 0 || b < 0) break;
    buf[n++] = a << 4 | b;
  }
  if (n < 4) {
    sendJson(400, "{\"ok\":false}");
    return;
  }
  bool ok = DivaState::queueLeds(buf, n);
  sendJson(200, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no controller\"}");
}

void serverTask(void *) {
  while (true) {
    server->handleClient();
    delay(2);
  }
}

}  // namespace

void begin() {
  if (server) return;
  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/api/state", HTTP_GET, handleState);
  server->on("/api/settings", HTTP_GET, handleGetSettings);
  server->on("/api/settings", HTTP_POST, handlePostSettings);
  server->on("/api/leds", HTTP_POST, handleLeds);
  server->onNotFound([] { server->send(404, "text/plain", "not found"); });
  server->begin();
  xTaskCreatePinnedToCore(serverTask, "web", 6144, nullptr, 1, nullptr, 1);
  Serial.println("[Web] tester and settings on http port 80");
}

}  // namespace WebUI
