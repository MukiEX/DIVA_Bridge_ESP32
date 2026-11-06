#include "NetBridge.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include <atomic>

#include "DivaState.h"
#include "NetProtocol.h"
#include "Settings.h"

using namespace diva;

namespace NetBridge {

namespace {

constexpr int MAX_CLIENTS = 4;
std::atomic<int> clientCount{0};
bool enabled = false;
bool mdnsUp = false;

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

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[Net] Wi-Fi connected: %s  (set DivallerBridge.ini host=%s or %s.local)\n",
                    WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str(),
                    hostName().c_str());
      if (!mdnsUp && MDNS.begin(hostName().c_str())) {
        MDNS.addService("divabridge", "tcp", Settings::get().port);
        mdnsUp = true;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // The Arduino core reconnects by itself (setAutoReconnect).
      break;
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
  WiFi.setSleep(s.radio == Settings::Radio::Both);
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
  Serial.printf("[Net] connecting to '%s'...\n", s.ssid.c_str());
  xTaskCreatePinnedToCore(serverTask, "net_server", 4096, nullptr, 3, nullptr, 1);
}

String describe() {
  if (!enabled) {
    if (Settings::get().radio == Settings::Radio::BleOnly) return "off (radio=ble)";
    return "not set up";
  }
  if (WiFi.status() == WL_CONNECTED)
    return "connected " + WiFi.localIP().toString() + " (" + hostName() + ".local:" +
           String(Settings::get().port) + ", RSSI " + String(WiFi.RSSI()) + ")";
  return "connecting to '" + Settings::get().ssid + "'";
}

int clients() { return clientCount; }

}  // namespace NetBridge
