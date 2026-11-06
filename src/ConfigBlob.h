/**
 * @file ConfigBlob.h
 * @brief The settings blob kept in the "divacfg" flash partition, so the web
 * flasher can write (and read back) settings without a serial console.
 *
 * Layout (little-endian), at the start of the 4 KB partition:
 *   0  char[4]  magic "DVCF"
 *   4  u16      format version (1)
 *   6  u16      payload length N
 *   8  u32      serial - the flasher picks a new one per write; the firmware
 *               applies a blob once per serial
 *  12  u32      CRC-32 (zlib/IEEE) of the payload
 *  16  N bytes  payload: UTF-8 "key=value\n" lines, same keys as the serial
 *               console's "set" command (plus "unpair=1")
 * The rest of the partition is 0xFF. tools/flasher reads/writes the same
 * format (see test/test_config_blob.py).
 *
 * Plain C++, no ESP-IDF, so it can be tested on a PC.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ConfigBlob {

constexpr uint32_t PARTITION_SIZE = 0x1000;
constexpr size_t HEADER_LEN = 16;
constexpr size_t MAX_PAYLOAD = PARTITION_SIZE - HEADER_LEN;
constexpr uint16_t VERSION = 1;

uint32_t crc32(const uint8_t *data, size_t len);

/** Fill `out` (PARTITION_SIZE bytes) with a blob. false if payload too big. */
bool encode(const char *payload, size_t len, uint32_t serial, uint8_t *out);

/**
 * Check a blob. On success points `payload` into `blob` and sets len/serial.
 * @return false for an erased partition, bad magic/version/length or CRC.
 */
bool decode(const uint8_t *blob, size_t blobLen, const char *&payload,
            size_t &len, uint32_t &serial);

}  // namespace ConfigBlob
