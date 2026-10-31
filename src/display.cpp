// On-board dashboard for the ESP32-3248S035 ("Cheap Yellow Display" 3.5").
// ST7796 480x320 over SPI + XPT2046 resistive touch. Fully standalone:
// tab bar for HOME / CELLS / BMS / LED / SETUP / INFO, BMS scan + connect and
// all settings on the screen, no browser needed.
#include "display.h"

#ifndef BATTLAB_DISPLAY
void displayStart() {}
#else
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <vector>

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

// Screen layout: header 0..40, content 44..276, tab bar 278..320.
static const int TAB_Y = 278, TAB_H = 42, TABS = 6;
enum Page { P_HOME, P_CELLS, P_BMS, P_LED, P_SETUP, P_INFO };
static const char *const TAB_NAMES[TABS] = {"HOME", "CELLS", "BMS",
                                            "LED",  "SETUP", "INFO"};

// --------------------------------------------------------- settings ----
struct DispCfg {
  uint8_t bl = 4;      // backlight level 1..5
  uint8_t sleep = 0;   // index into SLEEP_MIN
  uint8_t rot = 1;     // LovyanGFX rotation
  uint8_t lowSoc = 15; // %
  uint8_t hiTemp = 55; // deg C
  bool beep = true;
} dcfg;
static const uint8_t SLEEP_MIN[4] = {0, 1, 5, 10};
static const uint8_t BL_LEVEL[6] = {0, 40, 90, 140, 200, 255};

static void dcfgLoad() {
  Preferences p;
  p.begin("battlab-disp", true);
  dcfg.bl = p.getUChar("bl", 4);
  dcfg.sleep = p.getUChar("sleep", 0);
  dcfg.rot = p.getUChar("rot", 1) & 7;
  dcfg.lowSoc = p.getUChar("lowsoc", 15);
  dcfg.hiTemp = p.getUChar("hitemp", 55);
  dcfg.beep = p.getBool("beep", true);
  p.end();
  if (dcfg.bl < 1 || dcfg.bl > 5) dcfg.bl = 4;
  if (dcfg.sleep > 3) dcfg.sleep = 0;
}
static void dcfgSave() {
  Preferences p;
  p.begin("battlab-disp", false);
  p.putUChar("bl", dcfg.bl);
  p.putUChar("sleep", dcfg.sleep);
  p.putUChar("rot", dcfg.rot);
  p.putUChar("lowsoc", dcfg.lowSoc);
  p.putUChar("hitemp", dcfg.hiTemp);
  p.putBool("beep", dcfg.beep);
  p.end();
}

// ------------------------------------------------------------ helpers ----
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
  if (s == "SELECT_BMS") return "SELECT BMS";
  if (s == "UNCONFIGURED") return "NOT CONNECTED";
  if (s == "CONNECT_FAILED") return "CONNECT FAILED";
  if (s == "AUTO_DISCOVERY" || s == "AUTO_FOUND") return "AUTO FIND";
  if (s == "RECONFIGURING") return "CONNECTING";
  if (s == "GATT_UNSUPPORTED") return "NOT SUPPORTED";
  return s.c_str();
}
struct Btn {
  int x, y, w, h;
};
static bool hit(const Btn &b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}
static void button(const Btn &b, const String &label, bool selected,
                   uint16_t accent = 0, const lgfx::IFont *font = nullptr) {
  uint16_t bg = selected ? (accent ? accent : C_ACCENT) : C_CARD;
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  if (!selected) tft.drawRoundRect(b.x, b.y, b.w, b.h, 10, C_LINE);
  tft.setFont(font ? font : &fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(selected ? C_BG : C_TEXT, bg);
  tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2 + 1);
}
static void label(int x, int y, const char *t) {
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(t, x, y);
}
static void clearContent() { tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG); }

// Short on-screen message shown in the header for a few seconds.
static String toast;
static uint32_t toastUntil = 0;
static void showToast(const String &t, uint32_t ms = 2500) {
  toast = t;
  toastUntil = millis() + ms;
}

