#include "BleBridge.h"

#include <BleCompositeHID.h>
#include <NimBLEDevice.h>
#include <XboxGamepadDevice.h>
#include <stdarg.h>
#include <string.h>

#include <atomic>

#include "DivaState.h"
#include "Settings.h"

using namespace diva;

namespace BleBridge {

namespace {

constexpr int MAX_NATIVE = 4;
// INPUT value: 24-byte packet, 2 bytes extended buttons, 2 bytes press
// counter (LE) so the bridge can tell whether any press went missing.
constexpr size_t INPUT_LEN = EXT_PACKET_LEN + 2;
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
struct {
  uint32_t sent = 0, failed = 0;
  uint16_t intervalUnits = 0;  // of the first native connection, 1.25 ms units
} stats;
TaskHandle_t outTask = nullptr;

// Set by a handshake (new or repeated): the output loop starts the native
// stream afresh - new tap latch, status and current input sent right away.
std::atomic<bool> restartStream{false};

// Connections that did the handshake.
portMUX_TYPE nmux = portMUX_INITIALIZER_UNLOCKED;
uint16_t nativeConns[MAX_NATIVE];
uint32_t nativeSeen[MAX_NATIVE];   // millis() of the client's last CONTROL write
bool nativeLeased[MAX_NATIVE];     // handshook with protocol >= 3: must keep writing
uint8_t nativeProto[MAX_NATIVE];
int nativeCount = 0;

// INPUT subscriptions by connection (for the diagnostics page).
constexpr int MAX_SUBS = 8;
uint16_t inputSubs[MAX_SUBS];
int inputSubCount = 0;

// Connections subscribed to the Xbox pad's reports (the host's gamepad link).
uint16_t hidSubs[MAX_SUBS];
int hidSubCount = 0;

// Game-only links to drop once the game has left (Android keeps such a link up
// after the app dies, and a relaunch reusing it can stall): handle + when.
uint16_t dropConn[MAX_SUBS];
uint32_t dropAt[MAX_SUBS];
int dropCount = 0;

// One device at a time: the paired (encrypted) host currently connected.
uint16_t hostConn = BLE_HS_CONN_HANDLE_NONE;
char hostId[18] = "";
// Paired devices turned away (disconnected from housekeeping, not the callback).
uint16_t kickConn[MAX_SUBS];
int kickCount = 0;

// Pairing mode. Devices allowed to be paired: the bonds that existed when the
// board started, plus any paired while pairing mode was open. A device that
// pairs at any other time (an old host re-pairing by itself after being
// forgotten) is refused: its new bond is deleted and it's disconnected.
constexpr int MAX_ALLOWED = 8;
char allowed[MAX_ALLOWED][18];
int allowedCount = 0;
bool pairOpenUntilFirst = false;   // nothing paired: open until one device pairs
uint32_t pairUntil = 0;            // openPairing(): open until this millis()
char refuseAddr[MAX_SUBS][18];     // bonds to delete (from housekeeping)
uint16_t refuseConn[MAX_SUBS];
int refuseCount = 0;

bool isAllowed(const char *id) {
  for (int i = 0; i < allowedCount; i++)
    if (strcmp(allowed[i], id) == 0) return true;
  return false;
}
void allow(const char *id) {
  if (!isAllowed(id) && allowedCount < MAX_ALLOWED) snprintf(allowed[allowedCount++], 18, "%s", id);
}
void disallow(const char *id) {
  for (int i = 0; i < allowedCount; i++)
    if (strcmp(allowed[i], id) == 0) {
      memcpy(allowed[i], allowed[--allowedCount], 18);
      return;
    }
}
bool pairingOpen() { return pairOpenUntilFirst || (pairUntil && (int32_t)(pairUntil - millis()) > 0); }
/** Delete a pairing. NimBLE refuses (busy) to drop a peer with an IRK while
 *  advertising - and the board always advertises - so pause it; housekeeping
 *  starts advertising again. Returns true when the pairing is gone. */
bool deleteBondNow(const char *addr) {
  for (int b = 0; b < NimBLEDevice::getNumBonds(); b++) {
    NimBLEAddress a = NimBLEDevice::getBondedAddress(b);
    if (strcmp(a.toString().c_str(), addr) != 0) continue;
    bool adv = NimBLEDevice::getAdvertising()->isAdvertising();
    if (adv) NimBLEDevice::stopAdvertising();
    int rc = ble_gap_unpair(a.getBase());
    if (adv) NimBLEDevice::startAdvertising();
    if (rc != 0) Serial.printf("[BLE] couldn't delete the pairing with %s (rc %d)\n", addr, rc);
    return rc == 0;
  }
  return true;   // not paired (any more)
}

void snapshotBonds() {
  allowedCount = 0;
  for (int i = 0; i < NimBLEDevice::getNumBonds() && i < MAX_ALLOWED; i++)
    allow(NimBLEDevice::getBondedAddress(i).toString().c_str());
  pairOpenUntilFirst = allowedCount == 0;
}

bool hasHid(uint16_t h) {
  for (int i = 0; i < hidSubCount; i++)
    if (hidSubs[i] == h) return true;
  return false;
}

/** The game on this link left: if it isn't the gamepad link, disconnect it shortly. */
void dropIfGameOnly(uint16_t h) {
  portENTER_CRITICAL(&nmux);
  if (!hasHid(h) && dropCount < MAX_SUBS) {
    dropConn[dropCount] = h;
    dropAt[dropCount] = millis() + 500;
    dropCount++;
  }
  portEXIT_CRITICAL(&nmux);
}

// Recent events, for the screen (and the serial log).
constexpr int MAX_EVENTS = 12;
Event eventRing[MAX_EVENTS];
int eventHead = 0, eventTotal = 0;
portMUX_TYPE emux = portMUX_INITIALIZER_UNLOCKED;

void event(const char *fmt, ...) {
  Event e;
  e.ms = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&emux);
  eventRing[eventHead] = e;
  eventHead = (eventHead + 1) % MAX_EVENTS;
  eventTotal++;
  portEXIT_CRITICAL(&emux);
}

/** "B8:1F:3F:D8:27:31" -> "..27:31" style short name for event lines. */
String shortAddr(const NimBLEAddress &a) {
  String s = a.toString().c_str();
  return s.length() > 6 ? s.substring(s.length() - 5) : s;
}

inline NimBLEServer *s_server() { return server.load(std::memory_order_acquire); }

bool isNative(uint16_t h) {
  for (int i = 0; i < nativeCount; i++)
    if (nativeConns[i] == h) return true;
  return false;
}

int nativeIndex(uint16_t h) {
  for (int i = 0; i < nativeCount; i++)
    if (nativeConns[i] == h) return i;
  return -1;
}

void addNative(uint16_t h, bool leased) {
  bool added = false;
  portENTER_CRITICAL(&nmux);
  int i = nativeIndex(h);
  if (i < 0 && nativeCount < MAX_NATIVE) {
    i = nativeCount++;
    nativeConns[i] = h;
    added = true;
  }
  if (i >= 0) {
    nativeSeen[i] = millis();
    nativeLeased[i] = leased;
  }
  portEXIT_CRITICAL(&nmux);
  if (added) DivaState::nativeAcquire("BLE");
}

void removeNative(uint16_t h) {
  bool removed = false;
  portENTER_CRITICAL(&nmux);
  for (int i = 0; i < nativeCount; i++)
    if (nativeConns[i] == h) {
      --nativeCount;
      nativeConns[i] = nativeConns[nativeCount];
      nativeSeen[i] = nativeSeen[nativeCount];
      nativeLeased[i] = nativeLeased[nativeCount];
      nativeProto[i] = nativeProto[nativeCount];
      removed = true;
      break;
    }
  portEXIT_CRITICAL(&nmux);
  if (removed) DivaState::nativeRelease("BLE");
}

/** Any CONTROL write from a native client renews its lease. */
void touchNative(uint16_t h) {
  portENTER_CRITICAL(&nmux);
  int i = nativeIndex(h);
  if (i >= 0) nativeSeen[i] = millis();
  portEXIT_CRITICAL(&nmux);
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
      bool again = isNative(h);
      int ver = v.size() > 4 ? v.data()[4] : 0;
      Serial.printf("[BLE] handshake from %s (protocol %d, MTU %d)%s\n",
                    info.getAddress().toString().c_str(), ver, info.getMTU(),
                    again ? " - again: restarting its stream" : "");
      addNative(h, ver >= 3);
      portENTER_CRITICAL(&nmux);
      int ni = nativeIndex(h);
      if (ni >= 0) nativeProto[ni] = ver;
      portEXIT_CRITICAL(&nmux);
      event("%s v%d from %s", again ? "handshake again" : "handshake", ver, shortAddr(info.getAddress()).c_str());
      restartStream = true;
      if (outTask) xTaskNotifyGive(outTask);
      // 7.5 ms connection interval for the lowest latency.
      s_server()->updateConnParams(h, 6, 6, 0, 200);
    } else if (v.size() >= 3 && memcmp(v.data(), "BYE", 3) == 0) {
      Serial.printf("[BLE] %s said goodbye\n", info.getAddress().toString().c_str());
      event("goodbye from %s", shortAddr(info.getAddress()).c_str());
      removeNative(h);
      dropIfGameOnly(h);
    } else {
      touchNative(h);   // "HI" (or anything else): still here
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
    uint16_t h = info.getConnHandle();
    portENTER_CRITICAL(&nmux);
    int at = -1;
    for (int i = 0; i < inputSubCount; i++)
      if (inputSubs[i] == h) at = i;
    if (sub && at < 0 && inputSubCount < MAX_SUBS) inputSubs[inputSubCount++] = h;
    if (!sub && at >= 0) inputSubs[at] = inputSubs[--inputSubCount];
    portEXIT_CRITICAL(&nmux);
    event("input %s by %s", sub ? "subscribed" : "unsubscribed", shortAddr(info.getAddress()).c_str());
    if (sub == 0) removeNative(h);
  }
  void onRead(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    uint8_t buf[INPUT_LEN] = {};
    if (!DivaState::readExt(buf)) idlePacket(buf), buf[24] = buf[25] = 0;
    uint16_t n = DivaState::pressCount();
    buf[26] = n & 0xFF;
    buf[27] = n >> 8;
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
    snapshotBonds();
    if (pairOpenUntilFirst) {
      Serial.println("[BLE] nothing paired yet: pairing open for the first device");
      event("nothing paired: pairing open");
    }
    NimBLEDevice::setMTU(247);
    NimBLEService *svc = s->createService(DIVA_BLE_SERVICE_UUID);
    chControl = svc->createCharacteristic(
        DIVA_BLE_CONTROL_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY, 16);
    chInput = svc->createCharacteristic(
        DIVA_BLE_INPUT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY,
        INPUT_LEN);
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
  uint32_t now = millis();
  for (int i = 0; i < nativeCount; i++) {
    bool alive = false;
    for (uint16_t p : peers) alive |= p == nativeConns[i];
    // A leased client that stopped checking in is gone (killed game, OS kept the link)
    if (alive && nativeLeased[i] && now - nativeSeen[i] > LEASE_MS) {
      alive = false;
      Serial.printf("[BLE] native client (connection %u) went quiet for %u s - releasing it\n",
                    nativeConns[i], (unsigned)(LEASE_MS / 1000));
      event("game went quiet %us: released", (unsigned)(LEASE_MS / 1000));
    }
    if (!alive) stale[n++] = nativeConns[i];
  }
  portEXIT_CRITICAL(&nmux);
  for (int i = 0; i < n; i++) {
    removeNative(stale[i]);
    bool alive = false;
    for (uint16_t p : peers) alive |= p == stale[i];
    if (alive) dropIfGameOnly(stale[i]);   // went quiet: its link is a leftover
  }

  // Refused pairings: drop the link and the bond it just made.
  portENTER_CRITICAL(&nmux);
  char rAddr[MAX_SUBS][18];
  uint16_t rConn[MAX_SUBS];
  int nr = refuseCount;
  memcpy(rAddr, refuseAddr, sizeof(rAddr[0]) * nr);
  memcpy(rConn, refuseConn, sizeof(uint16_t) * nr);
  refuseCount = 0;
  portEXIT_CRITICAL(&nmux);
  for (int i = 0; i < nr; i++) {
    srv->disconnect(rConn[i]);
    deleteBondNow(rAddr[i]);
  }
  if (pairUntil && (int32_t)(pairUntil - now) <= 0) {
    pairUntil = 0;
    event("pairing mode closed");
  }

  // Turn away second paired devices (one device at a time).
  portENTER_CRITICAL(&nmux);
  uint16_t kicks[MAX_SUBS];
  int nk = kickCount;
  memcpy(kicks, kickConn, sizeof(uint16_t) * nk);
  kickCount = 0;
  portEXIT_CRITICAL(&nmux);
  for (int i = 0; i < nk; i++) srv->disconnect(kicks[i]);

  // Drop leftover game-only links (after a goodbye / a quiet game).
  for (int i = 0; i < dropCount;) {
    if ((int32_t)(now - dropAt[i]) < 0) {
      i++;
      continue;
    }
    uint16_t h = dropConn[i];
    portENTER_CRITICAL(&nmux);
    dropConn[i] = dropConn[--dropCount];
    dropAt[i] = dropAt[dropCount];
    bool keep = isNative(h) || hasHid(h);   // it handshook again / it's the pad link after all
    portEXIT_CRITICAL(&nmux);
    bool alive = false;
    for (uint16_t p : peers) alive |= p == h;
    // A paired (encrypted) link is the host's own gamepad link even when its HID
    // subscription was restored from the bond without an event: keep it.
    if (alive && srv->getPeerInfoByHandle(h).isEncrypted()) keep = true;
    if (alive && !keep) {
      event("closed the game's old link");
      srv->disconnect(h);
    }
  }

  // Track the connection interval the host actually granted.
  uint16_t first = BLE_HS_CONN_HANDLE_NONE;
  portENTER_CRITICAL(&nmux);
  if (nativeCount) first = nativeConns[0];
  portEXIT_CRITICAL(&nmux);
  uint16_t units = 0;
  if (first != BLE_HS_CONN_HANDLE_NONE) units = srv->getPeerInfoByHandle(first).getConnInterval();
  if (units != stats.intervalUnits) {
    stats.intervalUnits = units;
    if (units) Serial.printf("[BLE] bridge connection interval: %.2f ms\n", units * 1.25f);
  }

  // Connect / disconnect events (remember addresses: gone peers have none)
  static uint16_t known[8];
  static char knownAddr[8][8];
  static int knownCount = 0;
  for (uint16_t p : peers) {
    bool seen = false;
    for (int i = 0; i < knownCount; i++) seen |= known[i] == p;
    if (!seen && knownCount < 8) {
      known[knownCount] = p;
      snprintf(knownAddr[knownCount], sizeof(knownAddr[0]), "%s",
               shortAddr(srv->getPeerInfoByHandle(p).getAddress()).c_str());
      event("connected: %s", knownAddr[knownCount]);
      knownCount++;
    }
  }
  for (int i = 0; i < knownCount;) {
    bool alive = false;
    for (uint16_t p : peers) alive |= known[i] == p;
    if (alive) {
      i++;
      continue;
    }
    portENTER_CRITICAL(&nmux);
    for (int k = 0; k < inputSubCount; k++)
      if (inputSubs[k] == known[i]) inputSubs[k--] = inputSubs[--inputSubCount];
    for (int k = 0; k < hidSubCount; k++)
      if (hidSubs[k] == known[i]) hidSubs[k--] = hidSubs[--hidSubCount];
    portEXIT_CRITICAL(&nmux);
    known[i] = known[knownCount - 1];
    memcpy(knownAddr[i], knownAddr[knownCount - 1], sizeof(knownAddr[0]));
    knownCount--;
  }

  // A game link left at Android's "balanced" 30-50 ms interval adds that much
  // lag: ask for 7.5-15 ms again, a few times, a few seconds apart.
  static uint16_t slowConn = BLE_HS_CONN_HANDLE_NONE;
  static uint8_t slowTries = 0;
  static uint32_t slowAsked = 0;
  uint16_t nat[MAX_NATIVE];
  int nn = 0;
  portENTER_CRITICAL(&nmux);
  for (int i = 0; i < nativeCount; i++) nat[nn++] = nativeConns[i];
  portEXIT_CRITICAL(&nmux);
  for (int i = 0; i < nn; i++) {
    uint16_t units = srv->getPeerInfoByHandle(nat[i]).getConnInterval();
    if (units <= 12) continue;   // 15 ms or better
    if (nat[i] != slowConn) {
      slowConn = nat[i];
      slowTries = 0;
      slowAsked = now - 3000;
    }
    if (slowTries < 5 && now - slowAsked >= 3000) {
      slowTries++;
      slowAsked = now;
      srv->updateConnParams(nat[i], 6, 12, 0, 200);
      event("game link %.1f ms: asked for faster", units * 1.25f);
    }
    break;
  }

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
  uint32_t lastSeq = ~0u, lastSend = 0, lastHouse = 0, lastNative = 0;
  uint8_t lastStatus[8] = {};
  bool wasNative = false, padDirty = true, nativeDirty = true, padWasUp = false;
  uint8_t pkt[EXT_PACKET_LEN];
  uint8_t npkt[INPUT_LEN];
  int slot = DivaState::latchOpen();  // presses between notifications aren't lost
  while (true) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MIN_SEND_MS));
    uint32_t now = millis();
    if (now - lastHouse >= 250) {
      lastHouse = now;
      housekeeping();
    }
    NimBLEServer *srv = s_server();
    if (!srv) continue;

    if (restartStream.exchange(false)) {
      DivaState::latchClose(slot);  // forget taps meant for the previous session
      slot = DivaState::latchOpen();
      memset(lastStatus, 0, sizeof(lastStatus));  // re-send status
      nativeDirty = true;                         // and the current input
      lastSend = 0;
    }
    // Heartbeat: a native client hears from us at least every HEARTBEAT_MS,
    // so a relay can tell a dead stream from an idle controller.
    if (nativeCount > 0 && now - lastNative >= HEARTBEAT_MS) nativeDirty = true;

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
      if (!DivaState::readExtLatched(slot, npkt)) {
        idlePacket(npkt);
        npkt[24] = npkt[25] = 0;
      }
      uint16_t n = DivaState::pressCount();
      npkt[26] = n & 0xFF;
      npkt[27] = n >> 8;
      chInput->setValue(npkt, INPUT_LEN);
      if (chInput->notify()) {
        stats.sent++;
        nativeDirty = false;
        lastNative = now;
      } else {
        // No buffer free (link busy): keep the presses and retry next tick.
        stats.failed++;
        DivaState::latchRestore(slot, npkt, PACKET_LEN, npkt[24] | npkt[25] << 8);
      }
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

