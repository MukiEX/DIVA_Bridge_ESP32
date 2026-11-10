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
uint16_t *base = nullptr;        // last full frame without the live widgets (PSRAM)
uint16_t *strip = nullptr;       // DMA-able bounce buffer for partial pushes

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

// ------------------------------------------------------------- rotation
// The panel's framebuffer is always WIDTH x HEIGHT (portrait). The UI is laid
// out in "logical" coordinates (lw x lh, swapped for 90/270) and gfx's own
// rotation maps drawing calls; shade() and the background copy use the same
// mapping as Arduino_Canvas::writePixelPreclipped.
int rot = 0;            // 0-3 (quarter turns) - set in begin()
int lw = WIDTH, lh = HEIGHT;

inline int fbIndex(int x, int y) {
  switch (rot) {
    case 1: return x * WIDTH + (WIDTH - 1 - y);
    case 2: return (HEIGHT - 1 - y) * WIDTH + (WIDTH - 1 - x);
    case 3: return (HEIGHT - 1 - x) * WIDTH + y;
    default: return y * WIDTH + x;
  }
}

// ------------------------------------------------------------- background
// "bgimage" partition (partitions.csv, written by the setup / web page):
//   "DVBG" u16 width u16 height u32 byte length, then RGB565 little-endian.
// The pages make it at the screen's current orientation (410x502 or 502x410);
// any other size is cover-scaled and centre-cropped here, so a picture made
// before a rotation change still fills the screen.
volatile bool redrawAll = false;

void loadBackground() {
  if (!background) background = (uint16_t *)heap_caps_malloc(WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!background) return;
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bgimage");
  uint8_t head[12];
  int pw = 0, ph = 0;
  bool ok = part && esp_partition_read(part, 0, head, sizeof(head)) == ESP_OK && memcmp(head, "DVBG", 4) == 0;
  if (ok) {
    pw = head[4] | head[5] << 8;
    ph = head[6] | head[7] << 8;
    uint32_t len = head[8] | head[9] << 8 | head[10] << 16 | (uint32_t)head[11] << 24;
    ok = pw > 0 && ph > 0 && len == (uint32_t)pw * ph * 2 && 12 + len <= part->size;
  }
  uint16_t *src = nullptr;
  if (ok) {
    src = (uint16_t *)heap_caps_malloc((size_t)pw * ph * 2, MALLOC_CAP_SPIRAM);
    ok = src && esp_partition_read(part, 12, src, (size_t)pw * ph * 2) == ESP_OK;
  }
  if (ok) {
    // cover: scale so the picture fills lw x lh, crop the middle (nearest neighbour)
    float sc = max((float)lw / pw, (float)lh / ph);
    float ox = (pw * sc - lw) / 2, oy = (ph * sc - lh) / 2;
    for (int y = 0; y < lh; y++) {
      int sy = min(ph - 1, (int)((y + oy) / sc));
      for (int x = 0; x < lw; x++) {
        int sx = min(pw - 1, (int)((x + ox) / sc));
        background[fbIndex(x, y)] = src[sy * pw + sx];
      }
    }
    free(src);
    Serial.printf("[Watch] background image loaded (%dx%d)\n", pw, ph);
    return;
  }
  free(src);
  // Default: a dark teal-to-navy gradient with a soft glow.
  for (int y = 0; y < lh; y++)
    for (int x = 0; x < lw; x++) {
      float t = (float)y / lh;
      float dx = (x - lw * 0.75f) / lw, dy = (y - lh * 0.15f) / lh;
      float glow = expf(-(dx * dx + dy * dy) * 6.0f);
      uint8_t r = 8 + 10 * t + 20 * glow, g = 40 - 22 * t + 90 * glow, b = 52 - 10 * t + 100 * glow;
      background[fbIndex(x, y)] = rgb(r, g, b);
    }
}

// ------------------------------------------------------------- drawing helpers
// Text backdrops: either panels (big boxes) or, in outline mode, a shaded,
// shape-following edge PAD px out around each item (capsules round the pills,
// rings round the face buttons, rounded boxes round text and the slider).
// keep = how much of the picture shows (0-256).
int PAD = 6;
bool outline = false;
int keep = 64;

inline uint16_t darken(uint16_t c) {
  return (((c >> 11) * keep >> 8) << 11) | ((((c >> 5) & 63) * keep >> 8) << 5) | ((c & 31) * keep >> 8);
}

