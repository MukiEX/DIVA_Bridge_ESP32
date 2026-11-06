#include "BleBridge.h"

#include <BleCompositeHID.h>
#include <NimBLEDevice.h>
#include <XboxGamepadDevice.h>
#include <string.h>

#include <atomic>

#include "DivaState.h"
#include "Settings.h"

using namespace diva;

namespace BleBridge {

namespace {

constexpr int MAX_NATIVE = 4;
constexpr uint32_t MIN_SEND_MS = 4;        // don't flood the BLE queue
constexpr uint32_t MOTION_HOLD_MS = 60;    // stick held after a swipe step
constexpr int16_t STICK_MAX = 32767;

// Set last in onStarted(), once every characteristic exists.
std::atomic<NimBLEServer *> server{nullptr};
// The library's own advertising start has happened (ours may restart it).
bool advStarted = false;
NimBLECharacteristic *chControl = nullptr;
NimBLECharacteristic *chInput = nullptr;
NimBLECharacteristic *chLeds = nullptr;
XboxGamepadDevice *pad = nullptr;
TaskHandle_t outTask = nullptr;

// Connections that did the handshake.
portMUX_TYPE nmux = portMUX_INITIALIZER_UNLOCKED;
uint16_t nativeConns[MAX_NATIVE];
int nativeCount = 0;

inline NimBLEServer *s_server() { return server.load(std::memory_order_acquire); }

bool isNative(uint16_t h) {
  for (int i = 0; i < nativeCount; i++)
    if (nativeConns[i] == h) return true;
  return false;
}

void addNative(uint16_t h) {
  bool added = false;
  portENTER_CRITICAL(&nmux);
  if (!isNative(h) && nativeCount < MAX_NATIVE) {
    nativeConns[nativeCount++] = h;
    added = true;
  }
  portEXIT_CRITICAL(&nmux);
  if (added) DivaState::nativeAcquire("BLE");
}

void removeNative(uint16_t h) {
  bool removed = false;
  portENTER_CRITICAL(&nmux);
  for (int i = 0; i < nativeCount; i++)
    if (nativeConns[i] == h) {
      nativeConns[i] = nativeConns[--nativeCount];
      removed = true;
      break;
    }
  portEXIT_CRITICAL(&nmux);
  if (removed) DivaState::nativeRelease("BLE");
}

void statusBytes(uint8_t s[8]) {
  uint16_t vid, pid;
  DivaState::controllerIds(vid, pid);
  const Profile *p = DivaState::controller();
  s[0] = PROTOCOL_VERSION;
  s[1] = p != nullptr;
  s[2] = p ? (uint8_t)p->kind : 0;
  s[3] = (uint8_t)DivaState::nativeClients();
  s[4] = vid & 0xFF;
  s[5] = vid >> 8;
  s[6] = pid & 0xFF;
  s[7] = pid >> 8;
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    NimBLEAttValue v = c->getValue();
    uint16_t h = info.getConnHandle();
    if (v.size() >= 4 && memcmp(v.data(), "DIVA", 4) == 0) {
      Serial.printf("[BLE] handshake from %s (protocol %d, MTU %d)\n",
                    info.getAddress().toString().c_str(),
                    v.size() > 4 ? v.data()[4] : 0, info.getMTU());
      addNative(h);
      // 7.5 ms connection interval for the lowest latency.
      s_server()->updateConnParams(h, 6, 6, 0, 200);
    } else if (v.size() >= 3 && memcmp(v.data(), "BYE", 3) == 0) {
      removeNative(h);
    }
    uint8_t s[8];
    statusBytes(s);
    c->setValue(s, sizeof(s));
  }
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    uint8_t s[8];
    statusBytes(s);
    c->setValue(s, sizeof(s));
  }
};

class LedCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    DivaState::queueLeds(v.data(), v.size());
  }
};

class InputCallbacks : public NimBLECharacteristicCallbacks {
  // A relay that crashes without "BYE" while the PC keeps the link up for its
  // gamepad driver: NimBLE reports the unsubscribe (also on disconnect).
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t sub) override {
    if (sub == 0) removeNative(info.getConnHandle());
  }
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    uint8_t buf[EXT_PACKET_LEN];
    if (!DivaState::readExt(buf)) idlePacket(buf), buf[24] = buf[25] = 0;
    c->setValue(buf, sizeof(buf));
  }
};

/** The composite HID device, with the DIVA Bridge service added. */
class DivaHid : public BleCompositeHID {
 public:
  using BleCompositeHID::BleCompositeHID;

