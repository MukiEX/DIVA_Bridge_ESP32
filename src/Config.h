/**
 * @file Config.h
 * @brief Build-time board options. Runtime settings (Wi-Fi, names, slider
 * mode...) live in NVS and are changed over serial - see Settings.h.
 */

#pragma once

/**
 * Status LED: lit while a controller is attached, blinks while a native
 * client (game) is connected. -1 to disable.
 * Generic ESP32-S3 DevKitC boards have an RGB LED that needs a driver, so
 * this is off unless a board below sets it.
 */
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN -1
#endif

/**
 * Espressif ESP32-S3-USB-OTG board (build env "esp32s3_usb_otg"):
 * its USB-A host port needs the USB switch flipped and its power enabled.
 */
#ifdef BOARD_ESP32_S3_USB_OTG
#define OTG_USB_SEL_PIN 18      // high = USB_HOST port
#define OTG_BOOST_EN_PIN 13     // high = host port powered from the battery
#define OTG_DEV_VBUS_EN_PIN 12  // high = host port powered from the USB_DEV plug
#define OTG_LIMIT_EN_PIN 17     // high = current limiter on (port output enabled)
#define OTG_LED_GREEN_PIN 15
#undef STATUS_LED_PIN
#define STATUS_LED_PIN OTG_LED_GREEN_PIN
#endif

/**
 * Waveshare ESP32-S3-Touch-AMOLED-2.06 (watch, build env "esp32s3_amoled_206"):
 * 410x502 AMOLED status/tester screen (src/boards/WatchDisplay), battery
 * gauge, one USB-C port (native USB = the controller port; no serial).
 */
