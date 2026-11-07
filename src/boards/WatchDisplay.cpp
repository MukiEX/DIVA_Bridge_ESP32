#include "WatchDisplay.h"

#ifdef BOARD_WS_AMOLED_206

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_partition.h>

#include "../BleBridge.h"
#include "../ControllerHost.h"
#include "../DivaState.h"
#include "../NetBridge.h"
#include "../Settings.h"
#include "../Version.h"
#include <U8g2lib.h>  // fonts (Arduino_GFX picks up u8g2 font support from it)

using namespace diva;

namespace WatchDisplay {

namespace {

// ------------------------------------------------------------- pins
constexpr int LCD_SDIO0 = 4, LCD_SDIO1 = 5, LCD_SDIO2 = 6, LCD_SDIO3 = 7;
constexpr int LCD_SCLK = 11, LCD_CS = 12, LCD_RESET = 8;
constexpr int I2C_SDA = 15, I2C_SCL = 14, TP_RESET = 9;
constexpr uint8_t FT3168_ADDR = 0x38, AXP2101_ADDR = 0x34;

/** Canvas whose framebuffer we allocate ourselves, in PSRAM. */
class PsCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  void useFramebuffer(uint16_t *fb) { _framebuffer = fb; }
};

Arduino_DataBus *bus = nullptr;
Arduino_CO5300 *panel = nullptr;
PsCanvas *gfx = nullptr;
uint16_t *background = nullptr;  // WIDTH*HEIGHT RGB565, PSRAM

volatile int battery = -1;
volatile bool isCharging = false;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
}
constexpr uint16_t C_TEXT = rgb(0xEC, 0xEC, 0xF0), C_DIM = rgb(0x9A, 0x9C, 0xA8);
constexpr uint16_t C_CYAN = rgb(0x3F, 0xD7, 0xEA), C_GREEN = rgb(0x4F, 0xD0, 0x6A);
constexpr uint16_t C_AMBER = rgb(0xF2, 0xC8, 0x4B), C_RED = rgb(0xF0, 0x50, 0x50);
constexpr uint16_t C_PINK = rgb(0xE4, 0x5D, 0xD0), C_BLUE = rgb(0x5A, 0x8C, 0xFF);
constexpr uint16_t C_OFF = rgb(0x2A, 0x2C, 0x34);

// ------------------------------------------------------------- background
// "bgimage" partition (partitions.csv, written by the setup page):
//   "DVBG" u16 width u16 height u32 byte length, then RGB565 little-endian.
volatile bool redrawAll = false;

void loadBackground() {
  if (!background) background = (uint16_t *)heap_caps_malloc(WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!background) return;
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bgimage");
  uint8_t head[12];
  bool ok = part && esp_partition_read(part, 0, head, sizeof(head)) == ESP_OK &&
            memcmp(head, "DVBG", 4) == 0 && (head[4] | head[5] << 8) == WIDTH &&
            (head[6] | head[7] << 8) == HEIGHT &&
            (uint32_t)(head[8] | head[9] << 8 | head[10] << 16 | head[11] << 24) == WIDTH * HEIGHT * 2u;
  if (ok) ok = esp_partition_read(part, 12, background, WIDTH * HEIGHT * 2) == ESP_OK;
  if (ok) {
    Serial.println("[Watch] background image loaded");
    return;
  }
  // Default: a dark teal-to-navy gradient with a soft glow.
  for (int y = 0; y < HEIGHT; y++)
    for (int x = 0; x < WIDTH; x++) {
      float t = (float)y / HEIGHT;
      float dx = (x - WIDTH * 0.75f) / WIDTH, dy = (y - HEIGHT * 0.15f) / HEIGHT;
      float glow = expf(-(dx * dx + dy * dy) * 6.0f);
      uint8_t r = 8 + 10 * t + 20 * glow, g = 40 - 22 * t + 90 * glow, b = 52 - 10 * t + 100 * glow;
      background[y * WIDTH + x] = rgb(r, g, b);
    }
}