// ------------------------------------------------------------ alarms ----
static String alarmText;
static void evalAlarm(const DisplaySnap &d) {
  alarmText = "";
  if (!d.live) return;
  const auto &s = d.sample;
  float t = s.temp1;
  if (s.temp2 > t) t = s.temp2;
  if (s.mos > t) t = s.mos;
  if (dcfg.lowSoc && s.soc < dcfg.lowSoc)
    alarmText = String("LOW SOC ") + int(s.soc + .5f) + "%";
  else if (dcfg.hiTemp && t > dcfg.hiTemp)
    alarmText = String("HIGH TEMP ") + int(t + .5f) + "C";
}
static void beep(int times) {
  for (int i = 0; i < times; i++) {
    ledcWriteTone(3, 2300);
    vTaskDelay(pdMS_TO_TICKS(110));
    ledcWriteTone(3, 0);
    vTaskDelay(pdMS_TO_TICKS(90));
  }
}

// ------------------------------------------------------- header + tabs ----
static void header(const DisplaySnap &d) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(480, 40)) return;
  s.fillSprite(C_BG);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextColor(C_ACCENT, C_BG);
  s.setTextDatum(textdatum_t::middle_left);
  s.drawString("BATTLAB", 12, 20);
  uint16_t dot = d.live ? C_GOOD : d.linked ? C_WARN : C_BAD;
  s.fillCircle(150, 20, 6, dot);
  s.setFont(&fonts::FreeSans9pt7b);
  bool toastOn = millis() < toastUntil;
  if (toastOn) {
    s.setTextColor(C_TEXT, C_BG);
    s.drawString(toast, 164, 21);
  } else {
    String st = stateLabel(d.state);
    if (d.detected.length() && d.live) st = d.detected + "  " + st;
    s.setTextColor(C_TEXT, C_BG);
    s.drawString(st, 164, 21);
  }
  if (alarmText.length()) {
    int w = s.textWidth(alarmText) + 24;
    s.fillRoundRect(476 - w, 7, w, 26, 13, C_BAD);
    s.setTextColor(C_TEXT, C_BAD);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(alarmText, 476 - w / 2, 21);
  } else if (d.linked) {
    s.setTextDatum(textdatum_t::middle_right);
    s.setTextColor(C_MUTED, C_BG);
    s.drawString(String(d.rssi) + " dBm", 468, 21);
  }
  s.drawFastHLine(0, 39, 480, C_LINE);
  s.pushSprite(0, 0);
  s.deleteSprite();
}
static void tabs(int page) {
  tft.fillRect(0, TAB_Y, 480, TAB_H, C_CARD);
  tft.drawFastHLine(0, TAB_Y, 480, C_LINE);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < TABS; i++) {
    bool sel = i == page;
    if (sel) tft.fillRect(i * 80 + 8, TAB_Y + 1, 64, 3, C_ACCENT);
    tft.setTextColor(sel ? C_ACCENT : C_MUTED, C_CARD);
    tft.drawString(TAB_NAMES[i], i * 80 + 40, TAB_Y + TAB_H / 2 + 2);
  }
}

