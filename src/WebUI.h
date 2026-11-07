/**
 * @file WebUI.h
 * @brief The board's own web page on port 80 (http://<board>/ or
 * http://diva-bridge.local/): live controller tester in the DIVA Bridge GUI's
 * style, status / connection info, settings, and an LED test.
 *
 * Page source: web/index.html, packed into WebPage.h by tools/gen_webpage.py.
 *
 * API (used by the page):
 *   GET  /api/state     live JSON: controller, buttons, sensors, clients, Wi-Fi
 *   GET  /api/settings  JSON of the settings (password left out)
 *   POST /api/settings  "key=value" lines (same keys as the serial "set");
 *                       saves and restarts on success
 *   POST /api/leds      100-byte "DLa" LED packet as hex, as a game sends it
 * The tester only reads, so it doesn't count as a game: the Xbox pad stays on.
 */

#pragma once

namespace WebUI {

/** Start the HTTP server (call once Wi-Fi is set up). */
void begin();

}  // namespace WebUI