 protected:
  void onStarted(NimBLEServer *s) override {
    if (Settings::consumeUnpair()) {
      NimBLEDevice::deleteAllBonds();
      Serial.println("[BLE] all pairings forgotten");
    }
    NimBLEDevice::setMTU(247);
    NimBLEService *svc = s->createService(DIVA_BLE_SERVICE_UUID);
    chControl = svc->createCharacteristic(
        DIVA_BLE_CONTROL_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY, 16);
    chInput = svc->createCharacteristic(
        DIVA_BLE_INPUT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        EXT_PACKET_LEN);
    chLeds = svc->createCharacteristic(
        DIVA_BLE_LEDS_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
        MAX_DEVICE_OUT);
    chControl->setCallbacks(new ControlCallbacks());
    chInput->setCallbacks(new InputCallbacks());
    chLeds->setCallbacks(new LedCallbacks());
    uint8_t st[8];
    statusBytes(st);
    chControl->setValue(st, sizeof(st));
    server.store(s, std::memory_order_release);
    Serial.println("[BLE] DIVA Bridge service ready");
  }
};

DivaHid *hid = nullptr;

// ------------------------------------------------------------ gamepad map
struct HalfTracker {
  int last = -1;          // previous centroid (x16), -1 = untouched
  int16_t out = 0;
  uint32_t until = 0;     // motion: hold the deflection until then
};
HalfTracker halves[2];

/** Centroid of touched cells in [lo, lo+16), scaled x16; -1 if none. */
int centroid(uint32_t sensors, int lo) {
  int sum = 0, n = 0;
  for (int i = 0; i < 16; i++)
    if (sensors >> (lo + i) & 1) {
      sum += i;
      n++;
    }
  return n ? sum * 16 / n : -1;
}

int16_t sliderAxis(HalfTracker &t, uint32_t sensors, int lo, uint32_t now) {
  int c = centroid(sensors, lo);
  switch (Settings::get().slider) {
    case Settings::SliderMode::Off:
      return 0;
    case Settings::SliderMode::Position:
      // Leftmost cell -> full left, rightmost -> full right.
      return c < 0 ? 0 : (int16_t)((long)(c - 120) * STICK_MAX / 120);
    case Settings::SliderMode::Motion:
    default:
      if (c >= 0 && t.last >= 0 && c != t.last) {
        t.out = c > t.last ? STICK_MAX : -STICK_MAX;
        t.until = now + MOTION_HOLD_MS;
      }
      t.last = c;
      if ((int32_t)(now - t.until) >= 0) t.out = 0;
      return t.out;
  }
}

/** Divaller state -> Xbox pad. Same face-button positions as the HORIs. */
void updatePad(const uint8_t *pkt, uint16_t ext, uint32_t now) {
  Decoded d = decode(pkt);
  pad->resetInputs();
  auto btn = [&](int b, uint16_t x) {
    if (d.pressed >> b & 1) pad->press(x);
  };
  btn(BTN_TRIANGLE, XBOX_BUTTON_Y);
  btn(BTN_SQUARE, XBOX_BUTTON_X);
  btn(BTN_CROSS, XBOX_BUTTON_A);
  btn(BTN_CIRCLE, XBOX_BUTTON_B);
  btn(BTN_FN, XBOX_BUTTON_START);
  btn(BTN_L1, XBOX_BUTTON_SELECT);
  btn(BTN_L3, XBOX_BUTTON_HOME);
  if (d.pressed >> BTN_L2 & 1) pad->pressShare();
  if (ext & EXT_LB) pad->press(XBOX_BUTTON_LB);
  if (ext & EXT_RB) pad->press(XBOX_BUTTON_RB);
  if (ext & EXT_LS) pad->press(XBOX_BUTTON_LS);
  if (ext & EXT_RS) pad->press(XBOX_BUTTON_RS);
  pad->setTriggers(ext & EXT_LT ? XBOX_TRIGGER_MAX : 0,
                   ext & EXT_RT ? XBOX_TRIGGER_MAX : 0);
  uint8_t dir = XboxDpadFlags::NONE;
  if (ext & EXT_UP) dir |= XboxDpadFlags::NORTH;
  if (ext & EXT_DOWN) dir |= XboxDpadFlags::SOUTH;
  if (ext & EXT_LEFT) dir |= XboxDpadFlags::WEST;
  if (ext & EXT_RIGHT) dir |= XboxDpadFlags::EAST;
  pad->pressDPadDirectionFlag((XboxDpadFlags)dir);
  // Slider: left half -> left stick X, right half -> right stick X.
  pad->setLeftThumb(sliderAxis(halves[0], d.sensors, 0, now), 0);
  pad->setRightThumb(sliderAxis(halves[1], d.sensors, 16, now), 0);
  pad->sendGamepadReport();
}