// ------------------------------------------------------------- drawing helpers
/** Darken a rectangle of the framebuffer to 25% so text over any picture reads. */
void shade(int x0, int y0, int w, int h) {
  uint16_t *fb = gfx->getFramebuffer();
  for (int y = max(0, y0); y < min(HEIGHT, y0 + h); y++) {
    uint16_t *row = fb + y * WIDTH;
    for (int x = max(0, x0); x < min(WIDTH, x0 + w); x++) row[x] = (row[x] >> 2) & 0x39E7;
  }
}

void text(int x, int y, const char *s, uint16_t color, const uint8_t *font, int align = 0) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  int16_t bx, by;
  uint16_t bw, bh;
  if (align) {
    gfx->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    x -= align > 0 ? bw : bw / 2;  // 1 = right, -1 = centre... see callers
  }
  gfx->setCursor(x, y);
  gfx->print(s);
}

void symbol(int key, int cx, int cy, int r, uint16_t color) {
  // key: 0 triangle, 1 square, 2 cross, 3 circle; drawn 3 px thick
  for (int t = -1; t <= 1; t++) {
    switch (key) {
      case 0:
        gfx->drawTriangle(cx, cy - r + t, cx - r + t, cy + r * 3 / 4, cx + r - t, cy + r * 3 / 4, color);
        break;
      case 1:
        gfx->drawRect(cx - r * 3 / 4 + t, cy - r * 3 / 4 + t, r * 3 / 2 - 2 * t, r * 3 / 2 - 2 * t, color);
        break;
      case 2:
        gfx->drawLine(cx - r * 3 / 4 + t, cy - r * 3 / 4, cx + r * 3 / 4 + t, cy + r * 3 / 4, color);
        gfx->drawLine(cx - r * 3 / 4 + t, cy + r * 3 / 4, cx + r * 3 / 4 + t, cy - r * 3 / 4, color);
        break;
      default:
        gfx->drawCircle(cx, cy, r * 3 / 4 + t, color);
    }
  }
}

void pill(int x, int y, const char *label, bool on, uint16_t color) {
  int w = 44;
  gfx->fillRoundRect(x, y, w, 24, 12, on ? color : C_OFF);
  text(x + w / 2, y + 17, label, on ? 0x0000 : C_DIM, u8g2_font_helvB10_tr, -1);
}

