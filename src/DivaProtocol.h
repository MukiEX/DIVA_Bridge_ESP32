/**
 * @file DivaProtocol.h
 * @brief The DIVA Bridge wire format and per-controller translation.
 *
 * A straight port of the controller profiles in diva_bridge.py
 * (MukiEX/DIVA_Bridge_project): every supported controller is presented as a
 * Divaller - the 24-byte "BVZ" input packet TLAC reads, plus 2 bytes of
 * extended buttons for BridgeSlider - and the 100-byte "DLa" LED packet is
 * translated back to whatever the controller understands.
 *
 * Plain C++ with no Arduino/ESP-IDF dependencies, so it can be unit-tested on
 * a PC against the Python reference (see test/).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace diva {

constexpr uint8_t HEADER[3] = {0x42, 0x56, 0x5A};     // "BVZ"
constexpr uint8_t LED_HEADER[3] = {0x44, 0x4C, 0x61}; // "DLa"
constexpr size_t PACKET_LEN = 24;  // TLAC's DivallerState
constexpr size_t EXT_PACKET_LEN = PACKET_LEN + 2;  // + extended buttons (LE)
constexpr size_t LED_LEN = 100;
constexpr size_t MAX_IN_PACKET = 64;    // longest input packet we pass on
constexpr size_t MAX_DEVICE_OUT = 128;  // longest OUT report we pass on

// TCP port the PC bridge and DivallerBridge.dva use by default.
constexpr uint16_t DEFAULT_PORT = 45710;

/** The eight Divaller buttons, in the order used for "pressed" bitmasks. */
enum Button : uint8_t {
  BTN_FN = 0,
  BTN_SQUARE,
  BTN_CIRCLE,
  BTN_TRIANGLE,
  BTN_CROSS,
  BTN_L3,
  BTN_L2,
  BTN_L1,
  BTN_COUNT
};

/** Extended buttons the Divaller packet has no room for ('X' requests). */
enum Ext : uint16_t {
  EXT_UP = 1 << 0,
  EXT_DOWN = 1 << 1,
  EXT_LEFT = 1 << 2,
  EXT_RIGHT = 1 << 3,
  EXT_LB = 1 << 4,
  EXT_RB = 1 << 5,
  EXT_LT = 1 << 6,
  EXT_RT = 1 << 7,
  EXT_LS = 1 << 8,
  EXT_RS = 1 << 9,
};

/** Controller types, numbered the same way in the BLE status characteristic. */
enum class Kind : uint8_t {
  None = 0,
  Divaller = 1,
  ZhouSensor = 2,
  Hori = 3,     // NSW-230
  HoriPs4 = 4,  // PS4-161
};

struct Profile {
  Kind kind;
  const char *key;   // same keys as diva_bridge.py --device
  const char *name;  // same names as diva_bridge.py (the GUI matches on them)
  uint16_t vid, pid;
  uint8_t epIn, epOut;  // preferred endpoints; others are used if missing
  uint8_t minIn;        // shortest usable IN report
  const char *ledNote;
};

/** Known controller for this VID/PID, or nullptr. */
const Profile *findProfile(uint16_t vid, uint16_t pid);
const Profile *profileForKind(Kind kind);

/** Message for a recognised but unusable VID/PID (wrong mode), or nullptr. */
const char *wrongModeHint(uint16_t vid, uint16_t pid);

/** Options shared by the translators. */
struct Options {
  bool sliderReverse = false;
  bool ledReverse = false;
  bool leds = true;  // forward LEDs to the controller at all
};

/** An idle packet: header, nothing pressed. */
void idlePacket(uint8_t pkt[PACKET_LEN]);

/**
 * Build a Divaller packet from a pressed-button mask (bit n = Button n) and a
 * 32-bit sensor mask where bit i is TLAC sensor i (0 = leftmost).
 */
void buildPacket(uint8_t pkt[PACKET_LEN], uint8_t pressed, uint32_t sensors);

/**
 * Device IN report -> Divaller packet + extended buttons.
 * Translated controllers always give 24 bytes; a real Divaller's report is
 * passed through unchanged (up to MAX_IN_PACKET bytes), as the PC bridge does.
 * @return packet length, or 0 when the report should be skipped (too short,
 *         not an input report).
 */
size_t toDivaller(const Profile &p, const Options &o, const uint8_t *data,
                  size_t len, uint8_t pkt[MAX_IN_PACKET], uint16_t &ext);

/**
 * 100-byte "DLa" LED packet -> device OUT report.
 * @return length written to out (<= MAX_DEVICE_OUT), 0 to drop it.
 */
size_t ledsToDevice(const Profile &p, const Options &o, const uint8_t *dla,
                    size_t len, uint8_t *out);

/** Decode a Divaller packet the way TLAC does. */
struct Decoded {
  uint8_t pressed;   // bit n = Button n
  uint32_t sensors;  // bit i = TLAC sensor i, 0 = leftmost
};
Decoded decode(const uint8_t pkt[PACKET_LEN]);

uint32_t reverse32(uint32_t v);

}  // namespace diva