void neutralPad() {
  pad->resetInputs();
  pad->sendGamepadReport();
  halves[0] = halves[1] = HalfTracker();
}

/** Drop handshakes from connections that are gone; keep advertising so a
 * second central (e.g. the bridge relay next to the OS HID driver, or a
 * second machine) can still connect. */
void housekeeping() {
  NimBLEServer *srv = s_server();
  if (!srv) return;
  std::vector<uint16_t> peers = srv->getPeerDevices();
  uint16_t stale[MAX_NATIVE];
  int n = 0;
  portENTER_CRITICAL(&nmux);
  for (int i = 0; i < nativeCount; i++) {
    bool alive = false;
    for (uint16_t p : peers) alive |= p == nativeConns[i];
    if (!alive) stale[n++] = nativeConns[i];
  }
  portEXIT_CRITICAL(&nmux);
  for (int i = 0; i < n; i++) removeNative(stale[i]);

  static size_t lastPeers = 0;
  if (peers.size() > lastPeers) {
    // New central: ask for the shortest interval (the host decides).
    for (uint16_t p : peers) srv->updateConnParams(p, 6, 6, 0, 200);
  }
  lastPeers = peers.size();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (!adv) return;
  // Only restart advertising after the library's own start (it sets the
  // gamepad appearance and HID UUID, and starts the GATT server).
  if (!advStarted) {
    advStarted = adv->isAdvertising() || !peers.empty();
    return;
  }
  if (!adv->isAdvertising() && peers.size() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) adv->start();
}

void outputLoop(void *) {
  uint32_t lastSeq = ~0u, lastSend = 0, lastHouse = 0;
  uint8_t lastStatus[8] = {};
  bool wasNative = false, padDirty = true, nativeDirty = true, padWasUp = false;
  uint8_t pkt[EXT_PACKET_LEN];
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MIN_SEND_MS));
    uint32_t now = millis();
    if (now - lastHouse >= 250) {
      lastHouse = now;
      housekeeping();
    }
    NimBLEServer *srv = s_server();
    if (!srv) continue;

    // Status changes (controller plugged/unplugged, native count) -> notify.
    uint8_t st[8];
    statusBytes(st);
    if (memcmp(st, lastStatus, 8)) {
      memcpy(lastStatus, st, 8);
      chControl->setValue(st, sizeof(st));
      chControl->notify();
    }

    uint32_t seq = DivaState::inputSeq();
    if (seq != lastSeq) {
      padDirty = nativeDirty = true;
    }
    bool native = DivaState::nativeClients() > 0;
    if (now - lastSend < MIN_SEND_MS) continue;

    if (!DivaState::readExt(pkt)) {
      idlePacket(pkt);
      pkt[24] = pkt[25] = 0;
    }
    if (nativeDirty && nativeCount > 0) {
      chInput->setValue(pkt, EXT_PACKET_LEN);
      chInput->notify();
      nativeDirty = false;
      lastSend = now;
    }

    // Per-connection check: the library's flag only tracks authentication.
    bool padUp = srv->getConnectedCount() > 0 && hid->isConnected();
    if (padUp && !padWasUp) padDirty = true;  // (re)connected: send current state
    padWasUp = padUp;
    if (padUp) {
      if (native) {
        if (!wasNative) neutralPad();  // hand over to the native client
        padDirty = false;
      } else {
        bool motionActive = halves[0].out || halves[1].out;
        if (padDirty || motionActive || wasNative) {
          updatePad(pkt, pkt[24] | pkt[25] << 8, now);
          lastSend = now;
          padDirty = false;
        }
      }
    }
    wasNative = native;
    lastSeq = seq;
  }
}

}  // namespace

void begin() {
  auto &s = Settings::get();
  XboxGamepadDeviceConfiguration *cfg;
  if (s.xboxOneS)
    cfg = new XboxOneSControllerDeviceConfiguration();
  else
    cfg = new XboxSeriesXControllerDeviceConfiguration();
  cfg->setAutoReport(false);
  BLEHostConfiguration host = cfg->getIdealHostConfiguration();
  pad = new XboxGamepadDevice(cfg);
  hid = new DivaHid(s.name.c_str(), "DIVA Bridge", 100);
  hid->addDevice(pad);
  hid->begin(host);
  Serial.printf("[BLE] advertising as '%s' (Xbox %s profile)\n", s.name.c_str(),
                s.xboxOneS ? "One S" : "Series X");
  xTaskCreatePinnedToCore(outputLoop, "ble_out", 6144, nullptr, 3, &outTask, 1);
  DivaState::setInputListener(outTask);
}

int connections() {
  NimBLEServer *srv = s_server();
  return srv ? srv->getConnectedCount() : 0;
}

}  // namespace BleBridge
