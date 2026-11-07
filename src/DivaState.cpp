#include "DivaState.h"

#include <string.h>

#include <atomic>

using namespace diva;

namespace DivaState {

namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Options opts;

const Profile *profile = nullptr;
uint16_t devVid = 0, devPid = 0;

uint8_t latest[MAX_IN_PACKET];
size_t latestLen = 0;  // 0 = nothing received yet
uint16_t latestExt = 0;
std::atomic<uint32_t> seq{0};

uint8_t leds[MAX_DEVICE_OUT];
size_t ledsLen = 0;
bool ledsNew = false;

int natives = 0;  // guarded by mux

constexpr int MAX_SLOTS = 8;
constexpr size_t LATCH_FROM = 3, LATCH_TO = 10;  // bytes 3..9: buttons + slider
struct Slot {
  bool open = false;
  uint8_t bytes[LATCH_TO];
  uint16_t ext = 0;
};
Slot slots[MAX_SLOTS];
uint16_t presses = 0;
uint8_t prevButtons = 0;
uint16_t prevExt = 0;

TaskHandle_t ledListener = nullptr;
TaskHandle_t inputListener = nullptr;

void wake(TaskHandle_t t) {
  if (t) xTaskNotifyGive(t);
}
}  // namespace

void begin() {}

Options &options() { return opts; }

void setController(const Profile *p, uint16_t vid, uint16_t pid) {
  portENTER_CRITICAL(&mux);
  for (auto &sl : slots)
    if (sl.open) {
      memset(sl.bytes, 0, sizeof(sl.bytes));
      sl.ext = 0;
    }
  prevButtons = 0;
  prevExt = 0;
  profile = p;
  devVid = p ? vid : 0;
  devPid = p ? pid : 0;
  latestLen = 0;
  latestExt = 0;
  ledsNew = false;
  seq++;
  portEXIT_CRITICAL(&mux);
  wake(inputListener);
}

void setInput(const uint8_t *pkt, size_t len, uint16_t ext) {
  if (len > MAX_IN_PACKET) len = MAX_IN_PACKET;
  uint8_t full[PACKET_LEN] = {};
  memcpy(full, pkt, len < PACKET_LEN ? len : PACKET_LEN);
  uint8_t buttons = decode(full).pressed;
  portENTER_CRITICAL(&mux);
  memcpy(latest, pkt, len);
  latestLen = len;
  latestExt = ext;
  for (auto &sl : slots)
    if (sl.open) {
      for (size_t i = LATCH_FROM; i < LATCH_TO; i++) sl.bytes[i] |= full[i];
      sl.ext |= ext;
    }
  // Count press edges (what a game would count as a hit).
  presses += __builtin_popcount(buttons & ~prevButtons) + __builtin_popcount(ext & ~prevExt);
  prevButtons = buttons;
  prevExt = ext;
  seq++;
  portEXIT_CRITICAL(&mux);
  wake(inputListener);
}

size_t takeLeds(uint8_t *out, size_t max) {
  size_t n = 0;
  portENTER_CRITICAL(&mux);
  if (ledsNew) {
    n = ledsLen < max ? ledsLen : max;
    memcpy(out, leds, n);
    ledsNew = false;
  }
  portEXIT_CRITICAL(&mux);
  return n;
}

void setLedListener(TaskHandle_t task) { ledListener = task; }
void setInputListener(TaskHandle_t task) { inputListener = task; }

const Profile *controller() { return profile; }

void controllerIds(uint16_t &vid, uint16_t &pid) {
  portENTER_CRITICAL(&mux);
  vid = devVid;
  pid = devPid;
  portEXIT_CRITICAL(&mux);
}

bool present() { return profile != nullptr; }

bool readPacket(uint8_t *out, size_t max, size_t &len) {
  bool ok;
  portENTER_CRITICAL(&mux);
  ok = profile != nullptr;
  if (ok) {
    if (latestLen == 0) {
      // Nothing received yet: report "header, nothing pressed".
      uint8_t idle[PACKET_LEN];
      idlePacket(idle);
      len = max < PACKET_LEN ? max : PACKET_LEN;
      memcpy(out, idle, len);
    } else {
      len = max < latestLen ? max : latestLen;
      memcpy(out, latest, len);
    }
  }
  portEXIT_CRITICAL(&mux);
  return ok;
}

