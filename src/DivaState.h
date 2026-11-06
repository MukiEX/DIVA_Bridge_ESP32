/**
 * @file DivaState.h
 * @brief The one shared controller state: latest input, pending LEDs, and
 * which native ("handshaken") clients are attached.
 *
 * The USB side writes input and reads LEDs; the network and BLE sides read
 * input and write LEDs. Everything is guarded by a short critical section so
 * nobody ever waits on USB, Wi-Fi or Bluetooth while holding it.
 */

#pragma once

#include <Arduino.h>
#include "DivaProtocol.h"

namespace DivaState {

void begin();

/** Options used by the translators (reverse flags, LEDs on/off). */
diva::Options &options();

// ---------------------------------------------------------------- USB side
/** Controller attached (or nullptr when it goes away). */
void setController(const diva::Profile *profile, uint16_t vid, uint16_t pid);
void setInput(const uint8_t *pkt, size_t len, uint16_t ext);

/**
 * Take the newest LED packet, if one arrived since the last call.
 * @return length (0 = nothing new).
 */
size_t takeLeds(uint8_t *out, size_t max);

/** Task to wake (xTaskNotifyGive) when LEDs arrive. */
void setLedListener(TaskHandle_t task);

// ------------------------------------------------------ network / BLE side
const diva::Profile *controller();
void controllerIds(uint16_t &vid, uint16_t &pid);
bool present();

/** 'R': latest packet (idle packet before the first report). false = no
 * controller. */
bool readPacket(uint8_t *out, size_t max, size_t &len);

/** 'X': 24-byte packet + 2 bytes of extended buttons (LE). false = none. */
bool readExt(uint8_t out[diva::EXT_PACKET_LEN]);

/** 'W': queue an LED packet. false = no controller (as the PC bridge). */
bool queueLeds(const uint8_t *data, size_t len);

/** Counts input reports; changes whenever new input arrives. */
uint32_t inputSeq();

/** Task to wake (xTaskNotifyGive) when input or controller changes. */
void setInputListener(TaskHandle_t task);

// ------------------------------------------------------------ native mode
/** A client completed the handshake; the gamepad goes quiet while any are. */
void nativeAcquire(const char *who);
void nativeRelease(const char *who);
int nativeClients();

}  // namespace DivaState