// --------------------------------------------------------------- HOME ----
static void tile(int x, int y, int w, int h, const char *lbl,
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
  s.drawString(lbl, 12, 5);
  s.setTextColor(C_TEXT, C_CARD);
  s.setFont(&fonts::FreeSansBold18pt7b);
  s.setTextDatum(textdatum_t::bottom_left);
  s.drawString(value, 12, h - 4);
  int vw = s.textWidth(value);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_CARD);
  s.drawString(unit, 16 + vw, h - 8);
  s.pushSprite(x, y);
  s.deleteSprite();
}
static int lastSocDrawn = -2;
static void socGauge(const DisplaySnap &d, bool force) {
  int soc = d.live ? int(d.sample.soc + .5f) : -1;
  if (!force && soc == lastSocDrawn) return;
  lastSocDrawn = soc;
  const int cx = 106, cy = 150, r1 = 86, r0 = 71;
  uint16_t col = soc < 0 ? C_LINE : soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
  tft.fillArc(cx, cy, r0, r1, 135, 405, C_LINE);
  if (soc > 0) tft.fillArc(cx, cy, r0, r1, 135, 135 + 270.f * soc / 100.f, col);
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(130, 84)) return;
  s.fillSprite(C_BG);
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_TEXT, C_BG);
  s.setFont(&fonts::FreeSansBold24pt7b);
  s.drawString(soc < 0 ? String("--") : String(soc) + "%", 65, 34);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString("SOC", 65, 72);
  s.pushSprite(cx - 65, cy - 38);
  s.deleteSprite();
}
static void cellStats(const jk::Sample &s, float &mn, float &mx, int &imn, int &imx) {
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
static void pill(LGFX_Sprite &s, int x, int y, const char *t, bool on) {
  uint16_t bg = on ? C_GOOD : C_LINE;
  s.fillRoundRect(x, y, 72, 26, 13, bg);
  s.setTextColor(on ? C_BG : C_MUTED, bg);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(t, x + 36, y + 14);
}
static void home(const DisplaySnap &d, bool force) {
  socGauge(d, force);
  const auto &s = d.sample;
  bool ok = d.live;
  int x0 = 214, x1 = 346, w = 126, h = 56;
  tile(x0, 46, w, h, "VOLTAGE", ok ? fmt(s.voltage, 2) : "--", "V", C_ACCENT);
  tile(x1, 46, w, h, "CURRENT", ok ? fmt(s.current, 1) : "--", "A",
       s.current < 0 ? C_WARN : C_GOOD);
  tile(x0, 106, w, h, "POWER", ok ? fmt(s.voltage * s.current, 0) : "--", "W", C_BLUE);
  tile(x1, 106, w, h, "REMAIN", ok ? fmt(s.capacity, 1) : "--", "Ah", C_ACCENT);
  tile(x0, 166, w, h, "TEMP", ok ? fmt(s.temp1, 0) : "--", "C", C_WARN);
  tile(x1, 166, w, h, "MOS", ok ? fmt(s.mos, 0) : "--", "C", C_WARN);
  LGFX_Sprite b(&tft);
  b.setColorDepth(16);
  if (!b.createSprite(462, 46)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 46, 10, C_CARD);
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  String line = ok && imn >= 0
      ? String(s.count) + "S  min " + fmt(mn, 3) + "  max " + fmt(mx, 3) +
            "  d " + String(int((mx - mn) * 1000 + .5f)) + "mV"
      : String(d.linked ? "Waiting for BMS data" : "Not connected - open BMS tab");
  b.drawString(line, 12, 23);
  pill(b, 304, 10, "CHG", ok && s.charge);
  pill(b, 382, 10, "DSG", ok && s.discharge);
  b.pushSprite(9, 228);
  b.deleteSprite();
}

// -------------------------------------------------------------- CELLS ----
static void cells(const DisplaySnap &d, bool) {
  const auto &s = d.sample;
  int n = d.live ? int(s.count) : 0;
  if (n > 32) n = 32;
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  int cols = n <= 16 ? 2 : 4;
  int rows = n ? (n + cols - 1) / cols : 1;
  int cw = 462 / cols - 6, ch = 178 / rows - 4;
  if (ch > 36) ch = 36;
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
  float lo = 2.8f, hi = 4.25f;
  if (mx > 0 && mx < 3.7f) lo = 2.5f, hi = 3.65f; // LiFePO4-like pack
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
    c.pushSprite(12 + col * (cw + 6), 46 + row * (ch + 4));
  }
  c.deleteSprite();
  LGFX_Sprite b(&tft);
  b.setColorDepth(16);
  if (!b.createSprite(462, 42)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 42, 10, C_CARD);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  String line = n ? String("min ") + fmt(mn, 3) + " (#" + (imn + 1) + ")  max " +
                        fmt(mx, 3) + " (#" + (imx + 1) + ")  delta " +
                        String(int((mx - mn) * 1000 + .5f)) + " mV"
                  : String("Waiting for BMS data");
  b.drawString(line, 12, 21);
  b.pushSprite(9, 230);
  b.deleteSprite();
}