bool readExt(uint8_t out[EXT_PACKET_LEN]) {
  bool ok;
  portENTER_CRITICAL(&mux);
  ok = profile != nullptr;
  if (ok) {
    memset(out, 0, EXT_PACKET_LEN);
    if (latestLen == 0) {
      idlePacket(out);
    } else {
      memcpy(out, latest, latestLen < PACKET_LEN ? latestLen : PACKET_LEN);
    }
    out[PACKET_LEN] = latestExt & 0xFF;
    out[PACKET_LEN + 1] = latestExt >> 8;
  }
  portEXIT_CRITICAL(&mux);
  return ok;
}

bool queueLeds(const uint8_t *data, size_t len) {
  if (len > MAX_DEVICE_OUT) len = MAX_DEVICE_OUT;
  bool ok;
  portENTER_CRITICAL(&mux);
  ok = profile != nullptr;
  if (ok) {
    memcpy(leds, data, len);
    ledsLen = len;
    ledsNew = true;
  }
  portEXIT_CRITICAL(&mux);
  if (ok) wake(ledListener);
  return ok;  // accepted (dropped later for controllers without LEDs)
}

uint32_t inputSeq() { return seq; }

uint16_t pressCount() { return presses; }

int latchOpen() {
  int found = NO_SLOT;
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < MAX_SLOTS; i++)
    if (!slots[i].open) {
      slots[i].open = true;
      memset(slots[i].bytes, 0, sizeof(slots[i].bytes));
      slots[i].ext = 0;
      found = i;
      break;
    }
  portEXIT_CRITICAL(&mux);
  return found;
}

void latchClose(int slot) {
  if (slot < 0 || slot >= MAX_SLOTS) return;
  portENTER_CRITICAL(&mux);
  slots[slot].open = false;
  portEXIT_CRITICAL(&mux);
}

namespace {
/** Fold the slot into a packet (caller holds mux) and reset the slot. */
void applySlot(int slot, uint8_t *pkt, size_t len, uint16_t *ext) {
  if (slot < 0 || slot >= MAX_SLOTS || !slots[slot].open) return;
  Slot &sl = slots[slot];
  for (size_t i = LATCH_FROM; i < LATCH_TO && i < len; i++) pkt[i] |= sl.bytes[i];
  if (ext) *ext |= sl.ext;
  memset(sl.bytes, 0, sizeof(sl.bytes));
  sl.ext = 0;
}
}  // namespace

bool readPacketLatched(int slot, uint8_t *out, size_t max, size_t &len) {
  bool ok;
  portENTER_CRITICAL(&mux);
  ok = profile != nullptr;
  if (ok) {
    uint8_t buf[MAX_IN_PACKET];
    size_t n;
    if (latestLen == 0) {
      idlePacket(buf);
      n = PACKET_LEN;
    } else {
      memcpy(buf, latest, latestLen);
      n = latestLen;
    }
    applySlot(slot, buf, n, nullptr);
    len = max < n ? max : n;
    memcpy(out, buf, len);
  }
  portEXIT_CRITICAL(&mux);
  return ok;
}

bool readExtLatched(int slot, uint8_t out[EXT_PACKET_LEN]) {
  bool ok;
  portENTER_CRITICAL(&mux);
  ok = profile != nullptr;
  if (ok) {
    memset(out, 0, EXT_PACKET_LEN);
    if (latestLen == 0) idlePacket(out);
    else memcpy(out, latest, latestLen < PACKET_LEN ? latestLen : PACKET_LEN);
    uint16_t ext = latestExt;
    applySlot(slot, out, PACKET_LEN, &ext);
    out[PACKET_LEN] = ext & 0xFF;
    out[PACKET_LEN + 1] = ext >> 8;
  }
  portEXIT_CRITICAL(&mux);
  return ok;
}

void latchRestore(int slot, const uint8_t *pkt, size_t len, uint16_t ext) {
  if (slot < 0 || slot >= MAX_SLOTS) return;
  portENTER_CRITICAL(&mux);
  if (slots[slot].open) {
    for (size_t i = LATCH_FROM; i < LATCH_TO && i < len; i++) slots[slot].bytes[i] |= pkt[i];
    slots[slot].ext |= ext;
  }
  portEXIT_CRITICAL(&mux);
}

void nativeAcquire(const char *who) {
  portENTER_CRITICAL(&mux);
  int n = ++natives;
  portEXIT_CRITICAL(&mux);
  Serial.printf("[Native] %s handshake - native clients: %d\n", who, n);
  wake(inputListener);
}

void nativeRelease(const char *who) {
  portENTER_CRITICAL(&mux);
  int n = natives > 0 ? --natives : 0;
  portEXIT_CRITICAL(&mux);
  Serial.printf("[Native] %s left - native clients: %d\n", who, n);
  wake(inputListener);
}

int nativeClients() { return natives; }

}  // namespace DivaState
