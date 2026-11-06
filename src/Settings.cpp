#include "Settings.h"

#include <Preferences.h>

namespace Settings {

namespace {
Values vals;
Preferences prefs;
String line;
void (*statusPrinter)() = nullptr;
bool dirty = false;

const char *radioName(Radio r) {
  return r == Radio::WifiOnly ? "wifi" : r == Radio::BleOnly ? "ble" : "both";
}
const char *sliderName(SliderMode s) {
  return s == SliderMode::Position ? "position" : s == SliderMode::Off ? "off" : "motion";
}

void load() {
  prefs.begin("divabridge", true);
  vals.ssid = prefs.getString("ssid", "");
  vals.pass = prefs.getString("pass", "");
  vals.name = prefs.getString("name", "DIVA Bridge");
  vals.port = prefs.getUShort("port", 45710);
  vals.radio = (Radio)prefs.getUChar("radio", 0);
  vals.slider = (SliderMode)prefs.getUChar("slider", 0);
  vals.sliderReverse = prefs.getBool("slider_rev", false);
  vals.ledReverse = prefs.getBool("led_rev", false);
  vals.leds = prefs.getBool("leds", true);
  vals.xboxOneS = prefs.getBool("xbox_ones", false);
  vals.otgPower = (OtgPower)prefs.getUChar("otg_power", 0);
  prefs.end();
}

void save() {
  prefs.begin("divabridge", false);
  prefs.putString("ssid", vals.ssid);
  prefs.putString("pass", vals.pass);
  prefs.putString("name", vals.name);
  prefs.putUShort("port", vals.port);
  prefs.putUChar("radio", (uint8_t)vals.radio);
  prefs.putUChar("slider", (uint8_t)vals.slider);
  prefs.putBool("slider_rev", vals.sliderReverse);
  prefs.putBool("led_rev", vals.ledReverse);
  prefs.putBool("leds", vals.leds);
  prefs.putBool("xbox_ones", vals.xboxOneS);
  prefs.putUChar("otg_power", (uint8_t)vals.otgPower);
  prefs.end();
  dirty = false;
}

void printSettings() {
  // One "key=value" per line so the setup page can parse it.
  Serial.println("--- settings ---");
  Serial.printf("ssid=%s\n", vals.ssid.c_str());
  Serial.printf("pass=%s\n", vals.pass.length() ? "(set)" : "");
  Serial.printf("name=%s\n", vals.name.c_str());
  Serial.printf("port=%u\n", vals.port);
  Serial.printf("radio=%s\n", radioName(vals.radio));
  Serial.printf("slider=%s\n", sliderName(vals.slider));
  Serial.printf("slider_reverse=%d\n", vals.sliderReverse);
  Serial.printf("led_reverse=%d\n", vals.ledReverse);
  Serial.printf("leds=%d\n", vals.leds);
  Serial.printf("xbox=%s\n", vals.xboxOneS ? "ones" : "seriesx");
  Serial.printf("otg_power=%s\n", vals.otgPower == OtgPower::Battery ? "battery" : "usb");
  Serial.printf("unsaved=%d\n", dirty);
  Serial.println("--- end ---");
}

void help() {
  Serial.println(
      "Commands (one per line):\n"
      "  status                  controller, Wi-Fi, BLE and native clients\n"
      "  get                     show settings\n"
      "  set ssid <name>         Wi-Fi network (rest of the line, spaces ok)\n"
      "  set pass <password>     Wi-Fi password (rest of the line; empty = open)\n"
      "  set name <text>         Bluetooth name / mDNS host (default DIVA Bridge)\n"
      "  set port <n>            bridge TCP port (default 45710)\n"
      "  set radio both|wifi|ble both radios, or one (wifi = lowest Wi-Fi latency)\n"
      "  set slider motion|position|off   slider on the Xbox fallback pad\n"
      "  set slider_reverse 0|1  flip the touch direction (ZhouSensor/HORI)\n"
      "  set led_reverse 0|1     flip the slider LED order (ZhouSensor)\n"
      "  set leds 0|1            forward LEDs to the controller\n"
      "  set xbox seriesx|ones   XInput profile for the fallback pad\n"
      "  set otg_power usb|battery  ESP32-S3-USB-OTG board: host port power\n"
      "  save                    store settings and restart\n"
      "  forget                  erase Wi-Fi network and restart\n"
      "  unpair                  forget all Bluetooth pairings and restart\n"
      "  reboot");
}

bool parseBool(const String &v, bool &out) {
  if (v == "1" || v == "on" || v == "true" || v == "yes") return out = true, true;
  if (v == "0" || v == "off" || v == "false" || v == "no") return out = false, true;
  return false;
}

void setKey(const String &key, const String &value) {
  bool ok = true;
  if (key == "ssid") vals.ssid = value;
  else if (key == "pass" || key == "password") vals.pass = value;
  else if (key == "name") ok = value.length() > 0 && value.length() <= 28 && (vals.name = value, true);
  else if (key == "port") {
    long p = value.toInt();
    ok = p > 0 && p < 65536;
    if (ok) vals.port = p;
  } else if (key == "radio") {
    if (value == "both") vals.radio = Radio::Both;
    else if (value == "wifi") vals.radio = Radio::WifiOnly;
    else if (value == "ble") vals.radio = Radio::BleOnly;
    else ok = false;
  } else if (key == "slider") {
    if (value == "motion") vals.slider = SliderMode::Motion;
    else if (value == "position") vals.slider = SliderMode::Position;
    else if (value == "off") vals.slider = SliderMode::Off;
    else ok = false;
  } else if (key == "slider_reverse") ok = parseBool(value, vals.sliderReverse);
  else if (key == "led_reverse") ok = parseBool(value, vals.ledReverse);
  else if (key == "leds") ok = parseBool(value, vals.leds);
  else if (key == "xbox") {
    if (value == "seriesx") vals.xboxOneS = false;
    else if (value == "ones") vals.xboxOneS = true;
    else ok = false;
  } else if (key == "otg_power") {
    if (value == "usb") vals.otgPower = OtgPower::UsbDev;
    else if (value == "battery") vals.otgPower = OtgPower::Battery;
    else ok = false;
  } else {
    Serial.printf("ERR unknown setting '%s' (try: help)\n", key.c_str());
    return;
  }
  if (!ok) {
    Serial.printf("ERR bad value for %s\n", key.c_str());
    return;
  }
  dirty = true;
  Serial.printf("OK %s set - 'save' to store and restart\n", key.c_str());
}

void handle(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  int sp = cmd.indexOf(' ');
  String verb = sp < 0 ? cmd : cmd.substring(0, sp);
  String rest = sp < 0 ? "" : cmd.substring(sp + 1);
  verb.toLowerCase();

  if (verb == "help" || verb == "?") help();
  else if (verb == "status") { if (statusPrinter) statusPrinter(); }
  else if (verb == "get") printSettings();
  else if (verb == "set") {
    rest.trim();
    int sp2 = rest.indexOf(' ');
    String key = sp2 < 0 ? rest : rest.substring(0, sp2);
    String value = sp2 < 0 ? "" : rest.substring(sp2 + 1);
    key.toLowerCase();
    if (key != "ssid" && key != "pass" && key != "password" && key != "name") value.trim();
    setKey(key, value);
  } else if (verb == "save") {
    save();
    Serial.println("OK saved - restarting");
    delay(200);
    ESP.restart();
  } else if (verb == "forget") {
    vals.ssid = "";
    vals.pass = "";
    save();
    Serial.println("OK Wi-Fi forgotten - restarting");
    delay(200);
    ESP.restart();
  } else if (verb == "unpair") {
    prefs.begin("divabridge", false);
    prefs.putBool("unpair", true);  // done at the next boot, before BLE starts
    prefs.end();
    Serial.println("OK pairings will be cleared - restarting");
    delay(200);
    ESP.restart();
  } else if (verb == "reboot" || verb == "restart") {
    ESP.restart();
  } else {
    Serial.printf("ERR unknown command '%s' (try: help)\n", verb.c_str());
  }
}
}  // namespace

void begin() { load(); }

Values &get() { return vals; }

void poll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (line.length()) handle(line);
      line = "";
    } else if (line.length() < 200) {
      line += c;
    }
  }
}

void setStatusPrinter(void (*fn)()) { statusPrinter = fn; }

bool consumeUnpair() {
  prefs.begin("divabridge", false);
  bool f = prefs.getBool("unpair", false);
  if (f) prefs.remove("unpair");
  prefs.end();
  return f;
}

}  // namespace Settings
