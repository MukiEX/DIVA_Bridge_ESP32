/**
 * @file Settings.h
 * @brief Persistent settings (NVS) and the serial console used to change
 * them over the board's USB-UART port (the OTG port is busy being a host).
 *
 * Commands are plain text lines, one per line, so any serial terminal works
 * (115200 baud), as does tools/DIVA_Bridge_ESP32_Setup.html in Chrome/Edge.
 */

#pragma once

#include <Arduino.h>

namespace Settings {

enum class Radio : uint8_t { Both = 0, WifiOnly = 1, BleOnly = 2 };
enum class SliderMode : uint8_t { Motion = 0, Position = 1, Off = 2 };
enum class OtgPower : uint8_t { UsbDev = 0, Battery = 1 };

struct Values {
  String ssid;
  String pass;
  String name = "DIVA Bridge";    // BLE name and mDNS host name base
  uint16_t port = 45710;          // TCP port (same default as diva_bridge.py)
  Radio radio = Radio::Both;
  SliderMode slider = SliderMode::Motion;
  bool sliderReverse = false;
  bool ledReverse = false;
  bool leds = true;
  bool xboxOneS = false;          // XInput profile: Series X (default) or One S
  OtgPower otgPower = OtgPower::UsbDev;  // ESP32-S3-USB-OTG board only
};

/** Load from NVS (defaults where nothing is stored). */
void begin();
Values &get();

/** true once after 'unpair' was requested (clears the flag). */
bool consumeUnpair();

/** Feed serial input; handles complete lines. Call from loop(). */
void poll();

/** Called by the console to print live status (set up in main). */
void setStatusPrinter(void (*fn)());

}  // namespace Settings
