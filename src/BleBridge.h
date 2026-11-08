/**
 * @file BleBridge.h
 * @brief Bluetooth LE side: an Xbox-style XInput gamepad that any PC can use,
 * plus the DIVA Bridge GATT service for clients that do the handshake.
 *
 * DIVA Bridge service (all UUIDs 5e1a000X-d1ba-4b1d-9e00-000000d1ba00):
 *   0001 CONTROL  read/write/notify
 *        write "DIVA" + version byte -> this connection goes native: the
 *              gamepad goes quiet and INPUT streams the raw packets
 *        write "BYE"                 -> leave native mode
 *        read/notify -> status, 8 bytes:
 *              [0] protocol version (1)  [1] controller present (0/1)
 *              [2] controller kind (diva::Kind)  [3] native clients
 *              [4..5] VID LE  [6..7] PID LE
 *   0002 INPUT    read/notify   24-byte Divaller packet + 2 bytes extended
 *                               buttons (LE) - the same bytes as an 'X' reply -
 *                               + 2 bytes press counter (LE, wraps). Presses
 *                               between notifications are latched in, never lost.
 *   0003 LEDS     write / write-without-response   100-byte "DLa" packet
 *                               (needs ATT MTU >= 103, or a long write)
 */

#pragma once

#include <Arduino.h>

namespace BleBridge {

// 2: a repeated "DIVA" handshake restarts the stream, and natives get an
//    input notification at least every HEARTBEAT_MS (a liveness signal).
constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint32_t HEARTBEAT_MS = 1000;

#define DIVA_BLE_SERVICE_UUID "5e1a0000-d1ba-4b1d-9e00-000000d1ba00"
#define DIVA_BLE_CONTROL_UUID "5e1a0001-d1ba-4b1d-9e00-000000d1ba00"
#define DIVA_BLE_INPUT_UUID "5e1a0002-d1ba-4b1d-9e00-000000d1ba00"
#define DIVA_BLE_LEDS_UUID "5e1a0003-d1ba-4b1d-9e00-000000d1ba00"

void begin();

/** Connection interval granted to the bridge's link (0 = none). */
float intervalMs();
uint32_t notifySent();
uint32_t notifyFailed();

/** Number of connected BLE centrals. */
int connections();

}  // namespace BleBridge