// ------------------------------------------------------------- hardware bits
bool i2cRead(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
void i2cWrite(uint8_t addr, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(v);
  Wire.endTransmission();
}

bool touched() {
  uint8_t n;
  return i2cRead(FT3168_ADDR, 0x02, &n, 1) && (n & 0x0F) > 0 && (n & 0x0F) < 3;
}

void readBattery() {
  uint8_t st1, st2, pct;
  if (!i2cRead(AXP2101_ADDR, 0x00, &st1, 1) || !i2cRead(AXP2101_ADDR, 0x01, &st2, 1) ||
      !i2cRead(AXP2101_ADDR, 0xA4, &pct, 1))
    return;
  bool batPresent = st1 & 0x08;
  battery = batPresent && pct <= 100 ? pct : -1;
  isCharging = ((st2 >> 5) & 0x03) == 0x01;
}

// ------------------------------------------------------------- the screen
struct Snapshot {
  bool present;
  char name[24];
  uint8_t pkt[EXT_PACKET_LEN];
  int native, tcp, ble;
  bool wifiUp;
  char ip[16];
  int battery;
  bool charging;
  int rate;
};

void draw(const Snapshot &s) {
  memcpy(gfx->getFramebuffer(), background, WIDTH * HEIGHT * 2);
  auto &cfg = Settings::get();

  // Title bar
  shade(0, 0, WIDTH, 64);
  text(20, 42, "DIVA Bridge", C_TEXT, u8g2_font_helvB24_tr);
  char bat[16];
  if (s.battery >= 0) snprintf(bat, sizeof(bat), "%s%d%%", s.charging ? "+" : "", s.battery);
  else snprintf(bat, sizeof(bat), "USB");
  text(WIDTH - 20, 40, bat, s.battery >= 0 && s.battery < 15 && !s.charging ? C_RED : C_DIM,
       u8g2_font_helvB14_tr, 1);

  // Controller card
  shade(14, 78, WIDTH - 28, 196);
  if (!s.present) {
    text(WIDTH / 2, 160, "No controller", C_TEXT, u8g2_font_helvB14_tr, -1);
    text(WIDTH / 2, 188, "Plug one into the USB-C port", C_DIM, u8g2_font_helvR10_tr, -1);
    text(WIDTH / 2, 208, "(through a powered hub)", C_DIM, u8g2_font_helvR10_tr, -1);
  } else {
    Decoded d = decode(s.pkt);
    uint16_t ext = s.pkt[24] | s.pkt[25] << 8;
    text(28, 108, s.name, C_TEXT, u8g2_font_helvB14_tr);
    // FN / L1 / L2 / L3
    const char *small[4] = {"L1", "L2", "L3", "FN"};
    const int bits[4] = {BTN_L1, BTN_L2, BTN_L3, BTN_FN};
    for (int i = 0; i < 4; i++)
      pill(WIDTH - 28 - (4 - i) * 50, 90, small[i], d.pressed >> bits[i] & 1, i == 3 ? C_AMBER : C_PINK);
    // Slider
    const int sx = 28, sy = 128, sw = WIDTH - 56, sh = 34;
    gfx->drawRoundRect(sx - 4, sy - 4, sw + 8, sh + 8, 8, rgb(0x2B, 0x6E, 0x78));
    for (int i = 0; i < 32; i++) {
      int x0 = sx + i * sw / 32, x1 = sx + (i + 1) * sw / 32;
      bool on = d.sensors >> i & 1;
      gfx->fillRect(x0 + 1, sy, x1 - x0 - 2, sh, on ? C_CYAN : (i % 8 ? rgb(0x1E, 0x2A, 0x30) : rgb(0x26, 0x36, 0x3D)));
    }
    // Face buttons
    const int keys[4] = {BTN_TRIANGLE, BTN_SQUARE, BTN_CROSS, BTN_CIRCLE};
    const uint16_t colors[4] = {C_GREEN, C_PINK, C_BLUE, C_RED};
    for (int i = 0; i < 4; i++) {
      int cx = 28 + 44 + i * ((WIDTH - 56 - 88) / 3), cy = 220, r = 36;
      bool on = d.pressed >> keys[i] & 1;
      if (on) gfx->fillCircle(cx, cy, r, colors[i]);
      for (int t = 0; t < 3; t++) gfx->drawCircle(cx, cy, r - t, colors[i]);
      symbol(i, cx, cy, 18, on ? 0xFFFF : colors[i]);
    }
    if (ext) {
      char e[48] = "";
      const char *names[] = {"Up", "Dn", "Lt", "Rt", "LB", "RB", "LT", "RT", "LS", "RS"};
      for (int i = 0; i < 10; i++)
        if (ext >> i & 1) {
          strcat(e, names[i]);
          strcat(e, " ");
        }
      text(WIDTH / 2, 268, e, C_AMBER, u8g2_font_helvB10_tr, -1);
    }
  }

  // Status card
  shade(14, 288, WIDTH - 28, 182);
  char line[64];
  int y = 318;
  auto row = [&](const char *label, const char *value, uint16_t color) {
    text(28, y, label, C_DIM, u8g2_font_helvR12_tr);
    text(132, y, value, color, u8g2_font_helvB14_tr);
    y += 34;
  };
  if (cfg.radio == Settings::Radio::BleOnly) row("Wi-Fi", "off", C_DIM);
  else if (s.wifiUp) row("Wi-Fi", s.ip, C_TEXT);
  else row("Wi-Fi", cfg.ssid.length() ? "connecting..." : "not set up", C_AMBER);
  snprintf(line, sizeof(line), "%s.local:%u", NetBridge::hostName().c_str(), cfg.port);
  if (s.wifiUp) {
    text(132, y - 8, line, C_DIM, u8g2_font_helvR10_tr);
    y += 14;
  }
  if (s.native) snprintf(line, sizeof(line), "%d connected (%d Wi-Fi)", s.native, s.tcp);
  else snprintf(line, sizeof(line), "none");
  row("Games", line, s.native ? C_GREEN : C_DIM);
  const char *pad = cfg.radio == Settings::Radio::WifiOnly ? "off"
                    : !s.ble                               ? "not paired"
                    : s.native                             ? "muted (game)"
                                                           : "connected";
  row("Xbox pad", pad, cfg.radio != Settings::Radio::WifiOnly && s.ble && !s.native ? C_GREEN : C_DIM);
  snprintf(line, sizeof(line), "%d reports/s", s.rate);
  row("USB", s.present ? line : "-", C_DIM);

  // Footer
  snprintf(line, sizeof(line), "v%s  -  hold BOOT while plugging in to flash", DIVA_BRIDGE_VERSION);
  shade(0, HEIGHT - 26, WIDTH, 26);
  text(WIDTH / 2, HEIGHT - 8, line, C_DIM, u8g2_font_helvR10_tr, -1);

  gfx->flush();
}

void uiTask(void *) {
  Snapshot cur = {}, last = {};
  uint32_t lastDraw = 0, lastActivity = millis(), lastBattery = 0, lastRate = 0, lastReports = 0;
  int rate = 0;
  bool dimmed = false, firstDraw = true;
  auto &cfg = Settings::get();
  while (true) {
    uint32_t now = millis();
    if (now - lastBattery > 5000) {
      lastBattery = now;
      readBattery();
    }
    if (now - lastRate >= 1000) {
      uint32_t r = ControllerHost::reportCount();
      rate = (r - lastReports) * 1000 / max<uint32_t>(1, now - lastRate);
      lastReports = r;
      lastRate = now;
    }
    const Profile *p = DivaState::controller();
    cur.present = DivaState::readExt(cur.pkt);
    snprintf(cur.name, sizeof(cur.name), "%s", p ? p->name : "");
    cur.native = DivaState::nativeClients();
    cur.tcp = NetBridge::clients();
    cur.ble = cfg.radio == Settings::Radio::WifiOnly ? 0 : BleBridge::connections();
    cur.wifiUp = WiFi.status() == WL_CONNECTED;
    snprintf(cur.ip, sizeof(cur.ip), "%s", WiFi.localIP().toString().c_str());
    cur.battery = battery;
    cur.charging = isCharging;
    cur.rate = rate;

    bool inputChanged = memcmp(cur.pkt, last.pkt, sizeof(cur.pkt)) != 0 || cur.present != last.present;
    bool touch = touched();
    if (inputChanged || touch) lastActivity = now;
    uint32_t timeout = cfg.screenTimeout * 1000u;
    if (dimmed && now - lastActivity < 200) {
      panel->setBrightness(cfg.brightness);
      dimmed = false;
    } else if (!dimmed && timeout && now - lastActivity > timeout) {
      panel->setBrightness(max(4, cfg.brightness / 16));  // dim, not off: AMOLED, keep it gentle
      dimmed = true;
    }
    bool changed = firstDraw || inputChanged || memcmp(&cur, &last, sizeof(cur)) != 0;
    if (redrawAll) {
      redrawAll = false;
      changed = true;
    }
    if ((changed && now - lastDraw >= 33) || now - lastDraw >= 1000) {
      draw(cur);
      last = cur;
      lastDraw = now;
      firstDraw = false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

}  // namespace

void reloadBackground() {
  if (!background) return;
  loadBackground();
  redrawAll = true;
}

int batteryPercent() { return battery; }
bool charging() { return isCharging; }

void begin() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(10);
  digitalWrite(TP_RESET, HIGH);
  // AXP2101: make sure the fuel gauge is on (reg 0x18 bit 3).
  uint8_t v;
  if (i2cRead(AXP2101_ADDR, 0x18, &v, 1)) i2cWrite(AXP2101_ADDR, 0x18, v | 0x08);
  readBattery();

  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  panel = new Arduino_CO5300(bus, LCD_RESET, 0, WIDTH, HEIGHT, 22, 0, 0, 0);
  gfx = new PsCanvas(WIDTH, HEIGHT, panel);
  uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(16, WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!fb) {
    Serial.println("[Watch] no PSRAM for the framebuffer - screen off");
    return;
  }
  gfx->useFramebuffer(fb);
  if (!gfx->begin()) {
    Serial.println("[Watch] display init failed");
    return;
  }
  panel->setBrightness(Settings::get().brightness);
  loadBackground();
  if (!background) return;
  xTaskCreatePinnedToCore(uiTask, "watch_ui", 6144, nullptr, 1, nullptr, 1);
}

}  // namespace WatchDisplay

#endif  // BOARD_WS_AMOLED_206