float intervalMs() { return stats.intervalUnits * 1.25f; }
uint32_t notifySent() { return stats.sent; }
uint32_t notifyFailed() { return stats.failed; }

int nativeBleClients() { return nativeCount; }

int peers(PeerInfo *out, int max) {
  NimBLEServer *srv = s_server();
  if (!srv) return 0;
  int n = 0;
  uint32_t now = millis();
  for (uint16_t h : srv->getPeerDevices()) {
    if (n >= max) break;
    NimBLEConnInfo ci = srv->getPeerInfoByHandle(h);
    PeerInfo &p = out[n++];
    snprintf(p.addr, sizeof(p.addr), "%s", ci.getAddress().toString().c_str());
    p.intervalMs = ci.getConnInterval() * 1.25f;
    p.mtu = ci.getMTU();
    portENTER_CRITICAL(&nmux);
    int i = nativeIndex(h);
    p.native = i >= 0;
    p.proto = i >= 0 ? nativeProto[i] : 0;
    p.quietMs = i >= 0 ? now - nativeSeen[i] : 0;
    p.inputSub = false;
    for (int k = 0; k < inputSubCount; k++) p.inputSub |= inputSubs[k] == h;
    p.hid = hasHid(h);
    portEXIT_CRITICAL(&nmux);
  }
  return n;
}

