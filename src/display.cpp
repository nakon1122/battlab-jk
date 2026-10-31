// On-board dashboard for the ESP32-3248S035 ("Cheap Yellow Display" 3.5").
// ST7796 480x320 over SPI + XPT2046 resistive touch. Tap anywhere to switch
// between Overview / Cells / Info.
#include "display.h"

#ifndef BATTLAB_DISPLAY
void displayStart() {}
#else
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WiFi.h>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

public:
  void configure(bool invert, bool rgbOrder) {
    {
      auto cfg = bus_.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = true;
      cfg.invert = invert;
      cfg.rgb_order = rgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 200;
      cfg.y_max = 3900;
      cfg.pin_int = 36;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs = 33;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

static LGFX tft;
static uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_MUTED, C_ACCENT, C_GOOD, C_WARN,
    C_BAD, C_BLUE;

static String fmt(float v, int d) {
  char b[16];
  snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}

static const char *stateLabel(const String &s) {
  if (s == "LIVE") return "LIVE";
  if (s == "SCANNING") return "SCANNING";
  if (s == "CONNECTING") return "CONNECTING";
  if (s == "WAITING_DEVICE_INFO" || s == "WAITING_DATA" ||
      s == "VERIFYING_PROTOCOL")
    return "WAITING BMS";
  if (s == "NO_RESPONSE" || s == "NO_CELL_DATA") return "NO DATA";
  if (s == "STALE") return "STALE";
  if (s == "NO_JK_FOUND") return "NO BMS FOUND";
  if (s == "SELECT_BMS") return "SELECT BMS (WEB)";
  if (s == "UNCONFIGURED") return "NOT CONFIGURED";
  if (s == "CONNECT_FAILED") return "CONNECT FAILED";
  return s.c_str();
}

// Draw a rounded card with label/value/unit into an off-screen sprite and
// push it in one go, so values update without flicker.
static void tile(int x, int y, int w, int h, const char *label,
                 const String &value, const char *unit, uint16_t accent) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(w, h)) return;
  s.fillSprite(C_BG);
  s.fillRoundRect(0, 0, w, h, 10, C_CARD);
  s.fillRoundRect(0, 10, 4, h - 20, 2, accent);
  s.setTextColor(C_MUTED, C_CARD);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::top_left);
  s.drawString(label, 12, 7);
  s.setTextColor(C_TEXT, C_CARD);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextDatum(textdatum_t::bottom_left);
  s.drawString(value, 12, h - 6);
  int vw = s.textWidth(value);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_CARD);
  s.drawString(unit, 16 + vw, h - 10);
  s.pushSprite(x, y);
  s.deleteSprite();
}

static void header(const DisplaySnap &d, int page) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(480, 40)) return;
  s.fillSprite(C_BG);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_ACCENT, C_BG);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString("BATTLAB", 12, 20);
  String st = stateLabel(d.state);
  if (d.detected.length() && d.live) st = d.detected + "  " + st;
  uint16_t dot = d.live ? C_GOOD : d.linked ? C_WARN : C_BAD;
  s.fillCircle(150, 20, 6, dot);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_TEXT, C_BG);
  s.drawString(st, 164, 21);
  // page dots
  for (int i = 0; i < 3; i++)
    s.fillCircle(432 + i * 16, 20, 4, i == page ? C_ACCENT : C_LINE);
  if (d.linked) {
    s.setTextDatum(textdatum_t::middle_right);
    s.setTextColor(C_MUTED, C_BG);
    s.drawString(String(d.rssi) + " dBm", 412, 21);
  }
  s.drawFastHLine(0, 39, 480, C_LINE);
  s.pushSprite(0, 0);
  s.deleteSprite();
}

static int lastSocDrawn = -2;
static void socGauge(const DisplaySnap &d, bool force) {
  int soc = d.live ? int(d.sample.soc + .5f) : -1;
  if (!force && soc == lastSocDrawn) return;
  lastSocDrawn = soc;
  const int cx = 112, cy = 150, r1 = 92, r0 = 76;
  uint16_t col = soc < 0 ? C_LINE : soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
  tft.fillArc(cx, cy, r0, r1, 135, 405, C_LINE);
  if (soc > 0)
    tft.fillArc(cx, cy, r0, r1, 135, 135 + 270.f * soc / 100.f, col);
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(140, 90)) return;
  s.fillSprite(C_BG);
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_TEXT, C_BG);
  s.setFont(&fonts::FreeSansBold24pt7b);
  s.drawString(soc < 0 ? String("--") : String(soc) + "%", 70, 36);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString("STATE OF CHARGE", 70, 76);
  s.pushSprite(cx - 70, cy - 40);
  s.deleteSprite();
}

