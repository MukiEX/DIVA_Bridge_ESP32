#include "Settings.h"

#include <Preferences.h>
#include <esp_partition.h>
#include <esp_random.h>

#include "ConfigBlob.h"
#include "Version.h"

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

void mirrorToPartition();

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
  mirrorToPartition();
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

bool setKey(const String &key, const String &value, bool quiet = false) {
  bool ok = true;
  if (key == "ssid") vals.ssid = value;
  else if (key == "pass" || key == "password") vals.pass = value;
  else if (key == "name") ok = value.length() > 0 && value.length() <= 28 && (vals.name = value, true);
  else if (key == "port") {
    long p = value.toInt();
    ok = p > 0 && p < 65536 && p != 80;  // 80 is the web page
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
    Serial.printf("ERR unknown setting '%s'%s\n", key.c_str(), quiet ? "" : " (try: help)");
    return false;
  }
  if (!ok) {
    Serial.printf("ERR bad value for %s\n", key.c_str());
    return false;
  }
  dirty = true;
  if (!quiet) Serial.printf("OK %s set - 'save' to store and restart\n", key.c_str());
  return true;
}

// ------------------------------------------------- flasher settings blob
const esp_partition_t *cfgPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                  "divacfg");
}

/** Current settings as "key=value" lines (the blob payload). */
String settingsLines() {
  String s;
  s += "ssid=" + vals.ssid + "\n";
  s += "pass=" + vals.pass + "\n";
  s += "name=" + vals.name + "\n";
  s += "port=" + String(vals.port) + "\n";
  s += String("radio=") + radioName(vals.radio) + "\n";
  s += String("slider=") + sliderName(vals.slider) + "\n";
  s += "slider_reverse=" + String(vals.sliderReverse ? 1 : 0) + "\n";
  s += "led_reverse=" + String(vals.ledReverse ? 1 : 0) + "\n";
  s += "leds=" + String(vals.leds ? 1 : 0) + "\n";
  s += String("xbox=") + (vals.xboxOneS ? "ones" : "seriesx") + "\n";
  s += String("otg_power=") + (vals.otgPower == OtgPower::Battery ? "battery" : "usb") + "\n";
  s += "fw=" DIVA_BRIDGE_VERSION "\n";  // read-only: lets the flasher show the board's version
  return s;
}

/** Keep the partition in step with NVS, so the flasher can read what's
 * really in use (also after changes made over serial). Same serial as the
 * last applied blob, so it isn't applied again at the next boot. */
void mirrorToPartition() {
  const esp_partition_t *part = cfgPartition();
  if (!part) return;
  prefs.begin("divabridge", false);
  uint32_t serial = prefs.getULong("cfg_serial", 0);
  if (!serial) {
    serial = esp_random() | 1;
    prefs.putULong("cfg_serial", serial);
  }
  prefs.end();
  String lines = settingsLines();
  uint8_t *blob = (uint8_t *)malloc(ConfigBlob::PARTITION_SIZE);
  if (!blob) return;
  if (ConfigBlob::encode(lines.c_str(), lines.length(), serial, blob) &&
      esp_partition_erase_range(part, 0, ConfigBlob::PARTITION_SIZE) == ESP_OK)
    esp_partition_write(part, 0, blob, ConfigBlob::PARTITION_SIZE);
  free(blob);
}

/** Apply "key=value" lines (flasher blob / web page). "unpair=1" queues a
 * pairing reset for the next boot. Applies every valid line; false + error
 * names the first bad one. */
bool applyText(const String &text, String &error) {
  int start = 0;
  bool allOk = true;
  while (start < (int)text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = text.length();
    String l = text.substring(start, end);
    start = end + 1;
    if (l.endsWith("\r")) l.remove(l.length() - 1);
    int eq = l.indexOf('=');
    if (eq <= 0) continue;
    String key = l.substring(0, eq), value = l.substring(eq + 1);
    key.trim();
    key.toLowerCase();
    if (key == "fw") continue;  // informational
    if (key == "unpair") {
      if (value == "1") {
        prefs.begin("divabridge", false);
        prefs.putBool("unpair", true);
        prefs.end();
      }
      continue;
    }
    if (key != "ssid" && key != "pass" && key != "password" && key != "name") value.trim();
    if (!setKey(key, value, true) && allOk) {
      allOk = false;
      error = "bad value for " + key;
    }
  }
  return allOk;
}

/** Apply a blob the flasher wrote, once. */
void applyFromPartition() {
  const esp_partition_t *part = cfgPartition();
  if (!part) {
    Serial.println("[Settings] no divacfg partition (old partition table?) - serial setup only");
    return;
  }
  uint8_t *blob = (uint8_t *)malloc(ConfigBlob::PARTITION_SIZE);
  if (!blob) return;
  const char *payload;
  size_t len;
  uint32_t serial;
  bool valid = esp_partition_read(part, 0, blob, ConfigBlob::PARTITION_SIZE) == ESP_OK &&
               ConfigBlob::decode(blob, ConfigBlob::PARTITION_SIZE, payload, len, serial);
  prefs.begin("divabridge", true);
  uint32_t applied = prefs.getULong("cfg_serial", 0);
  prefs.end();
  if (!valid) {
    free(blob);
    mirrorToPartition();  // erased or damaged: write the current settings
    return;
  }
  if (serial == applied) {
    // Same settings; refresh the copy if it came from another firmware version.
    String text(payload, len);
    if (text.indexOf("fw=" DIVA_BRIDGE_VERSION "\n") < 0) {
      free(blob);
      mirrorToPartition();
      return;
    }
  }
  if (serial != applied) {
    Serial.println("[Settings] applying settings from the web flasher");
    String error;
    applyText(String(payload, len), error);
    prefs.begin("divabridge", false);
    prefs.putULong("cfg_serial", serial);
    prefs.end();
    save();  // stores NVS and rewrites the blob (normalised, without unpair)
  }
  free(blob);
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

uint32_t restartAt = 0;

void begin() {
  load();
  applyFromPartition();
}

Values &get() { return vals; }

void poll() {
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
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

namespace Settings {

String toLines(bool withPassword) {
  String s = settingsLines();
  if (!withPassword) {
    int i = s.indexOf("pass="), e = s.indexOf('\n', i);
    s = s.substring(0, i) + "pass=" + s.substring(e);
  }
  return s;
}

bool passwordSet() { return vals.pass.length() > 0; }

bool applyAndSave(const String &text, String &error) {
  Values backup = vals;
  if (!applyText(text, error)) {
    vals = backup;  // all or nothing for the web page
    return false;
  }
  save();
  return true;
}

void restartSoon(uint32_t ms) { restartAt = millis() + ms; if (!restartAt) restartAt = 1; }

}  // namespace Settings