/** Darken a rectangle (logical coordinates). */
void shade(int x0, int y0, int w, int h) {
  if (keep >= 256) return;
  uint16_t *fb = gfx->getFramebuffer();
  for (int y = max(0, y0); y < min(lh, y0 + h); y++)
    for (int x = max(0, x0); x < min(lw, x0 + w); x++) {
      uint16_t &p = fb[fbIndex(x, y)];
      p = darken(p);
    }
}
/** Darken a rounded rectangle (corner radius r). */
void shadeRound(int x0, int y0, int w, int h, int r) {
  if (keep >= 256 || w <= 0 || h <= 0) return;
  r = min(r, min(w, h) / 2);
  uint16_t *fb = gfx->getFramebuffer();
  for (int y = max(0, y0); y < min(lh, y0 + h); y++) {
    int dy = y - y0, inset = 0;
    int c = dy < r ? r - dy : dy >= h - r ? dy - (h - r) + 1 : 0;   // rows into a corner
    if (c > 0) {
      float e = c - 0.5f;
      inset = r - (int)lroundf(sqrtf(max(0.0f, (float)r * r - e * e)));
    }
    for (int x = max(0, x0 + inset); x < min(lw, x0 + w - inset); x++) {
      uint16_t &p = fb[fbIndex(x, y)];
      p = darken(p);
    }
  }
}
/** Darken a disc. */
void shadeCircle(int cx, int cy, int r) {
  if (keep >= 256) return;
  uint16_t *fb = gfx->getFramebuffer();
  for (int y = max(0, cy - r); y <= min(lh - 1, cy + r); y++)
    for (int x = max(0, cx - r); x <= min(lw - 1, cx + r); x++)
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
        uint16_t &p = fb[fbIndex(x, y)];
        p = darken(p);
      }
}
/** Panel-mode box. */
void panelBox(int x, int y, int w, int h) {
  if (!outline) shade(x, y, w, h);
}
/** Outline-mode backdrop for one item; r = the item's own corner radius. */
void itemBox(int x, int y, int w, int h, int r = 2) {
  if (outline) shadeRound(x - PAD, y - PAD, w + 2 * PAD, h + 2 * PAD, r + PAD);
}

