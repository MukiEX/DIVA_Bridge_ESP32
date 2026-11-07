/**
 * DIVA Bridge ESP32 - a Project DIVA slider controller over Bluetooth/Wi-Fi.
 *
 * Plug a Divaller, ZhouSensor (DIVA Pro, HDD mode) or HORI NSW-230 / PS4-161
 * (arcade mode) into the ESP32-S3's USB OTG port (directly or through a
 * powered hub). Then:
 *   - any PC sees a Bluetooth Xbox controller (buttons, D-pad, slider on the
 *     sticks), and
 *   - games using DIVA Bridge (DivallerBridge.dva for PD-Loader, also the
 *     Quest recomp; BridgeSlider for Mega Mix+) get the real slider and LEDs,
 *     over Wi-Fi (same TCP protocol as diva_bridge.py) or over BLE via
 *     diva_bridge.py --device ble. While one is connected the Xbox pad goes
 *     quiet.
 *
 * Fork of KoStard/ESP32S3-USB-Keyboard-To-BLE.
 */

#include <Arduino.h>

#include "BleBridge.h"
#include "Config.h"
#include "ControllerHost.h"
#include "DivaState.h"
#include "NetBridge.h"
#include "Settings.h"
#include "Version.h"

static void printStatus() {
  Serial.println("--- status ---");
  Serial.println("version=" DIVA_BRIDGE_VERSION);
  Serial.printf("controller=%s\n", ControllerHost::describe().c_str());
  Serial.printf("reports=%lu\n", (unsigned long)ControllerHost::reportCount());
  Serial.printf("wifi=%s\n", NetBridge::describe().c_str());
  Serial.printf("tcp_clients=%d\n", NetBridge::clients());
  Serial.printf("ble=%s\n", Settings::get().radio == Settings::Radio::WifiOnly
                                ? "off (radio=wifi)"
                                : (String(BleBridge::connections()) + " connected").c_str());
  Serial.printf("native_clients=%d\n", DivaState::nativeClients());
  Serial.printf("free_heap=%lu\n", (unsigned long)ESP.getFreeHeap());
  Serial.printf("usb_port=%s\n", ControllerHost::portStatus().c_str());
  Serial.printf("reset_reason=%d\n", (int)esp_reset_reason());
  Serial.printf("usb=%s\n", ControllerHost::usbTreeJson().c_str());
  Serial.println("--- end ---");
}

static void boardSetup() {
#ifdef BOARD_ESP32_S3_USB_OTG
  bool battery = Settings::get().otgPower == Settings::OtgPower::Battery;
  pinMode(OTG_USB_SEL_PIN, OUTPUT);
  pinMode(OTG_BOOST_EN_PIN, OUTPUT);
  pinMode(OTG_DEV_VBUS_EN_PIN, OUTPUT);
  pinMode(OTG_LIMIT_EN_PIN, OUTPUT);
  digitalWrite(OTG_BOOST_EN_PIN, battery ? HIGH : LOW);
  digitalWrite(OTG_DEV_VBUS_EN_PIN, battery ? LOW : HIGH);
  digitalWrite(OTG_LIMIT_EN_PIN, HIGH);
  digitalWrite(OTG_USB_SEL_PIN, HIGH);  // route the PHY to the USB-A host port
  Serial.printf("[Board] ESP32-S3-USB-OTG: host port powered from %s\n",
                battery ? "the battery" : "the USB_DEV plug");
#endif
  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== DIVA Bridge ESP32 v" DIVA_BRIDGE_VERSION " ===  (type 'help' for commands)");

  Settings::begin();
  auto &s = Settings::get();
  auto &o = DivaState::options();
  o.sliderReverse = s.sliderReverse;
  o.ledReverse = s.ledReverse;
  o.leds = s.leds;
  Settings::setStatusPrinter(printStatus);

  boardSetup();
  DivaState::begin();
  ControllerHost::begin();
  if (s.radio != Settings::Radio::WifiOnly) BleBridge::begin();
  NetBridge::begin();
  Serial.println("Ready.");
}

void loop() {
  Settings::poll();

  if (STATUS_LED_PIN >= 0) {
    bool on = DivaState::present();
    if (on && DivaState::nativeClients() > 0) on = (millis() / 250) & 1;
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
  delay(10);
}
