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
  portENTER_CRITICAL(&mux);
  memcpy(latest, pkt, len);
  latestLen = len;
  latestExt = ext;
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
