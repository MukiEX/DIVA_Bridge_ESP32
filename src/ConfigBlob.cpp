#include "ConfigBlob.h"

#include <string.h>

namespace ConfigBlob {

namespace {
void put16(uint8_t *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t get32(const uint8_t *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
}  // namespace

uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
  }
  return ~c;
}

bool encode(const char *payload, size_t len, uint32_t serial, uint8_t *out) {
  if (len > MAX_PAYLOAD) return false;
  memset(out, 0xFF, PARTITION_SIZE);
  memcpy(out, "DVCF", 4);
  put16(out + 4, VERSION);
  put16(out + 6, (uint16_t)len);
  put32(out + 8, serial);
  put32(out + 12, crc32((const uint8_t *)payload, len));
  memcpy(out + HEADER_LEN, payload, len);
  return true;
}

bool decode(const uint8_t *blob, size_t blobLen, const char *&payload,
            size_t &len, uint32_t &serial) {
  if (blobLen < HEADER_LEN || memcmp(blob, "DVCF", 4) != 0) return false;
  if (get16(blob + 4) != VERSION) return false;
  len = get16(blob + 6);
  if (len > MAX_PAYLOAD || HEADER_LEN + len > blobLen) return false;
  if (crc32(blob + HEADER_LEN, len) != get32(blob + 12)) return false;
  serial = get32(blob + 8);
  payload = (const char *)blob + HEADER_LEN;
  return true;
}

}  // namespace ConfigBlob