int pairedCount() { return NimBLEDevice::getNumBonds(); }

void openPairing(uint32_t seconds) {
  pairUntil = millis() + seconds * 1000;
  if (!pairUntil) pairUntil = 1;
  event("pairing mode open (%us)", (unsigned)seconds);
  Serial.printf("[BLE] pairing mode open for %u s\n", (unsigned)seconds);
}

void closePairing() {
  pairUntil = 0;
  pairOpenUntilFirst = false;
  event("pairing mode closed");
}

int pairingLeft() {
  if (pairOpenUntilFirst) return -1;
  if (!pairUntil) return 0;
  int32_t left = (int32_t)(pairUntil - millis());
  return left > 0 ? (left + 999) / 1000 : 0;
}

int bonded(BondInfo *out, int max) {
  NimBLEServer *srv = s_server();
  int n = min(max, NimBLEDevice::getNumBonds());
  std::vector<uint16_t> peers;
  if (srv) peers = srv->getPeerDevices();
  for (int i = 0; i < n; i++) {
    String a = NimBLEDevice::getBondedAddress(i).toString().c_str();
    BondInfo &b = out[i];
    snprintf(b.addr, sizeof(b.addr), "%s", a.c_str());
    b.connected = false;
    for (uint16_t p : peers)
      if (a == srv->getPeerInfoByHandle(p).getIdAddress().toString().c_str()) b.connected = true;
    portENTER_CRITICAL(&nmux);
    b.host = a == hostId;
    portEXIT_CRITICAL(&nmux);
  }
  return n;
}