/** align: 0 = left, 1 = right, -1 = centre. */
void text(int x, int y, const char *s, uint16_t color, const uint8_t *font, int align = 0) {
  if (!*s) return;
  gfx->setFont(font);
  gfx->setTextColor(color);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(s, 0, y, &bx, &by, &bw, &bh);
  if (align) x -= align > 0 ? bw : bw / 2;
  itemBox(x + bx, by, bw, bh);
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
  itemBox(x, y, w, 24, 12);
  gfx->fillRoundRect(x, y, w, 24, 12, on ? color : C_OFF);
  bool o = outline;
  outline = false;  // the pill is its own backdrop
  text(x + w / 2, y + 17, label, on ? 0x0000 : C_DIM, u8g2_font_helvB10_tr, -1);
  outline = o;
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

/** First touch point in logical (rotated) coordinates; false when not touched. */
bool readTouch(int &x, int &y) {
  uint8_t r[5];
  if (!i2cRead(FT3168_ADDR, 0x02, r, 5)) return false;
  int count = r[0] & 0x0F;
  if (count < 1 || count > 2) return false;
  int nx = (r[1] & 0x0F) << 8 | r[2], ny = (r[3] & 0x0F) << 8 | r[4];
  nx = constrain(nx, 0, WIDTH - 1);
  ny = constrain(ny, 0, HEIGHT - 1);
  switch (rot) {   // inverse of fbIndex()
    case 1: x = ny; y = WIDTH - 1 - nx; break;
    case 2: x = WIDTH - 1 - nx; y = HEIGHT - 1 - ny; break;
    case 3: x = HEIGHT - 1 - ny; y = nx; break;
    default: x = nx; y = ny;
  }
  return true;
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
  bool wifiUp, wifiPaused;
  char ip[16];
  int battery;
  bool charging;
  int rate;
};

/** Where things go, for portrait (410x502) or landscape (502x410). */
struct Layout {
  int titleH, titleBase;
  int cardY, cardH, nameY, pillY, sliderY, sliderH, faceY, faceR, extY;
  int statY, statH, cols, rowStart, rowH, valueX;
  int footH;
};
constexpr Layout PORTRAIT = {64, 44, 74, 200, 104, 86, 124, 34, 214, 34, 266, 286, 180, 1, 314, 30, 128, 26};
constexpr Layout LANDSCAPE = {56, 40, 64, 184, 92, 74, 106, 32, 186, 30, 238, 256, 118, 2, 282, 30, 104, 26};

// ------------------------------------------------------------- live widgets
// The controller widgets (L/FN pills, slider, face buttons, extra buttons) change
// with every touch; everything else changes rarely. A full frame keeps a copy of
// itself without the widgets ("base"), so an input change only restores and
// redraws the widgets that changed and pushes those rectangles to the panel -
// a few KB instead of the whole 400 KB frame.
enum : uint8_t { W_PILLS = 1, W_SLIDER = 2, W_FACES = 4, W_EXT = 8, W_ALL = 15 };
struct Rect { int x, y, w, h; };

const Layout &layout() { return lw > lh ? LANDSCAPE : PORTRAIT; }

Rect widgetRect(uint8_t w) {
  const Layout &L = layout();
  int m = PAD + 2;
  switch (w) {
    case W_PILLS: return {lw - 28 - 200 - m, L.pillY - m, 200 + 2 * m, 24 + 2 * m};
    case W_SLIDER: return {28 - 4 - m, L.sliderY - 4 - m, lw - 56 + 8 + 2 * m, L.sliderH + 8 + 2 * m};
    case W_FACES: return {14, L.faceY - L.faceR - m, lw - 28, 2 * (L.faceR + m)};
    default: return {14, L.extY - 18, lw - 28, 26};
  }
}

uint8_t widgetChanges(const uint8_t *a, const uint8_t *b) {
  Decoded x = decode(a), y = decode(b);
  const uint32_t pills = 1u << BTN_L1 | 1u << BTN_L2 | 1u << BTN_L3 | 1u << BTN_FN;
  uint8_t m = 0;
  if ((x.pressed ^ y.pressed) & pills) m |= W_PILLS;
  if ((x.pressed ^ y.pressed) & ~pills) m |= W_FACES;
  if (x.sensors != y.sensors) m |= W_SLIDER;
  if (a[24] != b[24] || a[25] != b[25]) m |= W_EXT;
  return m;
}

void drawWidgets(const Snapshot &s, uint8_t which) {
  const Layout &L = layout();
  Decoded d = decode(s.pkt);
  uint16_t ext = s.pkt[24] | s.pkt[25] << 8;
  if (which & W_PILLS) {
    const char *small[4] = {"L1", "L2", "L3", "FN"};
    const int bits[4] = {BTN_L1, BTN_L2, BTN_L3, BTN_FN};
    for (int i = 0; i < 4; i++)
      pill(lw - 28 - (4 - i) * 50, L.pillY, small[i], d.pressed >> bits[i] & 1, i == 3 ? C_AMBER : C_PINK);
  }
  if (which & W_SLIDER) {
    const int sx = 28, sy = L.sliderY, sw = lw - 56, sh = L.sliderH;
    itemBox(sx - 4, sy - 4, sw + 8, sh + 8, 8);
    gfx->drawRoundRect(sx - 4, sy - 4, sw + 8, sh + 8, 8, rgb(0x2B, 0x6E, 0x78));
    for (int i = 0; i < 32; i++) {
      int x0 = sx + i * sw / 32, x1 = sx + (i + 1) * sw / 32;
      bool on = d.sensors >> i & 1;
      gfx->fillRect(x0 + 1, sy, x1 - x0 - 2, sh, on ? C_CYAN : (i % 8 ? rgb(0x1E, 0x2A, 0x30) : rgb(0x26, 0x36, 0x3D)));
    }
  }
  if (which & W_FACES) {
    const int keys[4] = {BTN_TRIANGLE, BTN_SQUARE, BTN_CROSS, BTN_CIRCLE};
    const uint16_t colors[4] = {C_GREEN, C_PINK, C_BLUE, C_RED};
    const int r = L.faceR, span = lw - 56 - 2 * (r + 8);
    for (int i = 0; i < 4; i++) {
      int cx = 28 + r + 8 + i * span / 3, cy = L.faceY;
      bool on = d.pressed >> keys[i] & 1;
      if (outline) shadeCircle(cx, cy, r + PAD);
      if (on) gfx->fillCircle(cx, cy, r, colors[i]);
      for (int t = 0; t < 3; t++) gfx->drawCircle(cx, cy, r - t, colors[i]);
      symbol(i, cx, cy, r / 2, on ? 0xFFFF : colors[i]);
    }
  }
  if ((which & W_EXT) && ext) {
    char e[48] = "";
    const char *names[] = {"Up", "Dn", "Lt", "Rt", "LB", "RB", "LT", "RT", "LS", "RS"};
    for (int i = 0; i < 10; i++)
      if (ext >> i & 1) {
        strcat(e, names[i]);
        strcat(e, " ");
      }
    e[strlen(e) - 1] = 0;
    text(lw / 2, L.extY, e, C_AMBER, u8g2_font_helvB10_tr, -1);
  }
}

/** Logical rectangle -> panel (native) rectangle, clipped and even-aligned (CO5300). */
Rect toNative(Rect r) {
  int x0 = max(0, r.x), y0 = max(0, r.y), x1 = min(lw, r.x + r.w) - 1, y1 = min(lh, r.y + r.h) - 1;
  int a = fbIndex(x0, y0), b = fbIndex(x1, y1);
  int ax = a % WIDTH, ay = a / WIDTH, bx = b % WIDTH, by = b / WIDTH;
  int nx0 = min(ax, bx) & ~1, ny0 = min(ay, by) & ~1;
  int nx1 = max(ax, bx) | 1, ny1 = max(ay, by) | 1;
  nx1 = min(nx1, WIDTH - 1);
  ny1 = min(ny1, HEIGHT - 1);
  return {nx0, ny0, nx1 - nx0 + 1, ny1 - ny0 + 1};
}

constexpr int STRIP_PX = 4096;

/** Copy a native rectangle of `from` into the framebuffer. */
void restoreNative(const Rect &n, const uint16_t *from) {
  uint16_t *fb = gfx->getFramebuffer();
  for (int y = n.y; y < n.y + n.h; y++) memcpy(fb + y * WIDTH + n.x, from + y * WIDTH + n.x, n.w * 2);
}

/** Send a native rectangle of the framebuffer to the panel. */
void pushNative(const Rect &n) {
  uint16_t *fb = gfx->getFramebuffer();
  int rows = max(1, STRIP_PX / n.w) & ~1;
  if (!rows) rows = 2;
  for (int y = n.y; y < n.y + n.h; y += rows) {
    int h = min(rows, n.y + n.h - y);
    for (int i = 0; i < h; i++) memcpy(strip + i * n.w, fb + (y + i) * WIDTH + n.x, n.w * 2);
    panel->draw16bitRGBBitmap(n.x, y, strip, n.w, h);
  }
}

/** Input-only change: redraw just the widgets that changed. */
void drawPartial(const Snapshot &s, uint8_t which) {
  for (uint8_t w = 1; w <= W_EXT; w <<= 1) {
    if (!(which & w)) continue;
    Rect n = toNative(widgetRect(w));
    restoreNative(n, base);
    drawWidgets(s, w);
    pushNative(n);
  }
}

void pageDots();

void draw(const Snapshot &s) {
  memcpy(gfx->getFramebuffer(), background, WIDTH * HEIGHT * 2);
  auto &cfg = Settings::get();
  const Layout &L = lw > lh ? LANDSCAPE : PORTRAIT;
  char line[64];

  // Title, centred so the rounded corners don't clip it
  panelBox(0, 0, lw, L.titleH);
  text(lw / 2, L.titleBase - 6, "DIVA BRIDGE", C_TEXT, u8g2_font_helvB24_tr, -1);
  pageDots();

  // Controller card
  panelBox(14, L.cardY, lw - 28, L.cardH);
  if (!s.present) {
    int my = L.cardY + L.cardH / 2;
    text(lw / 2, my - 10, "No controller", C_TEXT, u8g2_font_helvB14_tr, -1);
    text(lw / 2, my + 16, "Plug one into the USB-C port", C_DIM, u8g2_font_helvR10_tr, -1);
    text(lw / 2, my + 34, "(through a powered hub)", C_DIM, u8g2_font_helvR10_tr, -1);
  } else {
    text(28, L.nameY, s.name, C_TEXT, u8g2_font_helvB14_tr);
  }

  // Status card: one column (portrait) or two (landscape)
  panelBox(14, L.statY, lw - 28, L.statH);
  int col = 0, y = L.rowStart;
  const int colW = (lw - 28) / L.cols;
  auto row = [&](const char *label, const char *value, uint16_t color, const char *sub = nullptr) {
    int x = 28 + col * colW;
    text(x, y, label, C_DIM, u8g2_font_helvR12_tr);
    text(x + L.valueX - 28, y, value, color, u8g2_font_helvB14_tr);
    if (sub) {
      y += 18;
      text(x + L.valueX - 28, y, sub, C_DIM, u8g2_font_helvR10_tr);
    }
    y += L.rowH;
  };
  auto nextCol = [&] {
    if (L.cols > 1) {
      col = 1;
      y = L.rowStart;
    }
  };
  snprintf(line, sizeof(line), "%s.local:%u", NetBridge::hostName().c_str(), cfg.port);
  if (cfg.radio == Settings::Radio::BleOnly) row("Wi-Fi", "off", C_DIM);
  else if (s.wifiPaused) row("Wi-Fi", "paused (BT game)", C_DIM);
  else if (s.wifiUp) row("Wi-Fi", s.ip, C_TEXT, line);
  else row("Wi-Fi", cfg.ssid.length() ? "connecting..." : "not set up", C_AMBER);
  if (s.native) snprintf(line, sizeof(line), "%d (%d Wi-Fi)", s.native, s.tcp);
  else snprintf(line, sizeof(line), "none");
  row("Games", line, s.native ? C_GREEN : C_DIM);
  nextCol();
  const char *pad = cfg.radio == Settings::Radio::WifiOnly ? "off"
                    : !s.ble                               ? "not paired"
                    : s.native                             ? "muted (game)"
                                                           : "connected";
  row("Xbox pad", pad, cfg.radio != Settings::Radio::WifiOnly && s.ble && !s.native ? C_GREEN : C_DIM);
  snprintf(line, sizeof(line), "%d reports/s", s.rate);
  row("USB", s.present ? line : "-", C_DIM);
  if (s.battery >= 0) snprintf(line, sizeof(line), "%d%%%s", s.battery, s.charging ? " charging" : "");
  else snprintf(line, sizeof(line), "no battery");
  row("Battery", line, s.battery >= 0 && s.battery < 15 && !s.charging ? C_RED : s.charging ? C_GREEN : C_TEXT);

  // Footer
  snprintf(line, sizeof(line), "v%s  -  hold BOOT while plugging in to flash", DIVA_BRIDGE_VERSION);
  panelBox(0, lh - L.footH, lw, L.footH);
  text(lw / 2, lh - 8, line, C_DIM, u8g2_font_helvR10_tr, -1);

  if (base) memcpy(base, gfx->getFramebuffer(), WIDTH * HEIGHT * 2);
  if (s.present) drawWidgets(s, W_ALL);
  gfx->flush();
}

// ------------------------------------------------------------- Bluetooth page
// Tap the screen to switch to it (and back): links, whether each did the DIVA
// handshake and is still checking in, the input stream, and recent events.
int page = 0;   // 0 = controller, 1 = paired devices, 2 = Bluetooth status
char lineBuf[64];
char *line_buf() { return lineBuf; }
// ------------------------------------------------------------- pages
// Swipe left/right to move between pages (they loop): 0 controller, 1 paired
// devices, 2 Bluetooth status. A touch that wakes a dimmed screen does nothing else.
constexpr int PAGES = 3;

/** Three dots under the title: which page this is. */
void pageDots() {
  const Layout &L = layout();
  int y = L.titleH - 8;
  for (int i = 0; i < PAGES; i++) {
    int cx = lw / 2 + (i - 1) * 14;
    if (i == page) gfx->fillCircle(cx, y, 3, C_TEXT);
    else gfx->drawCircle(cx, y, 3, C_DIM);
  }
}

// Buttons on the devices page (logical coordinates), for tap hit-testing.
struct Btn {
  Rect r;
  uint8_t action;   // 1 disconnect, 2 forget, 3 pairing on/off, 4 allow
  char addr[18];
};
Btn btns[12];
int nbtn = 0;
char confirmAddr[18] = "";   // "Forget" tapped once: tap again within 3 s
uint32_t confirmUntil = 0;

void button(int x, int y, int w, int h, const char *label, uint16_t color, uint8_t action, const char *addr) {
  itemBox(x, y, w, h, h / 2);
  gfx->fillRoundRect(x, y, w, h, h / 2, color);
  bool o = outline;
  outline = false;
  text(x + w / 2, y + h / 2 + 5, label, 0x0000, u8g2_font_helvB10_tr, -1);
  outline = o;
  if (nbtn < 12) {
    btns[nbtn].r = {x, y, w, h};
    btns[nbtn].action = action;
    snprintf(btns[nbtn].addr, sizeof(btns[nbtn].addr), "%s", addr);
    nbtn++;
  }
}

void drawDevices() {
  memcpy(gfx->getFramebuffer(), background, WIDTH * HEIGHT * 2);
  const Layout &L = layout();
  nbtn = 0;
  panelBox(0, 0, lw, L.titleH);
  text(lw / 2, L.titleBase - 6, "DEVICES", C_TEXT, u8g2_font_helvB24_tr, -1);
  pageDots();
  BleBridge::BondInfo b[6];
  int n = BleBridge::bonded(b, 6);
  int rowH = 74, y0 = L.titleH + 10;
  // Pairing mode strip at the bottom: "Pair new device" / countdown + Stop
  const int pairH = 56, pairY = lh - L.footH - 8 - pairH;
  int left = BleBridge::pairingLeft();
  panelBox(14, pairY, lw - 28, pairH);
  if (left == 0) {
    text(28, pairY + 34, "Pairing is closed", C_DIM, u8g2_font_helvR10_tr);
    button(lw - 28 - 170, pairY + 13, 170, 30, "Pair new device", C_CYAN, 3, "");
  } else {
    char msg[48];
    if (left < 0) snprintf(msg, sizeof(msg), "Pairing open: first device");
    else snprintf(msg, sizeof(msg), "Pairing open: %d s", left);
    text(28, pairY + 26, msg, C_GREEN, u8g2_font_helvB10_tr);
    text(28, pairY + 44, "Pair \"DIVA Bridge\" now", C_DIM, u8g2_font_helvR10_tr);
    button(lw - 28 - 90, pairY + 13, 90, 30, "Stop", C_AMBER, 3, "");
  }
  int maxRows = (pairY - 6 - y0) / (rowH + 6);
  if (!n) {
    panelBox(14, y0, lw - 28, 90);
    text(28, y0 + 34, "No paired devices", C_TEXT, u8g2_font_helvB14_tr);
    text(28, y0 + 58, "Pair \"DIVA Bridge\" in a device's", C_DIM, u8g2_font_helvR10_tr);
    text(28, y0 + 76, "Bluetooth settings.", C_DIM, u8g2_font_helvR10_tr);
  }
  bool confirming = confirmAddr[0] && millis() < confirmUntil;
  for (int i = 0; i < n && i < maxRows; i++) {
    int y = y0 + i * (rowH + 6);
    panelBox(14, y, lw - 28, rowH);
    text(28, y + 26, b[i].addr, C_TEXT, u8g2_font_helvB14_tr);
    const char *st = b[i].blocked ? "disconnected - stays off" : b[i].host ? "connected - in use"
                     : b[i].connected ? "connected" : "not connected";
    text(28, y + 52, st, b[i].blocked ? C_AMBER : b[i].connected ? C_GREEN : C_DIM, u8g2_font_helvR10_tr);
    int bw = 92, bh = 30, by = y + rowH - bh - 10, bx = lw - 28 - bw;
    bool sure = confirming && strcmp(confirmAddr, b[i].addr) == 0;
    button(bx, by, bw, bh, sure ? "Sure?" : "Forget", sure ? C_RED : rgb(0xE0, 0x80, 0x80), 2, b[i].addr);
    if (b[i].blocked) button(bx - 8 - 108, by, 108, bh, "Allow", C_GREEN, 4, b[i].addr);
    else if (b[i].connected) button(bx - 8 - 108, by, 108, bh, "Disconnect", C_AMBER, 1, b[i].addr);
  }
  if (n > maxRows) {
    snprintf(line_buf(), 40, "+%d more", n - maxRows);
    text(lw - 28, pairY - 4, line_buf(), C_DIM, u8g2_font_helvR10_tr, 1);
  }
  panelBox(0, lh - L.footH, lw, L.footH);
  text(lw / 2, lh - 8, "swipe for more  -  Forget: tap twice", C_DIM, u8g2_font_helvR10_tr, -1);
  gfx->flush();
}

/** A tap on the devices page. Returns true if something changed. */
bool devicesTap(int x, int y) {
  for (int i = 0; i < nbtn; i++) {
    const Rect &r = btns[i].r;
    if (x < r.x - 6 || x > r.x + r.w + 6 || y < r.y - 6 || y > r.y + r.h + 6) continue;
    if (btns[i].action == 3) {
      if (BleBridge::pairingLeft() != 0) BleBridge::closePairing();
      else BleBridge::openPairing(60);
    } else if (btns[i].action == 1) {
      BleBridge::disconnectDevice(btns[i].addr);
    } else if (btns[i].action == 4) {
      BleBridge::allowDevice(btns[i].addr);
    } else if (confirmAddr[0] && millis() < confirmUntil && strcmp(confirmAddr, btns[i].addr) == 0) {
      BleBridge::forgetDevice(btns[i].addr);
      confirmAddr[0] = 0;
    } else {
      snprintf(confirmAddr, sizeof(confirmAddr), "%s", btns[i].addr);
      confirmUntil = millis() + 3000;
    }
    return true;
  }
  return false;
}



const char *ago(uint32_t ms, char *buf, size_t n) {
  if (ms < 10000) snprintf(buf, n, "%u.%us", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
  else if (ms < 600000) snprintf(buf, n, "%us", (unsigned)(ms / 1000));
  else snprintf(buf, n, "%um", (unsigned)(ms / 60000));
  return buf;
}

void drawBluetooth(int sentPerSec, uint32_t failed) {
  memcpy(gfx->getFramebuffer(), background, WIDTH * HEIGHT * 2);
  const Layout &L = layout();
  auto &cfg = Settings::get();
  char line[80], t[16];
  panelBox(0, 0, lw, L.titleH);
  text(lw / 2, L.titleBase - 6, "BLUETOOTH", C_TEXT, u8g2_font_helvB24_tr, -1);
  pageDots();
  panelBox(14, L.titleH + 8, lw - 28, lh - L.titleH - 8 - L.footH - 8);
  int y = L.titleH + 34, x = 28;
  if (cfg.radio == Settings::Radio::WifiOnly) {
    text(x, y, "Bluetooth is off (radio = Wi-Fi only)", C_AMBER, u8g2_font_helvB14_tr);
  } else {
    BleBridge::PeerInfo pi[3];
    int n = BleBridge::peers(pi, 3);
    snprintf(line, sizeof(line), "%d link%s - %d paired - %s", n, n == 1 ? "" : "s", BleBridge::pairedCount(),
             BleBridge::advertising() ? "adv." : "not adv.");
    text(x, y, line, C_TEXT, u8g2_font_helvB14_tr);
    y += 28;
    if (!n) {
      text(x, y, "Nothing connected. A game connects when it starts.", C_DIM, u8g2_font_helvR10_tr);
      y += 22;
    }
    for (int i = 0; i < n; i++) {
      text(x, y, pi[i].addr, C_TEXT, u8g2_font_helvB10_tr);
      y += 20;
      if (pi[i].native) {
        if (pi[i].proto >= 3)
          snprintf(line, sizeof(line), "Handshake ACTIVE (v%u), checked in %s ago", pi[i].proto,
                   ago(pi[i].quietMs, t, sizeof(t)));
        else
          snprintf(line, sizeof(line), "Handshake ACTIVE (v%u, no check-ins)", pi[i].proto);
        text(x + 12, y, line, C_GREEN, u8g2_font_helvB10_tr);
      } else {
        text(x + 12, y, pi[i].hid ? "No handshake (Xbox pad link)" : "No handshake (idle link)", C_AMBER,
             u8g2_font_helvB10_tr);
      }
      y += 18;
      snprintf(line, sizeof(line), "Input %s  -  %.2f ms  -  MTU %u",
               pi[i].inputSub ? "subscribed" : "NOT subscribed", pi[i].intervalMs, pi[i].mtu);
      text(x + 12, y, line, pi[i].inputSub ? C_DIM : C_AMBER, u8g2_font_helvR10_tr);
      y += 24;
    }
    snprintf(line, sizeof(line), "Stream: %d updates/s, %u failed", sentPerSec, (unsigned)failed);
    text(x, y, line, C_DIM, u8g2_font_helvR10_tr);
    y += 28;
    text(x, y, "Recent", C_TEXT, u8g2_font_helvB10_tr);
    y += 20;
    BleBridge::Event ev[12];
    int ne = BleBridge::events(ev, 12);
    uint32_t now = millis();
    for (int i = 0; i < ne && y < lh - L.footH - 14; i++) {
      text(x, y, ago(now - ev[i].ms, t, sizeof(t)), C_DIM, u8g2_font_helvR10_tr);
      text(x + 52, y, ev[i].text, C_TEXT, u8g2_font_helvR10_tr);
      y += 17;
    }
    if (!ne) text(x, y, "(nothing yet)", C_DIM, u8g2_font_helvR10_tr);
  }
  panelBox(0, lh - L.footH, lw, L.footH);
  text(lw / 2, lh - 8, "swipe for more pages", C_DIM, u8g2_font_helvR10_tr, -1);
  gfx->flush();
}

void uiTask(void *) {
  Snapshot cur = {}, last = {};
  uint32_t lastDraw = 0, lastPartial = 0, lastRateShown = 0, lastActivity = millis(), lastBattery = 0, lastRate = 0, lastReports = 0;
  int rate = 0;
  bool dimmed = false, firstDraw = true, wasTouched = false, wakeTouch = false;
  int downX = 0, downY = 0, lastX = 0, lastY = 0;
  uint32_t downAt = 0, lastDevDraw = 0;
  uint32_t lastBtDraw = 0, lastSent = 0, lastSentAt = 0;
  int sentRate = 0;
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
    cur.wifiPaused = NetBridge::paused();
    snprintf(cur.ip, sizeof(cur.ip), "%s", WiFi.localIP().toString().c_str());
    cur.battery = battery;
    cur.charging = isCharging;
    // the report rate is the only status that ticks by itself: don't let it force a
    // full redraw while someone is playing (it would hitch the live widgets)
    if (now - lastActivity > 400 || now - lastRateShown > 5000) {
      cur.rate = rate;
      lastRateShown = now;
    } else {
      cur.rate = last.rate;
    }

    bool inputChanged = memcmp(cur.pkt, last.pkt, sizeof(cur.pkt)) != 0 || cur.present != last.present;
    int tx = 0, ty = 0;
    bool touch = readTouch(tx, ty);
    if (touch && !wasTouched) {   // finger down
      downX = lastX = tx;
      downY = lastY = ty;
      downAt = now;
      wakeTouch = dimmed;           // this touch only wakes the screen
    } else if (touch) {
      lastX = tx;
      lastY = ty;
    } else if (wasTouched && !wakeTouch) {   // finger up: swipe or tap?
      int dx = lastX - downX, dy = lastY - downY;
      if (abs(dx) > 60 && abs(dx) > abs(dy) * 3 / 2) {
        page = (page + (dx < 0 ? 1 : PAGES - 1)) % PAGES;   // swipe left = next
        redrawAll = true;
        firstDraw = true;
      } else if (abs(dx) < 20 && abs(dy) < 20 && now - downAt < 600) {
        if (page == 1 && devicesTap(downX, downY)) redrawAll = true;
      }
    }
    wasTouched = touch;
    if (inputChanged || touch) lastActivity = now;
    uint32_t timeout = cfg.screenTimeout * 1000u;
    if (dimmed && now - lastActivity < 200) {
      panel->setBrightness(cfg.brightness);
      dimmed = false;
    } else if (!dimmed && timeout && now - lastActivity > timeout) {
      panel->setBrightness(max(4, cfg.brightness / 16));  // dim, not off: AMOLED, keep it gentle
      dimmed = true;
    }
    if (page == 1) {   // devices page: redraw twice a second (and after a tap)
      if (redrawAll || now - lastDevDraw >= 500) {
        redrawAll = false;
        drawDevices();
        lastDevDraw = now;
      }
      last = cur;
      vTaskDelay(pdMS_TO_TICKS(8));
      continue;
    }
    if (page == 2) {   // Bluetooth status page: redraw a few times a second
      if (now - lastSentAt >= 1000) {
        uint32_t sNow = BleBridge::notifySent();
        sentRate = (sNow - lastSent) * 1000 / max<uint32_t>(1, now - lastSentAt);
        lastSent = sNow;
        lastSentAt = now;
      }
      if (redrawAll || now - lastBtDraw >= 250) {
        redrawAll = false;
        drawBluetooth(sentRate, BleBridge::notifyFailed());
        lastBtDraw = now;
        firstDraw = true;   // back on page 0: start with a full frame
      }
      last = cur;
      vTaskDelay(pdMS_TO_TICKS(8));
      continue;
    }
    // Only the controller's buttons/slider moved: partial update, as often as every 8 ms.
    Snapshot same = cur;
    memcpy(same.pkt, last.pkt, sizeof(same.pkt));
    bool onlyInput = !firstDraw && !redrawAll && base && strip && cur.present && last.present &&
                     memcmp(&same, &last, sizeof(same)) == 0;
    bool changed = firstDraw || inputChanged || memcmp(&cur, &last, sizeof(cur)) != 0;
    if (redrawAll) {
      redrawAll = false;
      changed = true;
    }
    if (onlyInput && inputChanged && now - lastPartial >= 8) {
      drawPartial(cur, widgetChanges(cur.pkt, last.pkt));
      last = cur;
      lastPartial = now;
    } else if ((changed && !onlyInput && now - lastDraw >= 33) ||
               (now - lastDraw >= 5000 && now - lastActivity > 400)) {   // occasional full refresh
      draw(cur);
      last = cur;
      lastDraw = lastPartial = now;
      firstDraw = false;
    }
    vTaskDelay(pdMS_TO_TICKS(4));
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
  auto &cfg = Settings::get();
  rot = (cfg.rotation / 90) & 3;
  gfx->setRotation(rot);
  lw = gfx->width();
  lh = gfx->height();
  outline = cfg.panelOutline;
  PAD = cfg.panelPad;
  keep = 256 - cfg.panelOpacity * 256 / 100;
  panel->setBrightness(Settings::get().brightness);
  loadBackground();
  if (!background) return;
  base = (uint16_t *)heap_caps_malloc(WIDTH * HEIGHT * 2, MALLOC_CAP_SPIRAM);
  strip = (uint16_t *)heap_caps_malloc(STRIP_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  xTaskCreatePinnedToCore(uiTask, "watch_ui", 6144, nullptr, 1, nullptr, 1);
}

}  // namespace WatchDisplay

#endif  // BOARD_WS_AMOLED_206
