#include "NetProtocol.h"

#include <string.h>

#include "DivaState.h"  // Arduino.h first, or lwIP's headers clash with it

#ifdef ARDUINO
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#endif

using namespace diva;

namespace NetProtocol {

namespace {

bool recvAll(int s, uint8_t *buf, size_t n) {
  while (n) {
    int r = recv(s, buf, n, 0);
    if (r <= 0) return false;
    buf += r;
    n -= r;
  }
  return true;
}

bool sendAll(int s, const uint8_t *buf, size_t n) {
  while (n) {
    int r = send(s, buf, n, 0);
    if (r <= 0) return false;
    buf += r;
    n -= r;
  }
  return true;
}

}  // namespace

void serve(int s, const char *who) {
  // Games poll continuously; a client silent this long has gone (PC asleep,
  // Wi-Fi dropped) - let it go so the Xbox pad comes back.
  struct timeval tv = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  bool native = false;
  // Our own latch: a press between two of this client's polls still shows up.
  int slot = DivaState::latchOpen();
  uint8_t buf[2 + 256];
  while (true) {
    uint8_t op;
    if (!recvAll(s, &op, 1)) break;
    if (op != 'P' && op != 'R' && op != 'X' && op != 'W') {
      Serial.printf("[Net] unknown opcode 0x%02x from %s, dropping client\n", op, who);
      break;
    }
    if (!native) {
      native = true;
      DivaState::nativeAcquire("TCP");
    }
    if (op == 'P') {
      uint8_t st = DivaState::present() ? 1 : 0;
      if (!sendAll(s, &st, 1)) break;
    } else if (op == 'R' || op == 'X') {
      uint8_t req[2];  // ep, max
      if (!recvAll(s, req, 2)) break;
      size_t max = req[1], len = 0;
      bool ok;
      if (op == 'R') {
        ok = DivaState::readPacketLatched(slot, buf + 2, max, len);
      } else {
        uint8_t ext[EXT_PACKET_LEN];
        ok = DivaState::readExtLatched(slot, ext);
        len = max < EXT_PACKET_LEN ? max : EXT_PACKET_LEN;
        memcpy(buf + 2, ext, len);
      }
      buf[0] = ok ? 1 : 0;
      buf[1] = ok ? len : 0;
      if (!sendAll(s, buf, ok ? 2 + len : 2)) break;
    } else {  // 'W'
      uint8_t req[2];  // ep, len
      if (!recvAll(s, req, 2)) break;
      uint8_t data[256];
      if (!recvAll(s, data, req[1])) break;
      uint8_t st = DivaState::queueLeds(data, req[1]) ? 1 : 0;
      if (!sendAll(s, &st, 1)) break;
    }
  }
  DivaState::latchClose(slot);
  if (native) DivaState::nativeRelease("TCP");
}

}  // namespace NetProtocol