void disconnectDevice(const char *addr) {
  NimBLEServer *srv = s_server();
  if (!srv) return;
  for (uint16_t p : srv->getPeerDevices())
    if (String(addr) == srv->getPeerInfoByHandle(p).getIdAddress().toString().c_str()) srv->disconnect(p);
  event("disconnected %s from the screen", addr + (strlen(addr) > 5 ? strlen(addr) - 5 : 0));
}

void forgetDevice(const char *addr) {
  disconnectDevice(addr);
  portENTER_CRITICAL(&nmux);
  disallow(addr);   // first: a reconnect in between is then refused, not re-allowed
  portEXIT_CRITICAL(&nmux);
  bool ok = deleteBondNow(addr);
  event(ok ? "forgot %s" : "couldn't forget %s", addr + (strlen(addr) > 5 ? strlen(addr) - 5 : 0));
  Serial.printf("[BLE] forgot paired device %s\n", addr);
}

bool advertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  return adv && adv->isAdvertising();
}

int events(Event *out, int max) {
  portENTER_CRITICAL(&emux);
  int n = min(max, min(eventTotal, MAX_EVENTS));
  for (int i = 0; i < n; i++) out[i] = eventRing[(eventHead - 1 - i + MAX_EVENTS) % MAX_EVENTS];
  portEXIT_CRITICAL(&emux);
  return n;
}

