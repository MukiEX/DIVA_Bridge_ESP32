/**
 * @file WatchDisplay.h
 * @brief Screen, touch and battery for the Waveshare ESP32-S3-Touch-AMOLED-2.06
 * (watch) build: a status + live tester screen over an optional background
 * image the setup page writes into the "bgimage" partition.
 *
 * Hardware (from Waveshare's board_config/pin_config.h):
 *   CO5300 AMOLED 410x502, QSPI: CS 12, SCK 11, D0-D3 4/5/6/7, RST 8
 *   FT3168 touch, I2C SDA 15 / SCL 14 (addr 0x38), INT 38, RST 9
 *   AXP2101 PMU on the same I2C bus (addr 0x34): battery gauge
 *   The only USB-C port is the S3's native USB (no UART), so this build has
 *   no serial console; setup is the web flasher (hold BOOT while plugging in)
 *   and the board's own web page.
 */

#pragma once

#ifdef BOARD_WS_AMOLED_206

#include <stdint.h>

namespace WatchDisplay {

constexpr int WIDTH = 410, HEIGHT = 502;

/** Bring up the screen, touch and battery gauge and start the UI task. */
void begin();

/** Re-read the "bgimage" partition (after the web page wrote a new one). */
void reloadBackground();

/** Battery percent (0-100, -1 unknown) and whether it's charging. */
int batteryPercent();
bool charging();

}  // namespace WatchDisplay

#endif