// ---------------------------------------------------------------- BMS ----
struct Dev {
  String name, mac;
  int rssi = 0;
  uint8_t type = 0;
  bool candidate = false;
};
static std::vector<Dev> devs;
static String devsJson;
static int devPage = 0, protoSel = 0;
static const char *const PROTO_LABEL[5] = {"AUTO", "JK32", "JK24", "ANT", "JBD"};
static const char *const PROTO_VALUE[5] = {"AUTO", "JK02_32S", "JK02_24S", "ANT", "JBD"};
static const Btn B_AUTOFIND = {214, 46, 84, 40}, B_SCAN = {302, 46, 84, 40},
                 B_STOP = {390, 46, 84, 40}, B_UP = {430, 136, 44, 66},
                 B_DOWN = {430, 208, 44, 66};
static Btn protoBtn(int i) { return {96 + i * 76, 92, 70, 36}; }
static Btn devRow(int i) { return {12, 136 + i * 35, 412, 32}; }

static void parseDevices(const String &json) {
  if (json == devsJson) return;
  devsJson = json;
  devs.clear();
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, json)) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    Dev v;
    v.name = o["name"] | "";
    v.mac = o["mac"] | "";
    v.rssi = o["rssi"] | 0;
    v.type = o["type"] | 0;
    v.candidate = o["candidate"] | false;
    if (v.mac.length() == 17) devs.push_back(v);
  }
  std::sort(devs.begin(), devs.end(), [](const Dev &a, const Dev &b) {
    if (a.candidate != b.candidate) return a.candidate;
    return a.rssi > b.rssi;
  });
  devPage = 0;
}
static String bmsKey; // what was last drawn, to avoid needless redraws
static void bmsPage(const DisplaySnap &d, bool force) {
  parseDevices(d.devices);
  // status card (always refreshed)
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (s.createSprite(196, 40)) {
    s.fillSprite(C_BG);
    s.setFont(&fonts::FreeSansBold9pt7b);
    s.setTextDatum(textdatum_t::top_left);
    s.setTextColor(d.live ? C_GOOD : d.linked ? C_WARN : C_TEXT, C_BG);
    s.drawString(d.live && d.detected.length() ? d.detected + " LIVE"
                                               : String(stateLabel(d.state)), 0, 2);
    s.setFont(&fonts::FreeSans9pt7b);
    s.setTextColor(C_MUTED, C_BG);
    s.drawString(d.mac.length() ? d.mac : String(d.automatic ? "auto find" : "no BMS set"), 0, 22);
    s.pushSprite(12, 46);
    s.deleteSprite();
  }
  String key = d.devices + d.mac + (d.scanning ? "S" : "") + protoSel + "/" + devPage;
  if (!force && key == bmsKey) return;
  bmsKey = key;
  button(B_AUTOFIND, "AUTO", d.automatic && !d.mac.length());
  button(B_SCAN, d.scanning ? "..." : "SCAN", d.scanning);
  button(B_STOP, "STOP", false);
  tft.fillRect(12, 92, 84, 36, C_BG);
  label(12, 110, "Protocol");
  for (int i = 0; i < 5; i++) button(protoBtn(i), PROTO_LABEL[i], protoSel == i);
  tft.fillRect(12, 134, 462, TAB_Y - 136, C_BG);
  if (d.scanning) {
    label(20, 170, "Scanning for Bluetooth devices (about 4 s)...");
    return;
  }
  if (devs.empty()) {
    label(20, 160, "Tap SCAN to find your BMS, or AUTO to");
    label(20, 184, "connect to the only BMS nearby.");
    label(20, 220, "Close the BMS app on your phone first.");
    return;
  }
  int pages = (devs.size() + 3) / 4;
  if (devPage >= pages) devPage = pages - 1;
  for (int i = 0; i < 4; i++) {
    size_t k = devPage * 4 + i;
    if (k >= devs.size()) break;
    const Dev &v = devs[k];
    Btn r = devRow(i);
    bool cur = v.mac.equalsIgnoreCase(d.mac);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 8, C_CARD);
    if (cur) tft.drawRoundRect(r.x, r.y, r.w, r.h, 8, C_GOOD);
    if (v.candidate) tft.fillRoundRect(r.x, r.y + 6, 4, r.h - 12, 2, C_ACCENT);
    tft.setFont(v.candidate ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(C_TEXT, C_CARD);
    String nm = v.name.length() ? v.name : String("(no name)");
    if (nm.length() > 18) nm = nm.substring(0, 18);
    tft.drawString(nm, r.x + 12, r.y + r.h / 2 + 1);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(C_MUTED, C_CARD);
    tft.drawString(v.mac, r.x + 196, r.y + r.h / 2 + 1);
    tft.setTextDatum(textdatum_t::middle_right);
    tft.drawString(String(v.rssi), r.x + r.w - 10, r.y + r.h / 2 + 1);
  }
  if (pages > 1) {
    button(B_UP, "<", false, 0, &fonts::FreeSansBold12pt7b);
    button(B_DOWN, ">", false, 0, &fonts::FreeSansBold12pt7b);
    tft.setFont(&fonts::FreeSans9pt7b);
  }
}
static void bmsTouch(const DisplaySnap &d, int x, int y) {
  if (hit(B_SCAN, x, y)) {
    bmsScan();
    showToast("Scanning...");
  } else if (hit(B_AUTOFIND, x, y)) {
    bmsAuto();
    showToast("Auto find: looking for one BMS");
  } else if (hit(B_STOP, x, y)) {
    bmsDisconnect();
    showToast("Disconnected");
  } else if (hit(B_UP, x, y) && devs.size() > 4) {
    if (devPage > 0) devPage--;
  } else if (hit(B_DOWN, x, y) && devs.size() > 4) {
    if ((devPage + 1) * 4 < (int)devs.size()) devPage++;
  } else {
    for (int i = 0; i < 5; i++)
      if (hit(protoBtn(i), x, y)) {
        protoSel = i;
        return;
      }
    for (int i = 0; i < 4; i++) {
      size_t k = devPage * 4 + i;
      if (k < devs.size() && hit(devRow(i), x, y)) {
        const Dev &v = devs[k];
        if (bmsConnect(v.mac, v.type, PROTO_VALUE[protoSel]))
          showToast(String("Connecting ") + v.mac);
        return;
      }
    }
  }
}

