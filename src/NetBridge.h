/**
 * @file NetBridge.h
 * @brief Wi-Fi side: joins your network and serves the exact DIVA Bridge TCP
 * protocol that diva_bridge.py serves, so DivallerBridge.dva (PD-Loader, also
 * the Quest recomp) and BridgeSlider (Mega Mix+) connect to it unchanged -
 * just set their host to this board.
 *
 * Protocol (requests start with one opcode byte):
 *   'P'                         -> status(1)                   device present?
 *   'R' ep(1) max(1)            -> status(1) len(1) data[len]  latest IN packet
 *   'W' ep(1) len(1) data[len]  -> status(1)                   queue OUT packet
 *   'X' ep(1) max(1)            -> status(1) len(1) data[len]  packet + 2 bytes
 *                                  extended buttons (LE)
 * A client's first request is its handshake: from then until it disconnects
 * the BLE gamepad goes quiet so the game doesn't see the controller twice.
 */

#pragma once

#include <Arduino.h>

namespace NetBridge {

void begin();

/** "connected 192.168.1.50", "connecting to <ssid>", "off", ... */
String describe();

/** Connected TCP clients. */
int clients();

}  // namespace NetBridge