static void pill(LGFX_Sprite &s, int x, int y, const char *t, bool on) {
  uint16_t bg = on ? C_GOOD : C_LINE;
  s.fillRoundRect(x, y, 78, 26, 13, bg);
  s.setTextColor(on ? C_BG : C_MUTED, bg);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(t, x + 39, y + 14);
}

static void cellStats(const jk::Sample &s, float &mn, float &mx, int &imn,
                      int &imx) {
  mn = 9;
  mx = 0;
  imn = imx = -1;
  for (int i = 0; i < 32; i++) {
    float v = s.cells[i];
    if (v <= 0) continue;
    if (v < mn) mn = v, imn = i;
    if (v > mx) mx = v, imx = i;
  }
}

static void overview(const DisplaySnap &d, bool force) {
  socGauge(d, force);
  const auto &s = d.sample;
  bool ok = d.live;
  float p = s.voltage * s.current;
  int x0 = 222, x1 = 352, w = 122, h = 64;
  tile(x0, 48, w, h, "VOLTAGE", ok ? fmt(s.voltage, 2) : "--", "V", C_ACCENT);
  tile(x1, 48, w, h, "CURRENT", ok ? fmt(s.current, 1) : "--", "A",
       s.current < 0 ? C_WARN : C_GOOD);
  tile(x0, 118, w, h, "POWER", ok ? fmt(p, 0) : "--", "W", C_BLUE);
  tile(x1, 118, w, h, "REMAIN", ok ? fmt(s.capacity, 1) : "--", "Ah", C_ACCENT);
  tile(x0, 188, w, h, "TEMP", ok ? fmt(s.temp1, 0) : "--", "C", C_WARN);
  tile(x1, 188, w, h, "MOS", ok ? fmt(s.mos, 0) : "--", "C", C_WARN);

  LGFX_Sprite b(&tft);
  b.setColorDepth(16);
  if (!b.createSprite(462, 54)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 54, 10, C_CARD);
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_MUTED, C_CARD);
  b.drawString("CELLS", 12, 16);
  b.setTextColor(C_TEXT, C_CARD);
  String line = ok && imn >= 0
      ? String(s.count) + "S   min " + fmt(mn, 3) + "  max " + fmt(mx, 3) +
            "   delta " + String(int((mx - mn) * 1000 + .5f)) + " mV"
      : String("--");
  b.drawString(line, 12, 38);
  pill(b, 290, 14, "CHG", ok && s.charge);
  pill(b, 376, 14, "DSG", ok && s.discharge);
  b.pushSprite(9, 260);
  b.deleteSprite();
}

static void cells(const DisplaySnap &d, bool) {
  const auto &s = d.sample;
  int n = d.live ? int(s.count) : 0;
  if (n > 32) n = 32;
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  int cols = n <= 16 ? 2 : 4;
  int rows = n ? (n + cols - 1) / cols : 1;
  int cw = 462 / cols - 6, ch = rows ? (214 / rows) - 4 : 30;
  if (ch > 40) ch = 40;
  LGFX_Sprite c(&tft);
  c.setColorDepth(16);
  if (!c.createSprite(cw, ch)) return;
  if (!n) {
    c.fillSprite(C_BG);
    c.setTextDatum(textdatum_t::middle_left);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextColor(C_MUTED, C_BG);
    c.drawString("No cell data yet", 4, ch / 2);
    c.pushSprite(12, 56);
  }
  // Visible voltage window for the bar length
  float lo = 2.8f, hi = 4.25f;
  if (mx > 0 && mx < 3.7f) lo = 2.5f, hi = 3.65f; // LiFePO4-ish pack
  int idx = 0;
  for (int i = 0; i < 32 && idx < n; i++) {
    float v = s.cells[i];
    if (v <= 0) continue;
    int col = idx % cols, row = idx / cols;
    idx++;
    c.fillSprite(C_BG);
    c.fillRoundRect(0, 0, cw, ch, 6, C_CARD);
    float f = (v - lo) / (hi - lo);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    uint16_t bar = i == imx ? C_BAD : i == imn ? C_BLUE : C_GOOD;
    c.fillRoundRect(0, ch - 5, int(cw * f), 5, 2, bar);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(textdatum_t::middle_left);
    c.setTextColor(C_MUTED, C_CARD);
    c.drawString(String(i + 1), 8, ch / 2 - 1);
    c.setTextDatum(textdatum_t::middle_right);
    c.setTextColor(i == imx ? C_BAD : i == imn ? C_BLUE : C_TEXT, C_CARD);
    c.drawString(fmt(v, 3), cw - 8, ch / 2 - 1);
    c.pushSprite(12 + col * (cw + 6), 48 + row * (ch + 4));
  }
  c.deleteSprite();
  LGFX_Sprite b(&tft);
  b.setColorDepth(16);
  if (!b.createSprite(462, 44)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 44, 10, C_CARD);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  String line = n ? String("min ") + fmt(mn, 3) + " V (#" + (imn + 1) + ")   max " +
                        fmt(mx, 3) + " V (#" + (imx + 1) + ")   delta " +
                        String(int((mx - mn) * 1000 + .5f)) + " mV"
                  : String("Waiting for BMS data");
  b.drawString(line, 12, 22);
  b.pushSprite(9, 270);
  b.deleteSprite();
}

