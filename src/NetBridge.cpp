#include "NetBridge.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include "BleBridge.h"
#include <lwip/sockets.h>

#include <atomic>

#include "DivaState.h"
#include "NetProtocol.h"
#include "WebUI.h"
#include "Settings.h"

using namespace diva;

namespace NetBridge {

namespace {

constexpr int MAX_CLIENTS = 4;
std::atomic<int> clientCount{0};
bool enabled = false;
bool mdnsUp = false;

}  // namespace

String hostName() {
  // "DIVA Bridge" -> "diva-bridge" (mDNS: diva-bridge.local)
  String h;
  for (char c : Settings::get().name) {
    if (isalnum((unsigned char)c)) h += (char)tolower(c);
    else if (h.length() && h[h.length() - 1] != '-') h += '-';
  }
  while (h.endsWith("-")) h.remove(h.length() - 1);
  return h.length() ? h : String("diva-bridge");
}

namespace {

void clientTask(void *arg) {
  int s = (int)(intptr_t)arg;
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct sockaddr_in peer;
  socklen_t plen = sizeof(peer);
  char who[32] = "?";
  if (getpeername(s, (struct sockaddr *)&peer, &plen) == 0)
    inet_ntoa_r(peer.sin_addr, who, sizeof(who));
  Serial.printf("[Net] game connected from %s\n", who);
  NetProtocol::serve(s, who);
  close(s);
  clientCount--;
  Serial.printf("[Net] game disconnected (%s)\n", who);
  vTaskDelete(nullptr);
}

void serverTask(void *) {
  int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(Settings::get().port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
    Serial.printf("[Net] can't listen on port %u\n", Settings::get().port);
    vTaskDelete(nullptr);
  }
  Serial.printf("[Net] DIVA bridge listening on port %u\n", Settings::get().port);
  while (true) {
    int s = accept(ls, nullptr, nullptr);
    if (s < 0) {
      delay(100);
      continue;
    }
    if (clientCount >= MAX_CLIENTS) {
      Serial.println("[Net] too many clients, refusing one");
      close(s);
      continue;
    }
    clientCount++;
    if (xTaskCreatePinnedToCore(clientTask, "net_client", 4096, (void *)(intptr_t)s, 4,
                                nullptr, 1) != pdPASS) {
      close(s);
      clientCount--;
    }
  }
}

// radio=auto: Wi-Fi off while a Bluetooth game is in native mode.
volatile bool wifiPaused = false;
constexpr uint32_t RESUME_AFTER_MS = 4000;   // the game must be gone this long (re-handshakes blink)

void pauseWifi() {
  wifiPaused = true;
  Serial.println("[Net] Bluetooth game playing: Wi-Fi paused (radio=auto)");
  if (mdnsUp) {
    MDNS.end();
    mdnsUp = false;
  }
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void resumeWifi() {
  auto &s = Settings::get();
  Serial.println("[Net] Bluetooth game gone: Wi-Fi back on");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(true);
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
  wifiPaused = false;
}

void autoRadioTask(void *) {
  uint32_t lastGame = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (BleBridge::nativeBleClients() > 0) {
      lastGame = millis();
      if (!wifiPaused) pauseWifi();
    } else if (wifiPaused && millis() - lastGame > RESUME_AFTER_MS) {
      resumeWifi();
    }
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (wifiPaused) return;   // our own switch-off, not news
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[Net] Wi-Fi connected: %s  (set DivallerBridge.ini host=%s or %s.local; "
                    "tester at http://%s/)\n",
                    WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(),
                    hostName().c_str(), WiFi.localIP().toString().c_str());
      if (!mdnsUp && MDNS.begin(hostName().c_str())) {
        MDNS.addService("divabridge", "tcp", Settings::get().port);
        MDNS.addService("http", "tcp", 80);
        mdnsUp = true;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      // The Arduino core reconnects by itself (setAutoReconnect); say why once
      // per reason so the log (and the web flasher) can tell what's wrong.
      static uint8_t lastReason = 0;
      uint8_t reason = info.wifi_sta_disconnected.reason;
      if (reason != lastReason) {
        lastReason = reason;
        const char *why = "disconnected";
        switch (reason) {
          case WIFI_REASON_NO_AP_FOUND: why = "network not found"; break;
          case WIFI_REASON_AUTH_FAIL:
          case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
          case WIFI_REASON_HANDSHAKE_TIMEOUT:
          case WIFI_REASON_MIC_FAILURE: why = "wrong password"; break;
          case WIFI_REASON_ASSOC_FAIL:
          case WIFI_REASON_AUTH_EXPIRE: why = "the router refused it"; break;
          default: break;
        }
        Serial.printf("[Net] Wi-Fi: can't join '%s': %s (reason %u) - retrying\n",
                      Settings::get().ssid.c_str(), why, reason);
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace

void begin() {
  auto &s = Settings::get();
  if (s.radio == Settings::Radio::BleOnly) {
    Serial.println("[Net] Wi-Fi off (radio=ble)");
    return;
  }
  if (!s.ssid.length()) {
    Serial.println("[Net] no Wi-Fi network set - use: set ssid <name>, set pass <password>, save");
    return;
  }
  enabled = true;
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName().c_str());
  WiFi.setAutoReconnect(true);
  // Only without Bluetooth can Wi-Fi stay fully awake (the radios share
  // time otherwise, and modem sleep is mandatory with BLE on).
  WiFi.setSleep(s.radio != Settings::Radio::WifiOnly);
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
  Serial.printf("[Net] connecting to '%s'...\n", s.ssid.c_str());
  xTaskCreatePinnedToCore(serverTask, "net_server", 4096, nullptr, 3, nullptr, 1);
  if (s.radio == Settings::Radio::Auto)
    xTaskCreatePinnedToCore(autoRadioTask, "net_auto", 3072, nullptr, 1, nullptr, 0);
  WebUI::begin();
}

String describe() {
  if (!enabled) {
    if (Settings::get().radio == Settings::Radio::BleOnly) return "off (radio=ble)";
    return "not set up";
  }
  if (wifiPaused) return "paused while a Bluetooth game plays (radio=auto)";
  if (WiFi.status() == WL_CONNECTED)
    return "connected " + WiFi.localIP().toString() + " (" + hostName() + ".local:" +
           String(Settings::get().port) + ", RSSI " + String(WiFi.RSSI()) + ")";
  return "connecting to '" + Settings::get().ssid + "'";
}

int clients() { return clientCount; }

bool paused() { return wifiPaused; }

}  // namespace NetBridge
