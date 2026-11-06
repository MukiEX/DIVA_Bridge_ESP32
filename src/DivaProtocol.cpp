/**
 * @file DivaProtocol.cpp
 * @brief Controller -> Divaller translation, ported from diva_bridge.py.
 */

#include "DivaProtocol.h"

#include <string.h>

namespace diva {

namespace {

struct BitPos {
  uint8_t byte, bit;
};

// Where each Divaller button lives in the input packet (Button order).
constexpr BitPos DIVALLER_BITS[BTN_COUNT] = {
    {3, 1},  // fn
    {3, 2},  // square
    {3, 3},  // circle
    {3, 4},  // triangle
    {3, 5},  // cross
    {4, 6},  // l3
    {4, 7},  // l2
    {5, 0},  // l1
};

constexpr Profile PROFILES[] = {
    {Kind::Divaller, "divaller", "Divaller", 0x0E8F, 0x2213, 0x84, 0x03, 1,
     "slider + button LEDs"},
    {Kind::ZhouSensor, "zhousensor", "ZhouSensor HDD", 0x1973, 0x8000, 0x81,
     0x02, 7, "slider LEDs (button LEDs are set in the board's menu)"},
    {Kind::Hori, "hori", "HORI NSW-230", 0x0F0D, 0x00FB, 0x81, 0x02, 7,
     "no LED control (the HORI lights itself)"},
    {Kind::HoriPs4, "hori_ps4", "HORI PS4-161", 0x0F0D, 0x013C, 0x84, 0x03, 8,
     "no LED control (the HORI lights itself)"},
};

struct WrongMode {
  uint16_t vid, pid;
  const char *msg;
};
constexpr WrongMode WRONG_MODE[] = {
    {0x1973, 0x1020, "ZhouSensor in Keyboard mode - switch it to HDD mode"},
    {0x0F0D, 0x0092,
     "HORI-type pad 0F0D:0092 (also ZhouSensor NS mode) - for a ZhouSensor, "
     "switch it to HDD mode"},
};

// Switch / PlayStation hat value (0 = up, clockwise, 8 = centred).
uint16_t hatToDpad(uint8_t hat) {
  switch (hat & 0x0F) {
    case 0: return EXT_UP;
    case 1: return EXT_UP | EXT_RIGHT;
    case 2: return EXT_RIGHT;
    case 3: return EXT_DOWN | EXT_RIGHT;
    case 4: return EXT_DOWN;
    case 5: return EXT_DOWN | EXT_LEFT;
    case 6: return EXT_LEFT;
    case 7: return EXT_UP | EXT_LEFT;
    default: return 0;
  }
}

inline bool bitAt(const uint8_t *d, BitPos p) { return d[p.byte] >> p.bit & 1; }

/** HORI arcade mode: the 32 touch cells spread over LX, LY, RX, RY, each XOR
 * 0x80. Touched bit 31 is the leftmost cell; returns bit i = TLAC sensor i. */
uint32_t sticksToMask(const uint8_t axes[4], bool sliderReverse) {
  uint32_t touched = (uint32_t)(axes[0] ^ 0x80) |
                     (uint32_t)(axes[1] ^ 0x80) << 8 |
                     (uint32_t)(axes[2] ^ 0x80) << 16 |
                     (uint32_t)(axes[3] ^ 0x80) << 24;
  return sliderReverse ? touched : reverse32(touched);
}

// ------------------------------------------------------------- ZhouSensor
// IN (EP 0x81, 7 bytes): [0] bit0 triangle, bit1 square, bit2 circle, bit3
// cross; [1] bits4-7 D-pad; [2] bit0 minus, bit1 plus, bit2 capture, bit3
// home; [3..6] 32-bit slider mask, LE.
constexpr BitPos ZHOU_BUTTONS[BTN_COUNT] = {
    {2, 1},  // fn       <- plus
    {0, 1},  // square
    {0, 2},  // circle
    {0, 0},  // triangle
    {0, 3},  // cross
    {2, 3},  // l3       <- home
    {2, 2},  // l2       <- capture
    {2, 0},  // l1       <- minus
};
struct ExtPos {
  uint16_t ext;
  BitPos pos;
};
constexpr ExtPos ZHOU_EXT[] = {
    {EXT_UP, {1, 4}}, {EXT_DOWN, {1, 5}}, {EXT_LEFT, {1, 6}},
    {EXT_RIGHT, {1, 7}}, {EXT_LB, {0, 4}}, {EXT_RB, {1, 0}},
    {EXT_LT, {0, 5}},   {EXT_RT, {1, 1}},  {EXT_LS, {0, 6}},
    {EXT_RS, {1, 2}},
};

// ------------------------------------------------------------- HORI NSW-230
// [0..1] buttons LE: 0 Y, 1 B, 2 A, 3 X, 4 L, 5 R, 6 ZL, 7 ZR, 8 minus,
// 9 plus, 10 L3, 11 R3, 12 home, 13 capture; [2] hat; [3..6] sticks.
constexpr uint8_t HORI_BUTTONS[BTN_COUNT] = {
    9,   // fn       <- plus
    0,   // square   <- Y
    2,   // circle   <- A
    3,   // triangle <- X
    1,   // cross    <- B
    12,  // l3       <- home
    13,  // l2       <- capture
    8,   // l1       <- minus
};
struct ExtBit {
  uint16_t ext;
  uint8_t bit;
};
constexpr ExtBit HORI_EXT[] = {{EXT_LB, 4}, {EXT_RB, 5},  {EXT_LT, 6},
                               {EXT_RT, 7}, {EXT_LS, 10}, {EXT_RS, 11}};

// ------------------------------------------------------------- HORI PS4-161
// [0] report ID 0x01; [1..4] sticks; [5] hat + square/cross/circle/triangle;
// [6] L1 R1 L2 R2 share options L3 R3; [7] PS, touchpad click.
constexpr BitPos PS4_BUTTONS[BTN_COUNT] = {
    {6, 5},  // fn       <- options
    {5, 4},  // square
    {5, 6},  // circle
    {5, 7},  // triangle
    {5, 5},  // cross
    {7, 0},  // l3       <- PS
    {7, 1},  // l2       <- touchpad click
    {6, 4},  // l1       <- share
};
constexpr ExtPos PS4_EXT[] = {{EXT_LB, {6, 0}}, {EXT_RB, {6, 1}},
                              {EXT_LT, {6, 2}}, {EXT_RT, {6, 3}},
                              {EXT_LS, {6, 6}}, {EXT_RS, {6, 7}}};

}  // namespace

uint32_t reverse32(uint32_t v) {
  uint32_t r = 0;
  for (int i = 0; i < 32; i++) {
    r = r << 1 | (v & 1);
    v >>= 1;
  }
  return r;
}

const Profile *findProfile(uint16_t vid, uint16_t pid) {
  for (const auto &p : PROFILES)
    if (p.vid == vid && p.pid == pid) return &p;
  return nullptr;
}

const Profile *profileForKind(Kind kind) {
  for (const auto &p : PROFILES)
    if (p.kind == kind) return &p;
  return nullptr;
}

const char *wrongModeHint(uint16_t vid, uint16_t pid) {
  for (const auto &w : WRONG_MODE)
    if (w.vid == vid && w.pid == pid) return w.msg;
  return nullptr;
}

void idlePacket(uint8_t pkt[PACKET_LEN]) {
  memset(pkt, 0, PACKET_LEN);
  memcpy(pkt, HEADER, 3);
}

void buildPacket(uint8_t pkt[PACKET_LEN], uint8_t pressed, uint32_t sensors) {
  idlePacket(pkt);
  for (int b = 0; b < BTN_COUNT; b++)
    if (pressed >> b & 1) pkt[DIVALLER_BITS[b].byte] |= 1 << DIVALLER_BITS[b].bit;
  // TLAC sensor i == bit i of (s5>>4 | s6<<4 | s7<<12 | s8<<20 | s9<<28).
  pkt[5] |= (sensors & 0x0F) << 4;
  pkt[6] = sensors >> 4 & 0xFF;
  pkt[7] = sensors >> 12 & 0xFF;
  pkt[8] = sensors >> 20 & 0xFF;
  pkt[9] = sensors >> 28 & 0x0F;
}

size_t toDivaller(const Profile &p, const Options &o, const uint8_t *d,
                  size_t len, uint8_t pkt[MAX_IN_PACKET], uint16_t &ext) {
  if (len < p.minIn) return 0;
  uint8_t pressed = 0;
  ext = 0;
  switch (p.kind) {
    case Kind::Divaller: {
      size_t n = len < MAX_IN_PACKET ? len : MAX_IN_PACKET;
      memcpy(pkt, d, n);  // already native; the Divaller has no D-pad
      return n;
    }
    case Kind::ZhouSensor: {
      for (int b = 0; b < BTN_COUNT; b++)
        if (bitAt(d, ZHOU_BUTTONS[b])) pressed |= 1 << b;
      for (const auto &e : ZHOU_EXT)
        if (bitAt(d, e.pos)) ext |= e.ext;
      uint32_t mask = (uint32_t)d[3] | (uint32_t)d[4] << 8 |
                      (uint32_t)d[5] << 16 | (uint32_t)d[6] << 24;
      if (o.sliderReverse) mask = reverse32(mask);
      buildPacket(pkt, pressed, mask);
      return PACKET_LEN;
    }
    case Kind::Hori: {
      uint16_t buttons = d[0] | d[1] << 8;
      for (int b = 0; b < BTN_COUNT; b++)
        if (buttons >> HORI_BUTTONS[b] & 1) pressed |= 1 << b;
      ext = hatToDpad(d[2]);
      for (const auto &e : HORI_EXT)
        if (buttons >> e.bit & 1) ext |= e.ext;
      buildPacket(pkt, pressed, sticksToMask(d + 3, o.sliderReverse));
      return PACKET_LEN;
    }
    case Kind::HoriPs4: {
      if (d[0] != 0x01) return 0;  // not an input report
      for (int b = 0; b < BTN_COUNT; b++)
        if (bitAt(d, PS4_BUTTONS[b])) pressed |= 1 << b;
      ext = hatToDpad(d[5]);
      for (const auto &e : PS4_EXT)
        if (bitAt(d, e.pos)) ext |= e.ext;
      buildPacket(pkt, pressed, sticksToMask(d + 1, o.sliderReverse));
      return PACKET_LEN;
    }
    default:
      return 0;
  }
}

size_t ledsToDevice(const Profile &p, const Options &o, const uint8_t *dla,
                    size_t len, uint8_t *out) {
  if (!o.leds) return 0;
  switch (p.kind) {
    case Kind::Divaller: {
      size_t n = len < MAX_DEVICE_OUT ? len : MAX_DEVICE_OUT;
      memcpy(out, dla, n);  // already native
      return n;
    }
    case Kind::ZhouSensor: {
      // TLAC puts sensor n's colour at 4 + 3*(31-n) as G, R, B. The firmware
      // unpacks each 3-byte group i into
      //   LED 2i:   R = b0 high nibble, G = b0 low nibble, B = b1 low nibble
      //   LED 2i+1: G = b1 high nibble, B = b2 high nibble, R = b2 low nibble
      if (len < LED_LEN || memcmp(dla, LED_HEADER, 3) != 0) return 0;
      uint8_t rgb[32][3];
      for (int n = 0; n < 32; n++) {
        int at = 4 + 3 * (31 - n);
        int slot = o.ledReverse ? 31 - n : n;
        rgb[slot][0] = dla[at + 1] >> 4;  // R
        rgb[slot][1] = dla[at] >> 4;      // G
        rgb[slot][2] = dla[at + 2] >> 4;  // B
      }
      for (int i = 0; i < 16; i++) {
        const uint8_t *a = rgb[2 * i], *b = rgb[2 * i + 1];
        out[3 * i] = a[0] << 4 | a[1];
        out[3 * i + 1] = b[1] << 4 | a[2];
        out[3 * i + 2] = b[2] << 4 | b[0];
      }
      return 48;
    }
    default:
      return 0;  // the HORIs light themselves
  }
}

Decoded decode(const uint8_t s[PACKET_LEN]) {
  Decoded r{0, 0};
  for (int b = 0; b < BTN_COUNT; b++)
    if (bitAt(s, DIVALLER_BITS[b])) r.pressed |= 1 << b;
  r.sensors = ((uint32_t)s[5] >> 4 | (uint32_t)s[6] << 4 |
               (uint32_t)s[7] << 12 | (uint32_t)s[8] << 20 |
               (uint32_t)s[9] << 28);
  return r;
}

}  // namespace diva