// ---------------------------------------------------------------- LED ----
static const Btn B_POWER = {12, 48, 150, 52}, B_MINUS = {250, 48, 64, 52},
                 B_PLUS = {410, 48, 64, 52};
static Btn modeBtn(int i) { return {12 + i * 117, 110, 109, 46}; }
static Btn colorBtn(int i) { return {12 + i * 58, 166, 52, 52}; }
static uint16_t paletteColor(int i) {
  return tft.color565(LED_PALETTE[i][0], LED_PALETTE[i][1], LED_PALETTE[i][2]);
}
static LedCfg ledDrawn;
static bool ledDrawnValid = false;
static void ledPage(bool force) {
  LedCfg c = ledGet();
  if (!force && ledDrawnValid && c.on == ledDrawn.on && c.mode == ledDrawn.mode &&
      c.color == ledDrawn.color && c.level == ledDrawn.level)
    return;
  ledDrawn = c;
  ledDrawnValid = true;
  button(B_POWER, c.on ? "LED ON" : "LED OFF", c.on, c.on ? C_GOOD : 0,
         &fonts::FreeSansBold12pt7b);
  button(B_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  button(B_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  tft.fillRect(318, 48, 88, 52, C_BG);
  for (int i = 0; i < 4; i++) {
    int h = 12 + i * 10;
    tft.fillRoundRect(326 + i * 20, 96 - h, 14, h, 3, i < c.level && c.on ? C_ACCENT : C_LINE);
  }
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("bright", 206, 74);
  const char *names[LED_MODES] = {"RAINBOW", "SOLID", "BREATHE", "STATUS"};
  for (int i = 0; i < LED_MODES; i++) button(modeBtn(i), names[i], c.mode == i);
  bool colorUsed = c.mode == 1 || c.mode == 2;
  for (int i = 0; i < LED_COLORS; i++) {
    Btn b = colorBtn(i);
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    tft.fillRect(b.x, b.y, b.w, b.h, C_BG);
    if (c.color == i) tft.fillCircle(cx, cy, 25, C_TEXT);
    tft.fillCircle(cx, cy, 21, colorUsed ? paletteColor(i) : C_LINE);
    if (!colorUsed) tft.fillCircle(cx, cy, 8, paletteColor(i));
  }
  tft.fillRect(12, 226, 462, 48, C_BG);
  const char *hint = c.mode == 0 ? "Rainbow: colours cycle by themselves"
                   : c.mode == 3 ? "Green live, amber waiting, blue scanning, red no BMS"
                   : "Tap a colour to change it";
  label(14, 248, hint);
}
static void ledTouch(int x, int y) {
  LedCfg c = ledGet();
  if (hit(B_POWER, x, y)) c.on = !c.on;
  else if (hit(B_MINUS, x, y)) { if (c.level > 1) c.level--; c.on = true; }
  else if (hit(B_PLUS, x, y)) { if (c.level < 4) c.level++; c.on = true; }
  else {
    bool done = false;
    for (int i = 0; i < LED_MODES && !done; i++)
      if (hit(modeBtn(i), x, y)) { c.mode = i; c.on = true; done = true; }
    for (int i = 0; i < LED_COLORS && !done; i++)
      if (hit(colorBtn(i), x, y)) {
        c.color = i;
        c.on = true;
        if (c.mode == 0 || c.mode == 3) c.mode = 1;
        done = true;
      }
    if (!done) return;
  }
  ledSet(c);
}

// -------------------------------------------------------------- SETUP ----
static const Btn S_BL_MINUS = {150, 46, 52, 40}, S_BL_PLUS = {300, 46, 52, 40},
                 S_ROTATE = {370, 46, 104, 40}, S_WIFI = {150, 138, 120, 40},
                 S_CAL = {280, 138, 194, 40}, S_SOC_MINUS = {150, 184, 52, 40},
                 S_SOC_PLUS = {270, 184, 52, 40}, S_BEEP = {340, 184, 134, 40},
                 S_T_MINUS = {150, 230, 52, 40}, S_T_PLUS = {270, 230, 52, 40},
                 S_RESTART = {340, 230, 134, 40};
static Btn sleepBtn(int i) { return {150 + i * 82, 92, 76, 40}; }
static String setupKey;
static void valueBox(int x, int y, const String &v) {
  tft.fillRect(x, y, 64, 40, C_BG);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(v, x + 32, y + 21);
}
static void setupPage(const DisplaySnap &d, bool force) {
  String key = String(dcfg.bl) + dcfg.sleep + dcfg.rot + dcfg.lowSoc + "/" +
               dcfg.hiTemp + dcfg.beep + d.wifiOn;
  if (!force && key == setupKey) return;
  setupKey = key;
  clearContent();
  label(12, 66, "Screen");
  button(S_BL_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  button(S_BL_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  for (int i = 0; i < 5; i++) {
    int h = 10 + i * 6;
    tft.fillRoundRect(210 + i * 17, 84 - h, 12, h, 3, i < dcfg.bl ? C_ACCENT : C_LINE);
  }
  button(S_ROTATE, "ROTATE", false);
  label(12, 112, "Sleep");
  const char *sl[4] = {"OFF", "1 min", "5 min", "10 min"};
  for (int i = 0; i < 4; i++) button(sleepBtn(i), sl[i], dcfg.sleep == i);
  label(12, 158, "Wi-Fi");
  button(S_WIFI, d.wifiOn ? "WIFI ON" : "WIFI OFF", d.wifiOn, C_GOOD);
  button(S_CAL, "CALIBRATE TOUCH", false);
  label(12, 204, "Low SOC alarm");
  button(S_SOC_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  valueBox(204, 184, dcfg.lowSoc ? String(dcfg.lowSoc) + "%" : String("off"));
  button(S_SOC_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  button(S_BEEP, dcfg.beep ? "BEEP ON" : "BEEP OFF", dcfg.beep, C_GOOD);
  label(12, 250, "High temp alarm");
  button(S_T_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  valueBox(204, 230, dcfg.hiTemp ? String(dcfg.hiTemp) + "C" : String("off"));
  button(S_T_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  button(S_RESTART, "RESTART", false, 0);
}
static void applyBacklight() { tft.setBrightness(BL_LEVEL[dcfg.bl]); }
static bool wantCalibrate = false, wantRotate = false;
static void setupTouch(const DisplaySnap &d, int x, int y) {
  bool changed = true;
  if (hit(S_BL_MINUS, x, y)) { if (dcfg.bl > 1) dcfg.bl--; applyBacklight(); }
  else if (hit(S_BL_PLUS, x, y)) { if (dcfg.bl < 5) dcfg.bl++; applyBacklight(); }
  else if (hit(S_ROTATE, x, y)) { wantRotate = true; changed = false; }
  else if (hit(S_WIFI, x, y)) {
    wifiSetEnabled(!d.wifiOn);
    showToast(d.wifiOn ? "Wi-Fi hotspot off" : "Wi-Fi BATTLAB-JK on");
    changed = false;
  } else if (hit(S_CAL, x, y)) { wantCalibrate = true; changed = false; }
  else if (hit(S_SOC_MINUS, x, y)) dcfg.lowSoc = dcfg.lowSoc >= 5 ? dcfg.lowSoc - 5 : 0;
  else if (hit(S_SOC_PLUS, x, y)) dcfg.lowSoc = dcfg.lowSoc <= 45 ? dcfg.lowSoc + 5 : 50;
  else if (hit(S_T_MINUS, x, y)) dcfg.hiTemp = dcfg.hiTemp > 30 ? dcfg.hiTemp - 5 : 0;
  else if (hit(S_T_PLUS, x, y)) dcfg.hiTemp = dcfg.hiTemp == 0 ? 30 : dcfg.hiTemp < 90 ? dcfg.hiTemp + 5 : 90;
  else if (hit(S_BEEP, x, y)) { dcfg.beep = !dcfg.beep; if (dcfg.beep) beep(1); }
  else if (hit(S_RESTART, x, y)) {
    showToast("Restarting...");
    delay(300);
    ESP.restart();
  } else {
    changed = false;
    for (int i = 0; i < 4; i++)
      if (hit(sleepBtn(i), x, y)) { dcfg.sleep = i; changed = true; }
  }
  if (changed) dcfgSave();
}

// --------------------------------------------------------------- INFO ----
static void infoRow(int y, const char *k, const String &v) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(462, 27)) return;
  s.fillSprite(C_BG);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString(k, 4, 13);
  s.setTextColor(C_TEXT, C_BG);
  s.drawString(v, 130, 13);
  s.drawFastHLine(0, 26, 462, C_LINE);
  s.pushSprite(9, y);
  s.deleteSprite();
}
static void info(const DisplaySnap &d, bool) {
  int y = 44;
  infoRow(y, "BMS", d.detected.length() ? d.detected : String("not identified")); y += 29;
  infoRow(y, "Model", d.model.length() ? d.model : String("-")); y += 29;
  infoRow(y, "Bluetooth", (d.mac.length() ? d.mac : String(d.automatic ? "auto find" : "none")) +
                              "  " + d.protocol); y += 29;
  infoRow(y, "State", d.state); y += 29;
  infoRow(y, "Probe", d.probe); y += 29;
  infoRow(y, "Received", String(d.rxBytes) + " B, " + d.good + " frames"); y += 29;
  infoRow(y, "Wi-Fi", d.wifiOn ? String("BATTLAB-JK  192.168.4.1  (") + d.stations + ")"
                               : String("off")); y += 29;
  infoRow(y, "Firmware", d.firmware + "   up " + String(d.uptimeMs / 60000) + " min");
}

// ------------------------------------------------------ touch calibrate --
static void calibrate() {
  uint16_t cal[8];
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("Touch calibration", 240, 130);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("Touch the tip of each corner arrow, one by one", 240, 170);
  tft.calibrateTouch(cal, C_ACCENT, C_BG, 24);
  tft.setTouchCalibrate(cal);
  Preferences p;
  p.begin("battlab-disp", false);
  p.putBytes("cal", cal, sizeof cal);
  p.end();
  tft.fillScreen(C_BG);
}

// --------------------------------------------------------------- task ----
static void displayTask(void *) {
  {
    uint16_t cal[8];
    Preferences p;
    p.begin("battlab-disp", true);
    bool have = p.getBytes("cal", cal, sizeof cal) == sizeof cal;
    p.end();
    if (have) tft.setTouchCalibrate(cal);
    else calibrate();
  }
  ledcSetup(3, 2000, 8);
  ledcAttachPin(26, 3);
  ledcWriteTone(3, 0);
  int page = P_HOME, drawnPage = -1;
  uint32_t lastDraw = 0, lastTouch = 0, lastActivity = millis();
  bool wasDown = false, asleep = false, alarmWas = false;
  DisplaySnap d;
  for (;;) {
    uint32_t now = millis();
    lgfx::touch_point_t tp;
    bool down = tft.getTouch(&tp) > 0;
    if (down && !wasDown && now - lastTouch > 220) {
      lastTouch = now;
      lastActivity = now;
      if (asleep) {
        asleep = false; // first touch only wakes the screen
        applyBacklight();
      } else if (tp.y >= TAB_Y) {
        int t = tp.x / 80;
        if (t >= 0 && t < TABS) page = t;
      } else if (tp.y > 42) {
        if (page == P_LED) ledTouch(tp.x, tp.y);
        else if (page == P_BMS) bmsTouch(d, tp.x, tp.y);
        else if (page == P_SETUP) setupTouch(d, tp.x, tp.y);
        lastDraw = 0; // show the change right away
      }
    }
    wasDown = down;
    if (wantCalibrate || wantRotate) {
      if (wantRotate) {
        dcfg.rot = dcfg.rot == 1 ? 3 : 1;
        dcfgSave();
        tft.setRotation(dcfg.rot);
      }
      calibrate(); // touch mapping changes with rotation
      wantCalibrate = wantRotate = false;
      drawnPage = -1;
    }
    // screen sleep
    uint32_t sleepMs = uint32_t(SLEEP_MIN[dcfg.sleep]) * 60000u;
    if (!asleep && sleepMs && now - lastActivity > sleepMs && !alarmText.length()) {
      asleep = true;
      tft.setBrightness(0);
    }
    if (page != drawnPage || now - lastDraw > 700) {
      bool force = page != drawnPage;
      takeDisplaySnapshot(d);
      evalAlarm(d);
      bool alarmNow = alarmText.length() > 0;
      if (alarmNow && !alarmWas) {
        lastActivity = now;
        if (asleep) { asleep = false; applyBacklight(); }
        if (dcfg.beep) beep(3);
      }
      alarmWas = alarmNow;
      if (force) {
        clearContent();
        tabs(page);
        lastSocDrawn = -2;
        ledDrawnValid = false;
        bmsKey = "";
        setupKey = "";
        if (page == P_BMS) {
          for (int i = 0; i < 5; i++)
            if (d.protocol == PROTO_VALUE[i]) protoSel = i;
        }
      }
      if (!asleep) {
        header(d);
        switch (page) {
          case P_HOME: home(d, force); break;
          case P_CELLS: cells(d, force); break;
          case P_BMS: bmsPage(d, force); break;
          case P_LED: ledPage(force); break;
          case P_SETUP: setupPage(d, force); break;
          default: info(d, force); break;
        }
      }
      drawnPage = page;
      lastDraw = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void displayStart() {
  dcfgLoad();
  Preferences p;
  p.begin("battlab-disp", true);
  bool invert = p.getBool("inv", false);
  bool rgb = p.getBool("rgb", false);
  p.end();
  tft.configure(invert, rgb);
  tft.init();
  tft.setRotation(dcfg.rot);
  applyBacklight();
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
  xTaskCreatePinnedToCore(displayTask, "disp", 10240, nullptr, 1, nullptr, 1);
}
#endif
