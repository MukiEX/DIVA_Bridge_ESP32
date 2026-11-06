/**
 * @file NetProtocol.h
 * @brief One DIVA Bridge TCP client session - the protocol diva_bridge.py
 * speaks, so DivallerBridge.dva and BridgeSlider work unchanged.
 * Kept apart from the Wi-Fi code so it can be tested on a PC (test/).
 */

#pragma once

namespace NetProtocol {

/** Serve requests on a connected socket until the client goes away. The
 * first request counts as the handshake (native mode on until it leaves). */
void serve(int sock, const char *who);

}  // namespace NetProtocol
