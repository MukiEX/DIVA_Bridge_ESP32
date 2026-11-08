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
#include "Version.h"
#include "boards/WatchDisplay.h"
#include <esp_partition.h>

using namespace diva;

namespace WebUI {

namespace {

WebServer *server = nullptr;

const char *radioName(Settings::Radio r) {
  return r == Settings::Radio::WifiOnly ? "wifi" : r == Settings::Radio::BleOnly ? "ble" : r == Settings::Radio::Auto ? "auto" : "both";
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
           "\"radio\":\"%s\",\"otg_board\":%s,\"screen\":%s,\"battery\":%d,\"charging\":%s,",
           present ? "true" : "false", p ? (int)p->kind : 0, vid, pid, d.pressed,
           (unsigned long)d.sensors, (unsigned)(pkt[24] | pkt[25] << 8), hex,
           (unsigned long)ControllerHost::reportCount(), DivaState::nativeClients(),
           NetBridge::clients(), BleBridge::connections(), (int)WiFi.RSSI(), s.port,
           (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), radioName(s.radio),
#ifdef BOARD_ESP32_S3_USB_OTG
           "true",
#else
           "false",
#endif
#ifdef BOARD_WS_AMOLED_206
           "true", WatchDisplay::batteryPercent(), WatchDisplay::charging() ? "true" : "false"
#else
           "false", -1, "false"
#endif
  );
  String body = head;
  body += "\"name\":" + jsonStr(p ? p->name : "") + ",";
  body += "\"ip\":" + jsonStr(WiFi.localIP().toString()) + ",";
  body += "\"host\":" + jsonStr(NetBridge::hostName()) + ",";
  body += "\"ssid\":" + jsonStr(s.ssid) + ",";
  body += "\"btname\":" + jsonStr(s.name) + ",";
  char extra[160];
  snprintf(extra, sizeof(extra),
           "\"presses\":%u,\"ble_interval\":%.2f,\"ble_sent\":%lu,\"ble_failed\":%lu,",
           DivaState::pressCount(), BleBridge::intervalMs(), (unsigned long)BleBridge::notifySent(),
           (unsigned long)BleBridge::notifyFailed());
  body += extra;
  body += "\"fw\":" + jsonStr(DIVA_BRIDGE_VERSION) + ",";
  body += "\"built\":" + jsonStr(__DATE__ " " __TIME__) + "}";
  sendJson(200, body);
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart (settings / command)";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT: return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dipped)";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
  }
}

void handleUsb() {
  sendJson(200, "{\"devices\":" + ControllerHost::usbTreeJson() + ",\"port\":" +
                    jsonStr(ControllerHost::portStatus()) + ",\"reset\":" + jsonStr(resetReason()) + "}");
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

// Background image upload (screen boards): the page posts the finished
// "DVBG" blob (header + 410x502 RGB565) as one file field; it is streamed
// straight into the "bgimage" partition.
const esp_partition_t *bgPart = nullptr;
size_t bgWritten = 0;
bool bgOk = false;

void handleBgUpload() {
  HTTPUpload &up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    bgPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bgimage");
    bgWritten = 0;
    bgOk = bgPart && esp_partition_erase_range(bgPart, 0, bgPart->size) == ESP_OK;
  } else if (up.status == UPLOAD_FILE_WRITE && bgOk) {
    if (bgWritten + up.currentSize > bgPart->size ||
        esp_partition_write(bgPart, bgWritten, up.buf, up.currentSize) != ESP_OK)
      bgOk = false;
    bgWritten += up.currentSize;
  }
}

void handleBgDone() {
  if (!bgOk || bgWritten < 12) {
    sendJson(400, "{\"ok\":false,\"error\":\"upload failed\"}");
    return;
  }
  Serial.printf("[Web] background image written (%u bytes)\n", (unsigned)bgWritten);
#ifdef BOARD_WS_AMOLED_206
  WatchDisplay::reloadBackground();
#endif
  sendJson(200, "{\"ok\":true}");
}

void handleBgClear() {
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bgimage");
  bool ok = p && esp_partition_erase_range(p, 0, 0x1000) == ESP_OK;
#ifdef BOARD_WS_AMOLED_206
  if (ok) WatchDisplay::reloadBackground();
#endif
  sendJson(ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"ok\":false}");
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
  server->on("/api/usb", HTTP_GET, handleUsb);
  server->on("/api/settings", HTTP_GET, handleGetSettings);
  server->on("/api/settings", HTTP_POST, handlePostSettings);
  server->on("/api/leds", HTTP_POST, handleLeds);
  server->on("/api/background", HTTP_POST, handleBgDone, handleBgUpload);
  server->on("/api/background", HTTP_DELETE, handleBgClear);
  server->onNotFound([] { server->send(404, "text/plain", "not found"); });
  server->begin();
  xTaskCreatePinnedToCore(serverTask, "web", 6144, nullptr, 1, nullptr, 1);
  Serial.println("[Web] tester and settings on http port 80");
}

}  // namespace WebUI