static void infoRow(int y, const char *k, const String &v) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(462, 26)) return;
  s.fillSprite(C_BG);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString(k, 4, 13);
  s.setTextColor(C_TEXT, C_BG);
  s.drawString(v, 150, 13);
  s.drawFastHLine(0, 25, 462, C_LINE);
  s.pushSprite(9, y);
  s.deleteSprite();
}

static void info(const DisplaySnap &d, bool) {
  int y = 48;
  infoRow(y, "BMS", d.detected.length() ? d.detected : String("not identified")); y += 28;
  infoRow(y, "Model", d.model.length() ? d.model : String("-")); y += 28;
  infoRow(y, "Bluetooth", d.mac.length() ? d.mac : String("auto")); y += 28;
  infoRow(y, "State", d.state); y += 28;
  infoRow(y, "Probe", d.probe); y += 28;
  infoRow(y, "Received", String(d.rxBytes) + " B, " + d.good + " frames"); y += 28;
  infoRow(y, "Wi-Fi", String("BATTLAB-JK  http://192.168.4.1  (") +
                          d.stations + " connected)"); y += 28;
  infoRow(y, "Firmware", d.firmware + "   up " + String(d.uptimeMs / 60000) + " min");
  y += 28;
  infoRow(y, "Settings", "web: /api/display?rotation=1&invert=0&rgb=0");
}

static void displayTask(void *) {
  int page = 0, drawnPage = -1;
  uint32_t lastDraw = 0, lastTouch = 0;
  bool wasDown = false;
  DisplaySnap d;
  for (;;) {
    uint32_t now = millis();
    lgfx::touch_point_t tp;
    bool down = tft.getTouch(&tp) > 0;
    if (down && !wasDown && now - lastTouch > 300) {
      page = (page + 1) % 3;
      lastTouch = now;
    }
    wasDown = down;
    if (page != drawnPage || now - lastDraw > 700) {
      bool force = page != drawnPage;
      if (force) {
        tft.fillRect(0, 40, 480, 280, C_BG);
        lastSocDrawn = -2;
      }
      takeDisplaySnapshot(d);
      header(d, page);
      if (page == 0) overview(d, force);
      else if (page == 1) cells(d, force);
      else info(d, force);
      drawnPage = page;
      lastDraw = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void displayStart() {
  Preferences p;
  p.begin("battlab-disp", true);
  uint8_t rotation = p.getUChar("rot", 1);
  bool invert = p.getBool("inv", false);
  bool rgb = p.getBool("rgb", false);
  p.end();
  tft.configure(invert, rgb);
  tft.init();
  tft.setRotation(rotation & 7);
  tft.setBrightness(200);
  C_BG = tft.color565(11, 17, 23);
  C_CARD = tft.color565(24, 33, 42);
  C_LINE = tft.color565(45, 58, 70);
  C_TEXT = tft.color565(236, 241, 244);
  C_MUTED = tft.color565(132, 147, 158);
  C_ACCENT = tft.color565(54, 196, 168);
  C_GOOD = tft.color565(64, 200, 120);
  C_WARN = tft.color565(232, 160, 64);
  C_BAD = tft.color565(232, 84, 88);
  C_BLUE = tft.color565(84, 148, 232);
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::FreeSansBold18pt7b);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("BATTLAB", 240, 140);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Battery Studio  -  starting", 240, 180);
  xTaskCreatePinnedToCore(displayTask, "disp", 6144, nullptr, 1, nullptr, 1);
}
#endif