int connections() {
  NimBLEServer *srv = s_server();
  return srv ? srv->getConnectedCount() : 0;
}

}  // namespace BleBridge

// Called by the (patched) Xbox HID lib when a connection (un)subscribes to the
// pad's reports.
void divaHidSubscribed(uint16_t connHandle, uint16_t subValue) {
  using namespace BleBridge;
  portENTER_CRITICAL(&nmux);
  int at = -1;
  for (int i = 0; i < hidSubCount; i++)
    if (hidSubs[i] == connHandle) at = i;
  if (subValue && at < 0 && hidSubCount < MAX_SUBS) hidSubs[hidSubCount++] = connHandle;
  if (!subValue && at >= 0) hidSubs[at] = hidSubs[--hidSubCount];
  portEXIT_CRITICAL(&nmux);
}

// A paired device finished encrypting (new pairing, or a known one coming back).
void divaAuthComplete(NimBLEConnInfo &info) {
  using namespace BleBridge;
  if (!info.isEncrypted()) return;
  uint16_t h = info.getConnHandle();
  String id = info.getIdAddress().toString().c_str();
  String shortId = id.length() > 6 ? id.substring(id.length() - 5) : id;
  // Pairing mode: is this device allowed to be paired?
  bool refused = false, newlyPaired = false;
  portENTER_CRITICAL(&nmux);
  if (!isAllowed(id.c_str())) {
    if (pairingOpen()) {
      allow(id.c_str());
      pairOpenUntilFirst = false;
      pairUntil = 0;   // one device per pairing window
      newlyPaired = true;
    } else if (refuseCount < MAX_SUBS) {
      snprintf(refuseAddr[refuseCount], 18, "%s", id.c_str());
      refuseConn[refuseCount++] = h;
      refused = true;
    }
  }
  portEXIT_CRITICAL(&nmux);
  if (refused) {
    static uint32_t lastRefuse = 0;
    if (millis() - lastRefuse > 30000) {
      lastRefuse = millis();
      event("refused %s: not in pairing mode", shortId.c_str());
      Serial.printf("[BLE] %s tried to pair outside pairing mode: refused\n", id.c_str());
    }
    return;
  }
  if (newlyPaired) {
    event("new device paired: %s", shortId.c_str());
    Serial.printf("[BLE] paired %s (pairing mode closed)\n", id.c_str());
  }
  bool kick = false;
  portENTER_CRITICAL(&nmux);
  if (hostConn == BLE_HS_CONN_HANDLE_NONE || Settings::get().multiHost || id == hostId) {
    hostConn = h;
    snprintf(hostId, sizeof(hostId), "%s", id.c_str());
  } else if (kickCount < MAX_SUBS) {
    kickConn[kickCount++] = h;
    kick = true;
  }
  portEXIT_CRITICAL(&nmux);
  if (kick) {
    static uint32_t lastNote = 0;   // a turned-away device retries: note it now and then
    if (millis() - lastNote > 30000) {
      lastNote = millis();
      event("turned away %s (one device at a time)", shortId.c_str());
      Serial.printf("[BLE] %s turned away: %s is connected (set hosts many to allow both)\n", id.c_str(), hostId);
    }
  } else {
    event("paired device: %s", shortId.c_str());
  }
}

void divaDisconnected(NimBLEConnInfo &info, int reason) {
  using namespace BleBridge;
  uint16_t h = info.getConnHandle();
  bool wasHost = false;
  portENTER_CRITICAL(&nmux);
  if (h == hostConn) {
    hostConn = BLE_HS_CONN_HANDLE_NONE;
    hostId[0] = 0;
    wasHost = true;
  }
  portEXIT_CRITICAL(&nmux);
  int code = reason & 0xFF;   // NimBLE: 0x200 + HCI reason
  const char *why = code == 0x13 ? "closed by it" : code == 0x16 ? "closed by us" : code == 0x08 ? "timed out"
                  : code == 0x3E ? "failed to connect" : code == 0x22 || code == 0x28 ? "link error" : "";
  String a = info.getAddress().toString().c_str();
  String sa = a.length() > 6 ? a.substring(a.length() - 5) : a;
  if (*why) event("disconnected: %s, %s%s", sa.c_str(), why, wasHost ? " (host)" : "");
  else event("disconnected: %s, 0x%02X%s", sa.c_str(), code, wasHost ? " (host)" : "");
}
