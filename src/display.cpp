// On-board dashboard for the ESP32-3248S035 ("Cheap Yellow Display" 3.5").
// ST7796 480x320 over SPI + XPT2046 resistive touch. Fully standalone:
// tab bar for HOME / CELLS / BMS / LED / SETUP / INFO, BMS scan + connect and
// all settings on the screen, no browser needed.
#include "display.h"

#ifndef BATTLAB_DISPLAY
void displayStart() {}
bool gfxHold(uint32_t) { return true; }
void gfxRelease() {}
void displayCfgGet(JsonObject) {}
const char *displayCfgSet(JsonObjectConst) { return "this board has no screen"; }
#else
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "logo.h"
#include "lux_dongle.h"
#include "inv_dongle.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <vector>

#ifdef BATTLAB_ES3C40
#include "lgfx_es3c40.h"
#else
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
#endif
#ifndef BATTLAB_SCREEN_INV_DEFAULT
#define BATTLAB_SCREEN_INV_DEFAULT false
#endif

static LGFX tft;
static uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_MUTED, C_ACCENT, C_GOOD, C_WARN,
    C_BAD, C_BLUE;

// Screen layout: header 0..40, content 44..276, tab bar 278..320.
// An 8th tab (INV, LuxPower inverter read from its Wi-Fi dongle) appears when the dongle reader is on.
static const int TAB_Y = 278, TAB_H = 42, TABS_MAX = 8;
enum Page { P_HOME, P_CELLS, P_BMS, P_PARAM, P_LED, P_SETUP, P_INFO, P_INV };
static const char *const TAB_NAMES[TABS_MAX] = {"HOME", "CELLS", "BMS", "PARAM",
                                                "LED",  "SETUP", "INFO", "INV"};
static int tabCount() { return (luxd::enabled() || invd::enabled()) ? 8 : 7; }
static int tabW() { return tabCount() == 8 ? 60 : 68; }

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

// Remote (cloud) access to the screen settings. Values are applied by the
// display task (wantDispApply) so only that task touches the panel.
static volatile bool wantDispApply = false;
void displayCfgGet(JsonObject o) {
  o["bl"] = dcfg.bl;
  o["sleep"] = SLEEP_MIN[dcfg.sleep < 4 ? dcfg.sleep : 0];
  o["lowsoc"] = dcfg.lowSoc;
  o["hitemp"] = dcfg.hiTemp;
  o["beep"] = dcfg.beep;
  o["rot"] = dcfg.rot;
}
const char *displayCfgSet(JsonObjectConst a) {
  DispCfg n = dcfg;
  if (a.containsKey("bl")) { int v = a["bl"]; if (v < 1 || v > 5) return "brightness 1-5"; n.bl = v; }
  if (a.containsKey("sleep")) {
    int v = a["sleep"], k = -1;
    for (int i = 0; i < 4; i++) if (SLEEP_MIN[i] == v) k = i;
    if (k < 0) return "sleep 0/1/5/10 min";
    n.sleep = k;
  }
  if (a.containsKey("lowsoc")) { int v = a["lowsoc"]; if (v < 0 || v > 50) return "low SOC 0-50"; n.lowSoc = v; }
  if (a.containsKey("hitemp")) { int v = a["hitemp"]; if (v != 0 && (v < 30 || v > 90)) return "high temp 0 or 30-90"; n.hiTemp = v; }
  if (a.containsKey("beep")) n.beep = a["beep"].as<bool>();
  dcfg = n;
  wantDispApply = true;
  return nullptr;
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
static bool isEv(const DisplaySnap &d) { return d.prof == 1 || d.prof == 2; }
static float evRangeKm(const DisplaySnap &d) { // remaining energy / consumption; <0 = unknown
  if (!d.live || !d.whkm || d.sample.capacity <= 0) return -1;
  return d.sample.capacity * d.sample.voltage / d.whkm;
}
static uint32_t regenUntil = 0;
static float regenKw = 0;
static void evalAlarm(const DisplaySnap &d) {
  alarmText = "";
  if (!d.live) return;
  const auto &s = d.sample;
  float rk = isEv(d) ? evRangeKm(d) : -1;
  if (rk >= 0 && d.rangeAlarm && rk < d.rangeAlarm) { alarmText = String("RANGE LOW ") + int(rk + .5f) + "km"; return; }
  float t = s.temp1;
  if (s.temp2 > t) t = s.temp2;
  if (s.mos > t) t = s.mos;
  if (dcfg.lowSoc && s.soc < dcfg.lowSoc)
    alarmText = String("LOW SOC ") + int(s.soc + .5f) + "%";
  else if (dcfg.hiTemp && t > dcfg.hiTemp)
    alarmText = String("HIGH TEMP ") + int(t + .5f) + "C";
}
static void beep(int times) {
#ifdef BATTLAB_ES3C40
  (void)times;
  return; // speaker on this board is an I2S codec (ES8311): no buzzer pin
#endif
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
  s.setColorDepth(8);
  if (!s.createSprite(480, 40)) return;
  s.fillSprite(C_BG);
  // BattLab logo (same artwork as the boot splash), white on the header bg
  {
    static lgfx::rgb565_t line[LOGO_S_W];
    uint8_t br = 11, bgc = 17, bb = 23; // C_BG components
    for (int y = 0; y < LOGO_S_H; y++) {
      const uint8_t *row = LOGO_S_GRAY + size_t(y) * LOGO_S_W;
      for (int x = 0; x < LOGO_S_W; x++) {
        uint8_t v = row[x];
        line[x] = lgfx::rgb565_t(br + (255 - br) * v / 255, bgc + (255 - bgc) * v / 255,
                                 bb + (255 - bb) * v / 255);
      }
      s.pushImage(8, 5 + y, LOGO_S_W, 1, line);
    }
  }
  s.setTextDatum(textdatum_t::middle_left);
  uint16_t dot = d.live ? C_GOOD : d.linked ? C_WARN : C_BAD;
  s.fillCircle(124, 20, 6, dot);
  s.setFont(&fonts::FreeSans9pt7b);
  bool toastOn = millis() < toastUntil;
  if (toastOn) {
    s.setTextColor(C_TEXT, C_BG);
    s.drawString(toast, 138, 21);
  } else {
    String st = stateLabel(d.state);
    if (d.detected.length() && d.live) st = d.detected + "  " + st;
    s.setTextColor(C_TEXT, C_BG);
    s.drawString(st, 138, 21);
  }
  if (millis() < regenUntil && !alarmText.length()) { // EV braking puts energy back
    String r = String("REGEN +") + fmt(regenKw, 1) + " kW";
    s.setFont(&fonts::FreeSansBold9pt7b);
    int w = s.textWidth(r) + 24;
    s.fillRoundRect(476 - w, 7, w, 26, 13, C_GOOD);
    s.setTextColor(C_BG, C_GOOD);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(r, 476 - w / 2, 21);
  } else if (alarmText.length()) {
    int w = s.textWidth(alarmText) + 24;
    s.fillRoundRect(476 - w, 7, w, 26, 13, C_BAD);
    s.setTextColor(C_TEXT, C_BAD);
    s.setTextDatum(textdatum_t::middle_center);
    s.drawString(alarmText, 476 - w / 2, 21);
  } else {
    String r;
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm t;
      localtime_r(&now, &t);
      char hhmm[8];
      snprintf(hhmm, sizeof hhmm, "%02d:%02d", t.tm_hour, t.tm_min);
      s.setTextDatum(textdatum_t::middle_right);
      s.setFont(&fonts::FreeSansBold9pt7b);
      s.setTextColor(C_TEXT, C_BG);
      s.drawString(hhmm, 470, 21);
      r = "  ";
      s.setFont(&fonts::FreeSans9pt7b);
      if (d.linked) {
        s.setTextColor(C_MUTED, C_BG);
        s.drawString(String(d.rssi) + "dBm", 470 - 58, 21);
      }
    } else if (d.linked) {
      s.setTextDatum(textdatum_t::middle_right);
      s.setTextColor(C_MUTED, C_BG);
      s.drawString(String(d.rssi) + " dBm", 468, 21);
    }
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
  const int TABS = tabCount(), TAB_W = tabW();
  for (int i = 0; i < TABS; i++) {
    bool sel = i == page;
    if (sel) tft.fillRect(i * TAB_W + 6, TAB_Y + 1, TAB_W - 12, 3, C_ACCENT);
    tft.setTextColor(sel ? C_ACCENT : C_MUTED, C_CARD);
    tft.drawString(TAB_NAMES[i], i * TAB_W + TAB_W / 2 + (i == TABS - 1 ? 2 : 0),
                   TAB_Y + TAB_H / 2 + 2);
  }
}


// ------------------------------------------------------- INV (LuxPower) ----
// Inverter(s) read straight from the LuxPower Wi-Fi dongle. Two inverters
// (parallel system) are added together, with one line per inverter below.
static void tile(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit, uint16_t accent);
static uint16_t blend565(uint16_t a, uint16_t b, float k);
static void invPage(bool force) {
  luxd::View v[2];
  int n = luxd::views(v, 2);
  if (n < 2) n += invd::views(v + n, 2 - n);
  bool other = !luxd::enabled() && invd::enabled();
  if (!n) {
    static char lastH[72] = {1};
    char hh[72] = {0};
    if (!other) luxd::hint(hh, sizeof hh);
    if (!force && !strcmp(hh, lastH)) return;
    memcpy(lastH, hh, sizeof hh);
    clearContent();
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString(other ? "Inverter" : "LuxPower inverter", 240, 120);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(other ? "Waiting for the Wi-Fi dongle" : "Waiting for the Wi-Fi dongle (port 8000)", 240, 160);
    const char *h = hh;
    if (h[0]) tft.setTextColor(C_WARN, C_BG);
    tft.drawString(h[0] ? h : "Set its IP in the web page: Settings", 240, 186);
    if (h[0]) { tft.setTextColor(C_MUTED, C_BG); tft.drawString("or set its IP in the web page: Settings", 240, 212); }
    return;
  }
  float soc = 0, vb = 0, p = 0, ppv = 0, pl = 0, pg = 0, epv = 0, el = 0;
  bool fresh = false;
  for (int i = 0; i < n; i++) {
    soc += v[i].soc / n; vb += v[i].v / n; p += v[i].p; ppv += v[i].ppv;
    pl += v[i].pload; pg += v[i].pgrid; epv += v[i].epv; el += v[i].eload;
    fresh |= v[i].fresh;
  }
  // battery card (left)
  LGFX_Sprite b(&tft);
  b.setColorDepth(16);
  if (b.createSprite(142, 176)) {
    b.fillSprite(C_BG);
    b.fillRoundRect(0, 0, 142, 176, 12, C_CARD);
    uint16_t col = soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
    b.setFont(&fonts::FreeSans9pt7b);
    b.setTextDatum(textdatum_t::top_left);
    b.setTextColor(C_MUTED, C_CARD);
    b.drawString(n > 1 ? "BATTERY x2" : "BATTERY", 12, 8);
    // battery outline with fill
    b.drawRoundRect(14, 34, 104, 46, 8, col);
    b.drawRoundRect(15, 35, 102, 44, 7, col);
    b.fillRoundRect(118, 49, 6, 16, 2, col);
    int w = int(96 * std::max(0.f, std::min(100.f, soc)) / 100.f);
    if (w > 0) b.fillRoundRect(18, 38, std::max(w, 6), 38, 5, col);
    // moving light while charging / discharging
    if (fabsf(p) > 30 && w > 12) {
      int pos = int(millis() / 25 % 120) - 20;
      if (p < 0) pos = 116 - pos;
      for (int k = -6; k <= 6; k++) {
        int x = 18 + pos + k;
        if (x >= 18 && x < 18 + w) b.drawFastVLine(x, 38, 38, blend565(col, C_TEXT, 0.45f - fabsf(k) * 0.07f));
      }
    }
    b.setFont(&fonts::FreeSansBold18pt7b);
    b.setTextDatum(textdatum_t::middle_center);
    b.setTextColor(C_TEXT, C_CARD);
    b.drawString(String(int(soc + .5f)) + "%", 71, 108);
    b.setFont(&fonts::FreeSans9pt7b);
    b.setTextColor(C_MUTED, C_CARD);
    b.drawString(fmt(vb, 1) + " V", 71, 138);
    b.setTextColor(p > 30 ? C_GOOD : p < -30 ? C_WARN : C_MUTED, C_CARD);
    b.drawString(fabsf(p) < 30 ? String("idle") : (p > 0 ? "+" : "-") + fmt(fabsf(p), 0) + " W", 71, 160);
    b.pushSprite(9, 46);
    b.deleteSprite();
  }
  // tiles (right)
  int x0 = 158, x1 = 316, w = 156, h = 56;
  tile(x0, 46, w, h, "SOLAR", fmt(ppv, 0), "W", C_WARN);
  tile(x1, 46, w, h, "HOME", fmt(pl, 0), "W", C_BLUE);
  tile(x0, 106, w, h, pg >= 0 ? "GRID IN" : "GRID OUT", fmt(fabsf(pg), 0), "W", C_MUTED);
  tile(x1, 106, w, h, "BATTERY", fmt(fabsf(p), 0), "W", p >= 0 ? C_GOOD : C_WARN);
  tile(x0, 166, w, h, "PV TODAY", fmt(epv, 1), "kWh", C_WARN);
  tile(x1, 166, w, h, "LOAD TODAY", fmt(el, 1), "kWh", C_BLUE);
  // one line per inverter
  LGFX_Sprite s(&tft);
  s.setColorDepth(8);
  if (!s.createSprite(462, 46)) return;
  s.fillSprite(C_BG);
  s.fillRoundRect(0, 0, 462, 46, 10, C_CARD);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  for (int i = 0; i < n; i++) {
    int y = n > 1 ? 13 + i * 20 : 23;
    s.setTextColor(v[i].fresh ? C_TEXT : C_BAD, C_CARD);
    s.drawString(String(v[i].sn + 6) + "  " + String(int(v[i].soc)) + "%  PV " + fmt(v[i].ppv, 0) +
                     "W  Load " + fmt(v[i].pload, 0) + "W", 12, y);
  }
  s.setTextDatum(textdatum_t::middle_right);
  s.setTextColor(fresh ? C_ACCENT : C_BAD, C_CARD);
  s.drawString(fresh ? "DONGLE LIVE" : "NO DATA", 450, 23);
  s.pushSprite(9, 228);
  s.deleteSprite();
}
// --------------------------------------------------------------- HOME ----
static void tileDraw(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit,
                     uint16_t accent, const String &old, int roll);
static void tile(int x, int y, int w, int h, const char *lbl,
                 const String &value, const char *unit, uint16_t accent) {
  // remember the last value per tile position; roll the number when it changes
  static int keys[12];
  static String vals[12];
  static int nk = 0;
  int key = x * 1000 + y, slot = -1;
  for (int i = 0; i < nk; i++) if (keys[i] == key) slot = i;
  if (slot < 0 && nk < 12) { slot = nk++; keys[slot] = key; vals[slot] = ""; }
  String old = slot >= 0 ? vals[slot] : String("");
  if (slot >= 0) vals[slot] = value;
  if (old.length() && old != value && value != "--" && old != "--") {
    for (int r = 1; r <= 4; r++) tileDraw(x, y, w, h, lbl, value, unit, accent, old, r);
  } else tileDraw(x, y, w, h, lbl, value, unit, accent, "", 0);
}
static void tileDraw(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit,
                     uint16_t accent, const String &old, int roll) {
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
  if (roll > 0) { // old value slides up and out, new one comes in from below
    int step = (h - 22) / 4;
    s.setClipRect(6, 22, w - 6, h - 22);
    s.drawString(old, 12, h - 4 - roll * step);
    s.drawString(value, 12, h - 4 + (4 - roll) * step);
    s.clearClipRect();
  } else s.drawString(value, 12, h - 4);
  int vw = s.textWidth(value);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_CARD);
  s.drawString(unit, 16 + vw, h - 8);
  s.pushSprite(x, y);
  s.deleteSprite();
}
static int lastSocDrawn = -2;
static uint32_t gaugeAnimAt = 0; // sweep 0 -> SOC when the HOME page opens
static int gaugeTarget = -1;
static void socGaugeDraw(int soc);
static void socGauge(const DisplaySnap &d, bool force) {
  int soc = d.live ? int(d.sample.soc + .5f) : -1;
  if (force && soc > 0) { gaugeAnimAt = millis(); gaugeTarget = soc; lastSocDrawn = soc; socGaugeDraw(0); return; }
  if (gaugeAnimAt) { gaugeTarget = soc; return; } // the sweep is running
  if (!force && soc == lastSocDrawn) return;
  lastSocDrawn = soc;
  socGaugeDraw(soc);
}
static void gaugeTick() {
  if (!gaugeAnimAt) return;
  uint32_t t = millis() - gaugeAnimAt;
  float k = std::min(1.f, t / 650.f);
  k = 1 - (1 - k) * (1 - k) * (1 - k);
  socGaugeDraw(int(gaugeTarget * k + .5f));
  if (t >= 650) { gaugeAnimAt = 0; lastSocDrawn = gaugeTarget; socGaugeDraw(gaugeTarget); }
}
static void socGaugeDraw(int soc) {
  const int cx = 106, cy = 150, r1 = 86, r0 = 71;
  uint16_t col = soc < 0 ? C_LINE : soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
  tft.fillArc(cx, cy, r0, r1, 135, 405, C_LINE);
  if (soc > 0) tft.fillArc(cx, cy, r0, r1, 135, 135 + 270.f * soc / 100.f, col);
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(130, 72)) return;
  s.fillSprite(C_BG);
  s.setTextDatum(textdatum_t::middle_center);
  s.setTextColor(C_TEXT, C_BG);
  s.setFont(&fonts::FreeSansBold24pt7b);
  s.drawString(soc < 0 ? String("--") : String(soc) + "%", 65, 34);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString("SOC", 65, 63);
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
// 10 minutes of power / SOC (one point every 5 s) for the HOME sparkline
static const int HIST_N = 120;
static float histP[HIST_N], histSoc[HIST_N];
static int histCount = 0, histHead = 0;
static uint32_t histAt = 0;
static void histPush(const DisplaySnap &d) {
  if (!d.live || millis() - histAt < 5000) return;
  histAt = millis();
  histP[histHead] = d.sample.voltage * d.sample.current;
  histSoc[histHead] = d.sample.soc;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}
static String etaText(const jk::Sample &s) {
  float full = s.full > 0 ? s.full : s.soc > 3 ? s.capacity / (s.soc / 100.f) : 0;
  float h = -1;
  const char *w = "";
  if (s.current > 0.3f && full > 0) { h = (full - s.capacity) / s.current; w = "full in "; }
  else if (s.current < -0.3f && s.capacity > 0) { h = s.capacity / -s.current; w = "empty in "; }
  if (h < 0 || h > 999) return "";
  int m = int(h * 60 + .5f);
  return String(w) + (m < 60 ? String(m) + "m" : String(m / 60) + "h" + (m % 60 < 10 ? "0" : "") + String(m % 60));
}
static bool bigMode = false;
static void bigHome(const DisplaySnap &d) {
  const auto &s = d.sample;
  bool ok = d.live;
  int soc = ok ? int(s.soc + .5f) : -1;
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  if (!b.createSprite(480, 236)) return;
  b.fillSprite(C_BG);
  uint16_t col = soc < 0 ? C_LINE : soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
  b.setTextDatum(textdatum_t::middle_center);
  b.setFont(&fonts::FreeSansBold24pt7b);
  b.setTextSize(2.4f);
  b.setTextColor(C_TEXT, C_BG);
  b.drawString(soc < 0 ? String("--") : String(soc) + "%", 240, 74);
  b.setTextSize(1);
  b.fillRoundRect(40, 132, 400, 14, 7, C_LINE);
  if (soc > 0) b.fillRoundRect(40, 132, 4 * std::min(soc, 100), 14, 7, col);
  b.setFont(&fonts::FreeSansBold18pt7b);
  b.setTextColor(s.current < 0 ? C_WARN : C_GOOD, C_BG);
  b.drawString(ok ? fmt(s.voltage * s.current, 0) + " W" : String("--"), 130, 186);
  b.setTextColor(C_TEXT, C_BG);
  b.drawString(ok ? fmt(s.voltage, 1) + " V" : String("--"), 350, 186);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextColor(C_MUTED, C_BG);
  b.drawString(ok ? etaText(s) + (etaText(s).length() ? "   " : "") + fmt(s.current, 1) + " A   " + fmt(s.temp1, 0) + " C"
                  : String("Waiting for BMS data"), 240, 222);
  b.pushSprite(0, 41);
  b.deleteSprite();
}
static void home(const DisplaySnap &d, bool force) {
  if (bigMode) {
    static uint32_t lastBig = 0;
    if (force || millis() - lastBig > 900) { lastBig = millis(); bigHome(d); }
    return;
  }
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
  b.setColorDepth(8);
  if (!b.createSprite(462, 46)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 46, 10, C_CARD);
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  if (ok && imn >= 0) {
    b.drawString(String(s.count) + "S  d" + String(int((mx - mn) * 1000 + .5f)) + "mV", 12, 14);
    String e = etaText(s);
    b.setTextColor(C_MUTED, C_CARD);
    // second line alternates: time left <-> energy today (kWh, counted on the board)
    bool showE = (millis() / 4000) % 2 && (d.eIn > 0.005f || d.eOut > 0.005f);
    b.drawString(showE ? String("+") + fmt(d.eIn, 2) + "/-" + fmt(d.eOut, 2) + "kWh" : e.length() ? e : String(s.cycles) + " cycles", 12, 33);
  } else {
    b.drawString(d.linked ? "Waiting for BMS data" : "Not connected - open BMS tab", 12, 23);
  }
  // power sparkline (last 10 min)
  if (ok && histCount > 1) {
    const int sx = 150, sw = 140, sy = 6, sh = 34;
    float lo = 0, hi = 0;
    for (int k = 0; k < histCount; k++) {
      float v = histP[(histHead - histCount + k + HIST_N) % HIST_N];
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (hi - lo < 1) hi = lo + 1;
    int zy = sy + int(sh * hi / (hi - lo));
    b.drawFastHLine(sx, zy, sw, C_LINE);
    int px = -1, py = 0;
    for (int k = 0; k < histCount; k++) {
      float v = histP[(histHead - histCount + k + HIST_N) % HIST_N];
      int x = sx + sw - (histCount - 1 - k) * sw / (HIST_N - 1);
      int y = sy + int(sh * (hi - v) / (hi - lo));
      if (px >= 0) b.drawLine(px, py, x, y, v < 0 ? C_WARN : C_GOOD);
      px = x;
      py = y;
    }
    b.setTextDatum(textdatum_t::top_left);
    b.setTextColor(C_MUTED, C_CARD);
    b.setFont(&fonts::Font0);
    b.drawString("10 min W", sx, 1);
    b.setFont(&fonts::FreeSans9pt7b);
  }
  pill(b, 304, 10, "CHG", ok && s.charge);
  pill(b, 382, 10, "DSG", ok && s.discharge);
  b.pushSprite(9, 228);
  b.deleteSprite();
}
// under the SOC number: pulsing bolt + chevrons with a moving light
// (up = charging, down = discharging)
static uint16_t blend565(uint16_t a, uint16_t b, float k) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return uint16_t(int(ar + (br - ar) * k) << 11 | int(ag + (bg - ag) * k) << 5 | int(ab + (bb - ab) * k));
}
static void flowAnim(const DisplaySnap &d) {
  if (bigMode) return;
  static uint32_t last = 0;
  if (millis() - last < 60) return;
  last = millis();
  int dir = !d.live ? 0 : d.sample.current > 0.3f ? 1 : d.sample.current < -0.3f ? -1 : 0;
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(96, 20)) return;
  s.fillSprite(C_BG);
  if (dir == 0) s.fillRoundRect(24, 8, 48, 4, 2, C_LINE);
  else {
    uint16_t on = dir > 0 ? C_GOOD : C_WARN;
    float t = millis() / 1000.f;
    // bolt (charging) / drop (discharging), gently pulsing
    float pul = .55f + .45f * (0.5f + 0.5f * sinf(t * 6.f));
    uint16_t ic = blend565(C_BG, dir > 0 ? C_WARN : C_WARN, pul);
    if (dir > 0) { s.fillTriangle(10, 1, 3, 11, 9, 11, ic); s.fillTriangle(9, 9, 15, 9, 7, 19, ic); }
    else { s.fillCircle(9, 12, 6, ic); s.fillTriangle(3, 11, 15, 11, 9, 1, ic); }
    for (int k = 0; k < 4; k++) {
      int idx = dir > 0 ? 3 - k : k;
      float ph = fmodf(t * 1.6f - idx * .22f, 1.f);
      if (ph < 0) ph += 1;
      float a = .2f + .8f * std::max(0.f, sinf(ph * 3.14159f));
      uint16_t c = blend565(C_LINE, on, a);
      int x = 24 + k * 18;
      if (dir > 0) s.fillTriangle(x, 16, x + 14, 16, x + 7, 4, c);
      else s.fillTriangle(x, 4, x + 14, 4, x + 7, 16, c);
    }
  }
  s.pushSprite(106 - 48, 150 + 36);
  s.deleteSprite();
}

// ----------------------------------------------------------- EV HOME ----
// Motorbike / car profile (layout C): a symmetric power gauge (idle at top,
// regen sweeps left in green, drive sweeps right in orange) with the live kW in
// the middle, plus RANGE / SOC / VOLT / CURRENT tiles. The needle eases so the
// motion stays smooth instead of jumping.
static float evShown = 0, evPeak = 3000;
static float evTarget = 0;
static const int EV_CX = 120, EV_CY = 182, EV_R0 = 90, EV_R1 = 110;
static void evMeter(float p) { // p in W: + = driving, - = regen
  tft.fillArc(EV_CX, EV_CY, EV_R0, EV_R1, 150, 390, C_LINE);          // 240deg track, gap at the bottom
  if (p > 1) tft.fillArc(EV_CX, EV_CY, EV_R0, EV_R1, 270, 270 + 120 * std::min(1.f, p / evPeak), C_WARN);
  else if (p < -1) tft.fillArc(EV_CX, EV_CY, EV_R0, EV_R1, 270 - 120 * std::min(1.f, -p / (evPeak * .4f)), 270, C_GOOD);
  LGFX_Sprite t(&tft);
  t.setColorDepth(8);
  if (!t.createSprite(150, 70)) return;
  t.fillSprite(C_BG);
  t.setTextDatum(textdatum_t::middle_center);
  t.setFont(&fonts::FreeSansBold24pt7b);
  t.setTextColor(p < -1 ? C_GOOD : C_TEXT, C_BG);
  t.drawString(fmt(std::fabs(p) / 1000.f, 1), 75, 26);
  t.setFont(&fonts::FreeSans9pt7b);
  t.setTextColor(C_MUTED, C_BG);
  t.drawString(p < -1 ? "kW regen" : p > 1 ? "kW drive" : "kW", 75, 58);
  t.pushSprite(EV_CX - 75, EV_CY - 35);
  t.deleteSprite();
}
// one data tile inside the right-hand sprite
static void evTile(LGFX_Sprite &b, int x, int y, int w, int h, const char *label,
                   const String &val, const char *unit, uint16_t accent, bool big) {
  b.fillRoundRect(x, y, w, h, 10, C_CARD);
  b.fillRoundRect(x, y + 10, 4, h - 20, 2, accent);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::top_left);
  b.setTextColor(C_MUTED, C_CARD);
  b.drawString(label, x + 12, y + 8);
  b.setFont(big ? &fonts::FreeSansBold24pt7b : &fonts::FreeSansBold18pt7b);
  b.setTextDatum(textdatum_t::bottom_left);
  b.setTextColor(C_TEXT, C_CARD);
  b.drawString(val, x + 12, y + h - 8);
  int vw = b.textWidth(val);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextColor(C_MUTED, C_CARD);
  b.drawString(unit, x + 16 + vw, y + h - 12);
}
static void evHome(const DisplaySnap &d, bool force) {
  const auto &s = d.sample;
  bool ok = d.live;
  evTarget = ok ? -(s.voltage * s.current) : 0;       // + = drive, - = regen
  evPeak = std::max(evPeak, std::fabs(evTarget));
  if (force) { evShown = 0; evMeter(0); }
  float rk = evRangeKm(d);
  bool low = rk >= 0 && d.rangeAlarm && rk < d.rangeAlarm;
  int soc = ok ? int(s.soc + .5f) : 0;
  // right-hand 2x2 tiles: RANGE / SOC / VOLT / CURRENT (screen x 250..472)
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  if (!b.createSprite(224, 188)) return;
  b.fillSprite(C_BG);
  evTile(b, 2, 0, 110, 90, "RANGE", rk < 0 ? String("--") : String(int(rk + .5f)), "km", low ? C_BAD : C_ACCENT, true);
  evTile(b, 116, 0, 108, 90, "SOC", ok ? String(soc) : String("--"), "%", C_WARN, false);
  evTile(b, 2, 96, 110, 90, "VOLT", ok ? fmt(s.voltage, 1) : String("--"), "V", C_BLUE, false);
  evTile(b, 116, 96, 108, 90, "CURRENT", ok ? fmt(s.current, 1) : String("--"), "A", (ok && s.current > .5f) ? C_GOOD : C_WARN, false);
  b.pushSprite(248, 46);
  b.deleteSprite();
  // footer: pack summary + CHG/DSG
  LGFX_Sprite st(&tft);
  st.setColorDepth(8);
  if (!st.createSprite(462, 40)) return;
  st.fillSprite(C_BG);
  st.fillRoundRect(0, 0, 462, 40, 10, C_CARD);
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  st.setFont(&fonts::FreeSans9pt7b);
  st.setTextDatum(textdatum_t::middle_left);
  st.setTextColor(C_TEXT, C_CARD);
  st.drawString(ok && imn >= 0 ? String(s.count) + "S  d" + String(int((mx - mn) * 1000 + .5f)) + "mV  " + fmt(s.temp1, 0) + "C" + (d.whkm ? String("  ") + d.whkm + " Wh/km" : String(""))
                               : String(d.linked ? "Waiting for BMS data" : "Not connected - open BMS tab"), 12, 20);
  pill(st, 304, 7, "CHG", ok && s.charge);
  pill(st, 382, 7, "DSG", ok && s.discharge);
  st.pushSprite(9, 234);
  st.deleteSprite();
  // regen: current flows back in shortly after driving
  static uint32_t lastDrive = 0;
  if (ok && s.current < -1) lastDrive = millis();
  if (ok && s.current > .5f && millis() - lastDrive < 8000) { regenUntil = millis() + 1500; regenKw = s.voltage * s.current / 1000; }
}
static void evTick() { // ease the needle toward the latest power
  static uint32_t last = 0;
  if (millis() - last < 60) return;
  last = millis();
  float n = evShown + (evTarget - evShown) * .35f;
  if (std::fabs(n - evShown) < 5) return;
  evShown = n;
  evMeter(evShown);
}
// tab change: a quick wipe in the direction of travel
static void pageWipe(int dir) {
  for (int k = 1; k <= 4; k++) {
    int x = dir > 0 ? 480 - k * 120 : (k - 1) * 120;
    tft.fillRect(x, 41, 120, TAB_Y - 41, C_BG);
    tft.fillRect(dir > 0 ? x : x + 117, 41, 3, TAB_Y - 41, C_ACCENT);
    vTaskDelay(pdMS_TO_TICKS(16));
  }
  clearContent();
}
// a new alarm: banner slides into the header, the screen edge blinks red 3 times
static void alarmFlash() {
  tft.setFont(&fonts::FreeSansBold9pt7b);
  int w = tft.textWidth(alarmText) + 24;
  for (int k = 0; k <= 4; k++) {
    int y = -28 + k * 9;
    tft.fillRect(476 - w, 0, w, 40, C_BG);
    tft.fillRoundRect(476 - w, y, w, 26, 13, C_BAD);
    tft.setTextColor(C_TEXT, C_BAD);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(alarmText, 476 - w / 2, y + 14);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  for (int i = 0; i < 6; i++) {
    uint16_t c = i % 2 ? C_BG : C_BAD;
    for (int k = 0; k < 4; k++) tft.drawRect(k, k, 480 - 2 * k, 320 - 2 * k, c);
    vTaskDelay(pdMS_TO_TICKS(110));
  }
}

// -------------------------------------------------------------- CELLS ----
static void cells(const DisplaySnap &d, bool force) {
  const auto &s = d.sample;
  int n = d.live ? int(s.count) : 0;
  if (n > 32) n = 32;
  float mn, mx;
  int imn, imx;
  cellStats(s, mn, mx, imn, imx);
  // Up to 16 cells: 2 columns. 17-24: 4 columns. 25-32: dense 4x8 grid that
  // uses more height, with a slim one-line summary underneath.
  bool dense = n > 24;
  static int lastLayout = -1;
  int layout = n <= 16 ? 0 : dense ? 2 : 1;
  if (layout != lastLayout) {
    tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG); // cell count changed
    lastLayout = layout;
  }
  int cols = n <= 16 ? 2 : 4;
  int rows = n ? (n + cols - 1) / cols : 1;
  int gap = dense ? 3 : 4, gridH = dense ? 204 : 178;
  int cw = 462 / cols - 6, ch = gridH / rows - gap;
  if (ch > 36) ch = 36;
  int barH = ch < 28 ? 3 : 5;
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
  // on page open the bars grow one after another (6 frames)
  for (int fr = force && n ? 1 : 6; fr <= 6; fr++) {
  int idx = 0;
  for (int i = 0; i < 32 && idx < n; i++) {
    float v = s.cells[i];
    if (v <= 0) continue;
    int col = idx % cols, row = idx / cols;
    float grow = std::min(1.f, std::max(0.f, fr / 6.f * 1.5f - idx * .5f / n));
    idx++;
    c.fillSprite(C_BG);
    c.fillRoundRect(0, 0, cw, ch, 6, C_CARD);
    float f = (v - lo) / (hi - lo);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    f *= grow;
    uint16_t bar = i == imx ? C_BAD : i == imn ? C_BLUE : C_GOOD;
    c.fillRoundRect(0, ch - barH, int(cw * f), barH, 1, bar);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(textdatum_t::middle_left);
    c.setTextColor(C_MUTED, C_CARD);
    c.drawString(String(i + 1), 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.setTextDatum(textdatum_t::middle_right);
    c.setTextColor(i == imx ? C_BAD : i == imn ? C_BLUE : C_TEXT, C_CARD);
    c.drawString(fmt(v, 3), cw - 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.pushSprite(12 + col * (cw + 6), 46 + row * (ch + gap));
  }
  }
  c.deleteSprite();
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  int sh = dense ? 24 : 42, sy = dense ? 252 : 230;
  if (!b.createSprite(462, sh)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, sh, dense ? 8 : 10, C_CARD);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  String line = n ? String("min ") + fmt(mn, 3) + " (#" + (imn + 1) + ")  max " +
                        fmt(mx, 3) + " (#" + (imx + 1) + ")  delta " +
                        String(int((mx - mn) * 1000 + .5f)) + " mV"
                  : String("Waiting for BMS data");
  b.drawString(line, 12, sh / 2);
  b.pushSprite(9, sy);
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
static const char *const PROTO_LABEL[6] = {"AUTO", "JK32", "JK24", "ANT", "JBD", "DALY"};
static const char *const PROTO_VALUE[6] = {"AUTO", "JK02_32S", "JK02_24S", "ANT", "JBD", "DALY"};
static const Btn B_AUTOFIND = {214, 46, 84, 40}, B_SCAN = {302, 46, 84, 40},
                 B_STOP = {390, 46, 84, 40}, B_UP = {430, 136, 44, 66},
                 B_DOWN = {430, 208, 44, 66};
static Btn protoBtn(int i) { return {96 + i * 64, 92, 60, 36}; }
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
  for (int i = 0; i < 6; i++) button(protoBtn(i), PROTO_LABEL[i], protoSel == i);
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
    for (int i = 0; i < 6; i++)
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
                 S_ROTATE = {370, 46, 104, 40}, S_WIFI = {150, 138, 104, 40},
                 S_TX = {260, 138, 96, 40}, S_CAL = {362, 138, 112, 40}, S_SOC_MINUS = {150, 184, 52, 40},
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
               dcfg.hiTemp + dcfg.beep + d.wifiOn + (d.radioMax ? "M" : "L");
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
  label(12, 158, "Radio");
  button(S_WIFI, d.wifiOn ? "WIFI ON" : "WIFI OFF", d.wifiOn, C_GOOD);
  button(S_TX, d.radioMax ? "TX MAX" : "TX LOW", d.radioMax, C_GOOD);
#ifndef BATTLAB_TOUCH_CAPACITIVE
  button(S_CAL, "CALIBRATE", false);
#endif
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
    showToast(d.wifiOn ? "Wi-Fi hotspot off" : "Wi-Fi BATTLAB on");
    changed = false;
  } else if (hit(S_TX, x, y)) {
    radioSetMaxPower(!d.radioMax);
    showToast(d.radioMax ? "Radio power LOW (weak power supply)" : "Radio power MAX");
    changed = false;
  } 
#ifndef BATTLAB_TOUCH_CAPACITIVE
  else if (hit(S_CAL, x, y)) { wantCalibrate = true; changed = false; }
#endif
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
  s.setColorDepth(8);
  if (!s.createSprite(462, 25)) return;
  s.fillSprite(C_BG);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(C_MUTED, C_BG);
  s.drawString(k, 4, 12);
  s.setTextColor(C_TEXT, C_BG);
  s.drawString(v, 130, 12);
  s.drawFastHLine(0, 24, 462, C_LINE);
  s.pushSprite(9, y);
  s.deleteSprite();
}
static const Btn I_WIFI = {12, 224, 224, 46}, I_CLOUD = {250, 224, 224, 46};
static String infoKey;
static void info(const DisplaySnap &d, bool force) {
  NetInfo n = netInfo();
  int y = 44;
  infoRow(y, "Hardware ID", n.name + "   " + n.hw); y += 25;
  infoRow(y, "BMS", (d.detected.length() ? d.detected : String("not identified")) +
                        (d.model.length() ? "   " + d.model : String(""))); y += 25;
  infoRow(y, "Bluetooth", (d.mac.length() ? d.mac : String(d.automatic ? "auto find" : "none")) +
                              "  " + d.protocol + "  TX " + d.bleTx); y += 25;
  infoRow(y, "State", d.state + "   rx " + d.rxBytes + " B, " + d.good + " frames"); y += 25;
  infoRow(y, "Home Wi-Fi", !n.ssid.length() ? String("not set")
                           : n.connected ? n.ssid + "  " + n.ip + "  " + n.rssi + " dBm"
                                         : n.ssid + "  connecting..."); y += 25;
  infoRow(y, "Cloud", n.cloud + (n.sent ? String("   sent ") + n.sent : String(""))); y += 25;
  infoRow(y, "System", d.firmware + "  " + d.resetReason + "  RAM " + (d.freeHeap / 1024) + "/" + (d.maxBlock / 1024) + "k");
  String key = String(n.connected ? "C" : "-") + n.paired;
  if (!force && key == infoKey) return;
  infoKey = key;
  button(I_WIFI, n.connected ? "HOME WI-FI  OK" : "HOME WI-FI", n.connected, C_GOOD);
  button(I_CLOUD, n.paired ? "CLOUD  PAIRED" : "CLOUD PAIRING", n.paired, C_GOOD);
}

// ----------------------------------------------- network overlays (INFO) --
enum Overlay { OV_NONE, OV_WIFI, OV_KB, OV_CLOUD };
static Overlay overlay = OV_NONE;
static String ovKey;
struct Net {
  String ssid;
  int rssi;
  bool lock;
};
static std::vector<Net> nets;
static int netPage = 0;
static String kbSsid, kbText;
static uint8_t kbLayer = 0; // 0 lower, 1 upper, 2 symbols, 3 symbols 2
static const char *const KB_ROWS[4][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
    {"1234567890", "!@#$%^&*()", "-_=+[]{};", ":'\".,?/"},
    {"1234567890", "~`|\\<>!@#$", "%^&*()-_=", "+;:'\",."}};
static const Btn W_SCAN = {302, 46, 84, 40}, W_BACK = {390, 46, 84, 40},
                 W_FORGET = {12, 240, 140, 34}, W_UP = {430, 94, 44, 68},
                 W_DOWN = {430, 168, 44, 68};
static Btn netRow(int i) { return {12, 94 + i * 36, 412, 32}; }
static Btn kbKey(int row, int i) {
  static const int x0[4] = {6, 6, 29, 78};
  return {x0[row] + i * 47, 108 + row * 42, 43, 38};
}
static const Btn K_SHIFT = {6, 234, 66, 38}, K_DEL = {410, 234, 64, 38},
                 K_SYM = {6, 278, 72, 38}, K_SPACE = {82, 278, 184, 38},
                 K_CANCEL = {270, 278, 92, 38}, K_OK = {366, 278, 108, 38},
                 C_BACK = {390, 46, 84, 40};

static void parseNets(const String &json) {
  nets.clear();
  DynamicJsonDocument d(4096);
  if (deserializeJson(d, json)) return;
  for (JsonObject o : d.as<JsonArray>()) nets.push_back({o["ssid"] | "", o["rssi"] | 0, o["lock"] | true});
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  netPage = 0;
}
static void kbDrawText() {
  tft.fillRoundRect(6, 64, 468, 36, 8, C_CARD);
  tft.setFont(&fonts::FreeSans12pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_TEXT, C_CARD);
  String t = kbText;
  while (t.length() && tft.textWidth(t) > 440) t = t.substring(1); // show the tail
  tft.drawString(t + "_", 16, 83);
}
static void kbDraw() {
  tft.fillRect(0, 41, 480, 320 - 41, C_BG);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(String("Password for  ") + kbSsid, 8, 52);
  kbDrawText();
  for (int r = 0; r < 4; r++) {
    const char *row = KB_ROWS[kbLayer][r];
    for (int i = 0; row[i]; i++) button(kbKey(r, i), String(row[i]), false, 0, &fonts::FreeSans12pt7b);
  }
  button(K_SHIFT, kbLayer >= 2 ? (kbLayer == 3 ? "#+= " : "#+=") : (kbLayer ? "abc" : "ABC"), kbLayer == 1 || kbLayer == 3);
  button(K_DEL, "DEL", false);
  button(K_SYM, kbLayer >= 2 ? "abc" : "?123", false);
  button(K_SPACE, "space", false);
  button(K_CANCEL, "CANCEL", false);
  button(K_OK, "CONNECT", true, C_GOOD);
}
static void openKeyboard(const String &ssid) {
  kbSsid = ssid;
  kbText = "";
  kbLayer = 0;
  overlay = OV_KB;
  kbDraw();
}
static void wifiOverlayDraw(bool force) {
  NetInfo n = netInfo();
  String scan = netScanJson();
  String key = scan + n.ssid + n.connected + n.ip + netPage;
  if (!force && key == ovKey) return;
  ovKey = key;
  if (scan.length()) parseNets(scan);
  tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG);
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(n.connected ? C_GOOD : C_TEXT, C_BG);
  tft.drawString(n.connected ? "Connected: " + n.ssid : String(n.ssid.length() ? "Connecting: " + n.ssid : "Home Wi-Fi not set"), 12, 48);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(n.connected ? n.ip + "   " + n.rssi + " dBm" : String("Pick your network below"), 12, 68);
  button(W_SCAN, scan.length() ? "SCAN" : "...", !scan.length());
  button(W_BACK, "BACK", false);
  if (!scan.length()) {
    label(20, 140, "Scanning for Wi-Fi networks...");
  } else if (nets.empty()) {
    label(20, 140, "No networks found. Tap SCAN.");
  } else {
    int pages = (nets.size() + 3) / 4;
    if (netPage >= pages) netPage = pages - 1;
    for (int i = 0; i < 4; i++) {
      size_t k = netPage * 4 + i;
      if (k >= nets.size()) break;
      Btn r = netRow(i);
      bool cur = nets[k].ssid == n.ssid;
      tft.fillRoundRect(r.x, r.y, r.w, r.h, 8, C_CARD);
      if (cur) tft.drawRoundRect(r.x, r.y, r.w, r.h, 8, n.connected ? C_GOOD : C_WARN);
      tft.setFont(&fonts::FreeSansBold9pt7b);
      tft.setTextDatum(textdatum_t::middle_left);
      tft.setTextColor(C_TEXT, C_CARD);
      String nm = nets[k].ssid;
      if (nm.length() > 26) nm = nm.substring(0, 26);
      tft.drawString(nm, r.x + 12, r.y + 17);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextDatum(textdatum_t::middle_right);
      tft.setTextColor(C_MUTED, C_CARD);
      tft.drawString(String(nets[k].lock ? "lock  " : "open  ") + nets[k].rssi, r.x + r.w - 10, r.y + 17);
    }
    if (pages > 1) {
      button(W_UP, "<", false, 0, &fonts::FreeSansBold12pt7b);
      button(W_DOWN, ">", false, 0, &fonts::FreeSansBold12pt7b);
    }
  }
  if (n.ssid.length()) button(W_FORGET, "FORGET", false);
}
static void cloudOverlayDraw(bool force) {
  NetInfo n = netInfo();
  String key = n.cloud + n.paired + n.connected + n.sent;
  if (!force && key == ovKey) return;
  ovKey = key;
  tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG);
  button(C_BACK, "BACK", false);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("PAIR CODE", 12, 52);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(n.pair, 12, 104);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  if (!n.configured) {
    tft.setTextColor(C_WARN, C_BG);
    tft.drawString("Cloud is not set up in this firmware build.", 12, 160);
    return;
  }
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(String("1. On your phone open  ") + n.site, 12, 150);
  tft.drawString("2. Sign in and tap  Add device", 12, 176);
  tft.drawString("3. Enter the pair code above", 12, 202);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(String("Hotspot + Bluetooth name: ") + n.name, 12, 224);
  tft.setTextColor(C_TEXT, C_BG);
  uint16_t sc = n.paired ? C_GOOD : n.cloud.startsWith("online") ? C_WARN : C_BAD;
  tft.fillCircle(18, 252, 6, sc);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(String("Cloud: ") + n.cloud, 32, 253);
}
// returns true when the overlay wants the page underneath repainted
static bool overlayTouch(int x, int y) {
  if (overlay == OV_CLOUD) {
    if (hit(C_BACK, x, y)) { overlay = OV_NONE; return true; }
    return false;
  }
  if (overlay == OV_WIFI) {
    if (hit(W_BACK, x, y)) { overlay = OV_NONE; return true; }
    if (hit(W_SCAN, x, y)) { netScanStart(); ovKey = ""; return false; }
    if (hit(W_FORGET, x, y)) { netForgetHomeWifi(); showToast("Home Wi-Fi removed"); ovKey = ""; return false; }
    if (nets.size() > 4 && hit(W_UP, x, y)) { if (netPage > 0) netPage--; ovKey = ""; return false; }
    if (nets.size() > 4 && hit(W_DOWN, x, y)) { if ((netPage + 1) * 4 < (int)nets.size()) netPage++; ovKey = ""; return false; }
    for (int i = 0; i < 4; i++) {
      size_t k = netPage * 4 + i;
      if (k < nets.size() && hit(netRow(i), x, y)) {
        if (nets[k].lock) openKeyboard(nets[k].ssid);
        else { netSetHomeWifi(nets[k].ssid, ""); showToast("Connecting to " + nets[k].ssid); ovKey = ""; }
        return false;
      }
    }
    return false;
  }
  if (overlay == OV_KB) {
    for (int r = 0; r < 4; r++) {
      const char *row = KB_ROWS[kbLayer][r];
      for (int i = 0; row[i]; i++)
        if (hit(kbKey(r, i), x, y)) {
          if (kbText.length() < 64) kbText += row[i];
          if (kbLayer == 1) { kbLayer = 0; kbDraw(); } // one-shot shift
          else kbDrawText();
          return false;
        }
    }
    if (hit(K_DEL, x, y)) { if (kbText.length()) kbText.remove(kbText.length() - 1); kbDrawText(); }
    else if (hit(K_SPACE, x, y)) { if (kbText.length() < 64) kbText += ' '; kbDrawText(); }
    else if (hit(K_SHIFT, x, y)) { kbLayer = kbLayer == 0 ? 1 : kbLayer == 1 ? 0 : kbLayer == 2 ? 3 : 2; kbDraw(); }
    else if (hit(K_SYM, x, y)) { kbLayer = kbLayer >= 2 ? 0 : 2; kbDraw(); }
    else if (hit(K_CANCEL, x, y)) { overlay = OV_WIFI; ovKey = ""; return true; }
    else if (hit(K_OK, x, y)) {
      netSetHomeWifi(kbSsid, kbText);
      kbText = "";
      showToast("Connecting to " + kbSsid, 4000);
      overlay = OV_WIFI;
      ovKey = "";
      return true;
    }
    return false;
  }
  return false;
}
static void infoTouch(int x, int y) {
  if (hit(I_WIFI, x, y)) {
    overlay = OV_WIFI;
    ovKey = "";
    if (!netScanJson().length() || nets.empty()) netScanStart();
  } else if (hit(I_CLOUD, x, y)) {
    overlay = OV_CLOUD;
    ovKey = "";
  }
}

// ------------------------------------------------------ touch calibrate --
static void calibrate() {
#ifdef BATTLAB_TOUCH_CAPACITIVE
  return; // capacitive touch is factory-calibrated
#endif
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

// -------------------------------------------------------------- PARAM ----
// JK protection settings with -/+ and SET, plus MOSFET switches for every
// BMS family. Every write goes through a confirmation dialog.
static const int PARAM_ROWS = 5;
static int paramPage = 0;
static float pending[bset::MAX_N];
static bool pendingSet[bset::MAX_N];
static uint8_t pendingFam = 0;
static String paramKey, lastWriteShown;
// One parameter view for every BMS: JK (bms_params.h) or ANT / JBD (bms_settings.h)
struct PInfo {
  const char *name, *unit;
  float step, min, max;
  uint8_t dec;
};
static int pCount(const DisplaySnap &d) {
  if (d.family == 1) return bmsw::JK_PARAM_COUNT;
  int n;
  bset::table(d.family, n);
  return n;
}
static int pPages(const DisplaySnap &d) { return std::max(1, (pCount(d) + PARAM_ROWS - 1) / PARAM_ROWS); }
static PInfo pInfo(const DisplaySnap &d, int i) {
  if (d.family == 1) {
    const auto &p = bmsw::JK_PARAMS[i];
    return {p.name, p.unit, p.step, p.min, p.max, p.decimals};
  }
  int n;
  const bset::Def &p = bset::table(d.family, n)[i];
  return {p.name, p.unit, p.step, p.min, p.max, p.dec};
}
static bool pReady(const DisplaySnap &d) { return d.family == 1 ? d.jkFresh : (d.family == 2 || d.family == 3) && d.pvHave; }
static bool pHas(const DisplaySnap &d, int i) { return d.family == 1 || (d.pvHave >> i & 1); }
static float pVal(const DisplaySnap &d, int i) { return d.family == 1 ? d.jk.v[i] : d.pv[i]; }
static const char *pCheck(const DisplaySnap &d, int i, float v) {
  return d.family == 1 ? bmsw::checkJkValue(d.jk, i, v) : bset::check(d.family, d.pv, d.pvHave, i, v);
}
static Btn rowMinus(int i) { return {292, 46 + i * 39, 50, 35}; }
static Btn rowPlus(int i) { return {348, 46 + i * 39, 50, 35}; }
static Btn rowSet(int i) { return {404, 46 + i * 39, 70, 35}; }
static Btn swBtn(int i) { return {12 + i * 102, 242, 96, 34}; }
static const Btn P_PREV = {322, 242, 56, 34}, P_NEXT = {418, 242, 56, 34};
static const char *const SW_NAMES[bmsw::SW_COUNT] = {"CHG", "DSG", "BAL"};

// confirmation dialog
struct Confirm {
  bool active = false, isSwitch = false, on = false;
  int idx = 0;
  float value = 0;
  String line1, line2;
} confirmDlg;
static const Btn C_CANCEL = {70, 196, 160, 46}, C_OK = {250, 196, 160, 46};
static void drawConfirm() {
  tft.fillRoundRect(40, 62, 400, 196, 14, C_CARD);
  tft.drawRoundRect(40, 62, 400, 196, 14, C_WARN);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_WARN, C_CARD);
  tft.drawString("Write to BMS?", 240, 90);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.drawString(confirmDlg.line1, 240, 130);
  tft.setTextColor(C_MUTED, C_CARD);
  tft.drawString(confirmDlg.line2, 240, 160);
  button(C_CANCEL, "CANCEL", false);
  button(C_OK, "WRITE", true, C_WARN);
}
static bool switchState(const DisplaySnap &d, int i) {
  if (d.family == 1) return d.jk.sw[i];
  if (i == 0) return d.sample.charge;
  if (i == 1) return d.sample.discharge;
  return false;
}
static bool switchAvailable(const DisplaySnap &d, int i) {
  if (!d.linked || !d.family) return false;
  if (d.family == 1) return d.jkFresh;
  return i < 2 && d.live; // ANT / JBD: charge + discharge from live data
}
static String fmtParam(const DisplaySnap &d, int i, float v) {
  PInfo p = pInfo(d, i);
  return fmt(v, p.dec) + " " + p.unit;
}
static void paramPageDraw(const DisplaySnap &d, bool force) {
  if (d.lastWrite.length() && d.lastWrite != lastWriteShown) {
    lastWriteShown = d.lastWrite;
    showToast(d.lastWrite, 5000);
  }
  if (pendingFam != d.family) { // another BMS: forget edits
    pendingFam = d.family;
    for (auto &p : pendingSet) p = false;
    paramPage = 0;
  }
  bool ready = pReady(d);
  int n = pCount(d), pages = pPages(d);
  if (paramPage >= pages) paramPage = 0;
  String key = String(d.family) + (ready ? "J" : "-") + paramPage + (d.linked ? "L" : "-") + (d.live ? "V" : "-");
  for (int i = 0; i < bmsw::SW_COUNT; i++) key += switchState(d, i) ? '1' : '0';
  if (ready)
    for (int i = 0; i < n; i++) key += (pHas(d, i) ? fmt(pVal(d, i), 3) : String("?")) + (pendingSet[i] ? fmt(pending[i], 3) : String("-"));
  if (!force && key == paramKey) return;
  paramKey = key;
  tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG);
  if (!ready) {
    label(16, 70, !d.linked ? "Connect a BMS on the BMS tab first."
                : d.family ? "Reading settings from the BMS..."
                : "Identifying the BMS type...");
    label(16, 98, d.family == 2 ? "ANT: settings are read one by one (~8 s)"
                : d.family == 3 ? "JBD: settings are read one by one (~4 s)" : "");
    label(16, 140, "All writes ask for confirmation first.");
  } else {
    for (int r = 0; r < PARAM_ROWS; r++) {
      int i = paramPage * PARAM_ROWS + r;
      if (i >= n) break;
      PInfo p = pInfo(d, i);
      int y = 46 + r * 39;
      tft.fillRoundRect(12, y, 274, 35, 8, C_CARD);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextDatum(textdatum_t::middle_left);
      tft.setTextColor(C_MUTED, C_CARD);
      String nm = p.name;
      while (nm.length() && tft.textWidth(nm) > 150) nm.remove(nm.length() - 1);
      tft.drawString(nm, 22, y + 18);
      bool have = pHas(d, i);
      bool edited = have && pendingSet[i] && std::fabs(pending[i] - pVal(d, i)) > p.step / 2;
      tft.setFont(&fonts::FreeSansBold9pt7b);
      tft.setTextDatum(textdatum_t::middle_right);
      tft.setTextColor(edited ? C_WARN : have ? C_TEXT : C_MUTED, C_CARD);
      tft.drawString(have ? fmtParam(d, i, edited ? pending[i] : pVal(d, i)) : String("not read"), 278, y + 18);
      button(rowMinus(r), "-", false, 0, &fonts::FreeSansBold12pt7b);
      button(rowPlus(r), "+", false, 0, &fonts::FreeSansBold12pt7b);
      button(rowSet(r), "SET", edited, C_WARN);
    }
    button(P_PREV, "<", false, 0, &fonts::FreeSansBold12pt7b);
    button(P_NEXT, ">", false, 0, &fonts::FreeSansBold12pt7b);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(String(paramPage + 1) + "/" + pages, 398, 260);
  }
  for (int i = 0; i < bmsw::SW_COUNT; i++) {
    if (!switchAvailable(d, i)) continue;
    bool on = switchState(d, i);
    button(swBtn(i), String(SW_NAMES[i]) + (on ? " ON" : " OFF"), on, C_GOOD);
  }
}
static bool paramRepeatable(int x, int y) {
  for (int r = 0; r < PARAM_ROWS; r++)
    if (hit(rowMinus(r), x, y) || hit(rowPlus(r), x, y)) return true;
  return false;
}
static void paramTouch(const DisplaySnap &d, int x, int y) {
  bool ready = pReady(d);
  int n = pCount(d), pages = pPages(d);
  if (ready && hit(P_PREV, x, y)) { paramPage = (paramPage + pages - 1) % pages; return; }
  if (ready && hit(P_NEXT, x, y)) { paramPage = (paramPage + 1) % pages; return; }
  for (int i = 0; i < bmsw::SW_COUNT; i++)
    if (switchAvailable(d, i) && hit(swBtn(i), x, y)) {
      bool on = !switchState(d, i);
      const char *what[3] = {"Charge MOSFET", "Discharge MOSFET", "Balancer"};
      confirmDlg = Confirm();
      confirmDlg.active = true;
      confirmDlg.isSwitch = true;
      confirmDlg.idx = i;
      confirmDlg.on = on;
      confirmDlg.line1 = String(what[i]) + (on ? "  ->  ON" : "  ->  OFF");
      confirmDlg.line2 = i == 1 && !on ? String("Discharge OFF cuts power to the load!")
                                       : String("BMS: ") + (d.detected.length() ? d.detected : String("?"));
      drawConfirm();
      return;
    }
  if (!ready) return;
  for (int r = 0; r < PARAM_ROWS; r++) {
    int i = paramPage * PARAM_ROWS + r;
    if (i >= n || !pHas(d, i)) continue;
    PInfo p = pInfo(d, i);
    float cur = pendingSet[i] ? pending[i] : pVal(d, i);
    if (hit(rowMinus(r), x, y) || hit(rowPlus(r), x, y)) {
      float v = cur + (hit(rowPlus(r), x, y) ? p.step : -p.step);
      v = std::round(v / p.step) * p.step;
      if (v < p.min) v = p.min;
      if (v > p.max) v = p.max;
      pending[i] = v;
      pendingSet[i] = true;
      return;
    }
    if (hit(rowSet(r), x, y) && pendingSet[i] && std::fabs(pending[i] - pVal(d, i)) > p.step / 2) {
      if (const char *e = pCheck(d, i, pending[i])) {
        showToast(String("Not allowed: ") + e, 4000);
        return;
      }
      confirmDlg = Confirm();
      confirmDlg.active = true;
      confirmDlg.idx = i;
      confirmDlg.value = pending[i];
      confirmDlg.line1 = String(p.name) + ":  " + fmtParam(d, i, pVal(d, i)) + "  ->  " + fmtParam(d, i, pending[i]);
      confirmDlg.line2 = d.family == 2 ? "ANT write is experimental - it is read back" : "Check the value is right for your cells";
      drawConfirm();
      return;
    }
  }
}
// returns true when the dialog closed
static bool confirmTouch(int x, int y) {
  if (hit(C_CANCEL, x, y)) {
    confirmDlg.active = false;
    showToast("Cancelled");
    return true;
  }
  if (hit(C_OK, x, y)) {
    confirmDlg.active = false;
    const char *e = confirmDlg.isSwitch ? bmsSetSwitch(confirmDlg.idx, confirmDlg.on)
                                        : bmsWriteParam(confirmDlg.idx, confirmDlg.value);
    if (e) showToast(String("Not sent: ") + e, 4000);
    else {
      showToast("Writing to BMS...", 3000);
      if (!confirmDlg.isSwitch) pendingSet[confirmDlg.idx] = false;
    }
    return true;
  }
  return false;
}

// ------------------------------------------------------------- splash ----
// BattLab logo on black for 5 s with a fade-in and a thin progress bar.
// Bluetooth and Wi-Fi are already starting in the background meanwhile.
static void splash() {
  const uint32_t SPLASH_MS = 5000;
  tft.setBrightness(0);
  tft.fillScreen(TFT_BLACK);
  const int lx = (480 - LOGO_W) / 2, ly = 96;
  static lgfx::rgb565_t line[LOGO_W];
  for (int y = 0; y < LOGO_H; y++) {
    const uint8_t *row = LOGO_GRAY + size_t(y) * LOGO_W;
    for (int x = 0; x < LOGO_W; x++) line[x] = lgfx::rgb565_t(row[x], row[x], row[x]);
    tft.pushImage(lx, ly + y, LOGO_W, 1, line);
  }
  const uint16_t barBg = tft.color565(40, 40, 40);
  const int bx = 140, by = 262, bw = 200;
  tft.fillRoundRect(bx, by, bw, 4, 2, barBg);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(tft.color565(120, 120, 120), TFT_BLACK);
  DisplaySnap d;
  takeDisplaySnapshot(d);
  if (d.resetAbnormal) {
    tft.setTextColor(C_BAD, TFT_BLACK);
    tft.drawString(String("Last restart: ") + d.resetReason, 240, 242);
  }
  uint32_t t0 = millis();
  int drawn = 0, shineWas = -1000, stepShown = -1;
  auto logoCols = [&](int x0, int x1, int shine) { // repaint logo columns x0..x1 with a light band at "shine"
    x0 = std::max(0, x0); x1 = std::min(LOGO_W - 1, x1);
    if (x1 < x0) return;
    for (int y = 0; y < LOGO_H; y++) {
      const uint8_t *row = LOGO_GRAY + size_t(y) * LOGO_W;
      for (int x = x0; x <= x1; x++) {
        int d = std::abs(x - shine + (y - LOGO_H / 2) / 3);
        int boost = d < 18 ? (18 - d) * 7 : 0;
        int v = std::min(255, row[x] + (row[x] > 40 ? boost : boost / 3));
        line[x - x0] = lgfx::rgb565_t(v, v, v);
      }
      tft.pushImage(lx + x0, ly + y, x1 - x0 + 1, 1, line);
    }
  };
  for (;;) {
    uint32_t t = millis() - t0;
    if (t > SPLASH_MS) t = SPLASH_MS;
    // fade the backlight in over the first 700 ms
    uint32_t level = BL_LEVEL[dcfg.bl];
    tft.setBrightness(t < 700 ? level * t / 700 : level);
    int w = bw * t / SPLASH_MS;
    if (w > drawn) {
      tft.fillRoundRect(bx, by, w, 4, 2, C_ACCENT);
      drawn = w;
    }
    // light sweep across the logo (twice), only the columns that change
    int ph = int(t % 2400);
    int shine = ph < 1400 ? -30 + (LOGO_W + 60) * ph / 1400 : -1000;
    if (shine != shineWas) {
      if (shineWas > -1000) logoCols(shineWas - 30, shineWas + 30, shine);
      if (shine > -1000) logoCols(shine - 30, shine + 30, shine);
      shineWas = shine;
    }
    // what is starting right now
    NetInfo n = netInfo();
    int step = t < 1500 ? 0 : !n.connected && n.configured ? 1 : 2;
    if (step != stepShown) {
      stepShown = step;
      tft.fillRect(100, 276, 280, 22, TFT_BLACK);
      tft.setTextColor(tft.color565(120, 120, 120), TFT_BLACK);
      tft.drawString(step == 0 ? "Starting Bluetooth" : step == 1 ? "Joining Wi-Fi " + n.ssid : n.connected ? String("Wi-Fi ok - cloud") : String("Ready"), 240, 286);
    }
    if (t >= SPLASH_MS) break;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  tft.fillScreen(C_BG);
}

// --------------------------------------------------------------- task ----
// The screen draws through short-lived sprites (up to ~22 KB). The cloud task
// takes this lock while its TLS connection is open, so both never need big
// RAM blocks at the same moment (classic ESP32 has little heap with BLE+Wi-Fi).
static SemaphoreHandle_t gfxMutex = nullptr;
bool gfxHold(uint32_t waitMs) {
  return !gfxMutex || xSemaphoreTake(gfxMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}
void gfxRelease() {
  if (gfxMutex) xSemaphoreGive(gfxMutex);
}

static void displayTask(void *) {
  splash();
  {
    uint16_t cal[8];
    Preferences p;
    p.begin("battlab-disp", true);
    bool have = p.getBytes("cal", cal, sizeof cal) == sizeof cal;
    p.end();
    #ifndef BATTLAB_TOUCH_CAPACITIVE
    if (have) tft.setTouchCalibrate(cal);
    else calibrate();
    #else
    (void)have;
    #endif
  }
#ifndef BATTLAB_ES3C40 // GPIO26 is a flash pin on the ESP32-S3
  ledcSetup(3, 2000, 8);
  ledcAttachPin(26, 3);
  ledcWriteTone(3, 0);
#endif
  int page = P_HOME, drawnPage = -1;
  uint32_t lastDraw = 0, lastTouch = 0, lastActivity = millis();
  bool wasDown = false, asleep = false, alarmWas = false;
  DisplaySnap d;
  int otaShown = -2;
  for (;;) {
    if (otaProgress() >= 0) { // firmware update: simple progress screen, no sprites
      int &shown = otaShown;
      int pct = otaProgress();
      if (shown == -2) {
        tft.setBrightness(BL_LEVEL[dcfg.bl]);
        tft.fillScreen(C_BG);
        tft.setFont(&fonts::FreeSansBold12pt7b);
        tft.setTextDatum(textdatum_t::middle_center);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString("Updating firmware", 240, 120);
        tft.setFont(&fonts::FreeSans9pt7b);
        tft.setTextColor(C_MUTED, C_BG);
        tft.drawString("Do not switch off - restarts by itself", 240, 210);
        tft.fillRoundRect(60, 156, 360, 18, 9, C_LINE);
      }
      if (pct != shown) {
        shown = pct;
        if (pct > 0) tft.fillRoundRect(60, 156, 360 * pct / 100, 18, 9, C_ACCENT);
        tft.fillRect(200, 178, 80, 22, C_BG);
        tft.setFont(&fonts::FreeSansBold9pt7b);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(String(pct) + "%", 240, 189);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (otaShown != -2) { otaShown = -2; drawnPage = -1; } // update failed: back to the dashboard
    xSemaphoreTake(gfxMutex, portMAX_DELAY);
    uint32_t now = millis();
    lgfx::touch_point_t tp;
    bool down = tft.getTouch(&tp) > 0;
    if (down && !wasDown && now - lastTouch > 220) {
      lastTouch = now;
      lastActivity = now;
      if (asleep) {
        asleep = false; // first touch only wakes the screen
        applyBacklight();
      } else if (overlay == OV_KB) {
        if (overlayTouch(tp.x, tp.y)) drawnPage = -1;
      } else if (overlay != OV_NONE && tp.y > 42 && tp.y < TAB_Y) {
        if (overlayTouch(tp.x, tp.y)) drawnPage = -1;
        lastDraw = 0;
      } else if (confirmDlg.active) {
        if (confirmTouch(tp.x, tp.y)) drawnPage = -1; // repaint the page
      } else if (tp.y >= TAB_Y) {
        int t = tp.x / tabW();
        if (t >= tabCount()) t = tabCount() - 1;
        if (t >= 0) page = t;
        if (overlay != OV_NONE) { overlay = OV_NONE; drawnPage = -1; }
      } else if (tp.y > 42) {
        if (page == P_HOME && (bigMode || (tp.x < 210 && tp.y < 228))) { bigMode = !bigMode; drawnPage = -1; }
        else if (page == P_PARAM) paramTouch(d, tp.x, tp.y);
        else if (page == P_LED) ledTouch(tp.x, tp.y);
        else if (page == P_BMS) bmsTouch(d, tp.x, tp.y);
        else if (page == P_SETUP) setupTouch(d, tp.x, tp.y);
        else if (page == P_INFO) {
          infoTouch(tp.x, tp.y);
          if (overlay != OV_NONE) drawnPage = -1;
        }
        lastDraw = 0; // show the change right away
      }
    }
    // hold -/+ on the PARAM page to repeat
    static uint32_t pressStart = 0, lastRepeat = 0;
    if (down && !wasDown) pressStart = now;
    if (down && wasDown && !asleep && !confirmDlg.active && page == P_PARAM &&
        now - pressStart > 500 && now - lastRepeat > 110 && paramRepeatable(tp.x, tp.y)) {
      lastRepeat = now;
      paramTouch(d, tp.x, tp.y);
      lastDraw = 0;
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
    if (wantDispApply) {
      wantDispApply = false;
      dcfgSave();
      if (!asleep) applyBacklight();
      drawnPage = -1;
    }
    // screen sleep
    uint32_t sleepMs = uint32_t(SLEEP_MIN[dcfg.sleep]) * 60000u;
    if (!asleep && sleepMs && now - lastActivity > sleepMs && !alarmText.length()) {
      asleep = true;
      tft.setBrightness(0);
    }
    static int tabsShown = 0;
    if (page >= tabCount()) page = P_HOME;
    if (tabCount() != tabsShown) { tabsShown = tabCount(); drawnPage = -1; }
    if (!confirmDlg.active && overlay != OV_KB && (page != drawnPage || now - lastDraw > 700)) {
      bool force = page != drawnPage;
      takeDisplaySnapshot(d);
      histPush(d);
      evalAlarm(d);
      static uint8_t profShown = 255;
      if (d.prof != profShown) { profShown = d.prof; force = true; }
      bool alarmNow = alarmText.length() > 0;
      if (alarmNow && !alarmWas) {
        lastActivity = now;
        if (asleep) { asleep = false; applyBacklight(); }
        alarmFlash();
        if (dcfg.beep) beep(3);
        force = true;
      }
      alarmWas = alarmNow;
      if (force) {
        if (drawnPage >= 0 && drawnPage != page) pageWipe(page > drawnPage ? 1 : -1);
        else clearContent();
        tabs(page);
        lastSocDrawn = -2;
        ledDrawnValid = false;
        bmsKey = "";
        setupKey = "";
        paramKey = "";
        infoKey = "";
        ovKey = "";
        if (page == P_BMS) {
          for (int i = 0; i < 6; i++)
            if (d.protocol == PROTO_VALUE[i]) protoSel = i;
        }
      }
      if (!asleep) {
        header(d);
        switch (page) {
          case P_HOME: if (isEv(d) && !bigMode) evHome(d, force); else home(d, force); break;
          case P_CELLS: cells(d, force); break;
          case P_BMS: bmsPage(d, force); break;
          case P_PARAM: paramPageDraw(d, force); break;
          case P_LED: ledPage(force); break;
          case P_SETUP: setupPage(d, force); break;
          case P_INV: invPage(force); break;
          default:
            if (overlay == OV_WIFI) wifiOverlayDraw(force);
            else if (overlay == OV_CLOUD) cloudOverlayDraw(force);
            else info(d, force);
            break;
        }
      }
      drawnPage = page;
      lastDraw = millis();
    }
    if (page == P_HOME && drawnPage == P_HOME && !asleep && overlay == OV_NONE && !confirmDlg.active) {
      if (isEv(d) && !bigMode) evTick();
      else { gaugeTick(); flowAnim(d); }
    }
    xSemaphoreGive(gfxMutex);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void displayStart() {
  dcfgLoad();
  Preferences p;
  p.begin("battlab-disp", true);
  bool invert = p.getBool("inv", BATTLAB_SCREEN_INV_DEFAULT);
  bool rgb = p.getBool("rgb", false);
  p.end();
  tft.configure(invert, rgb);
  tft.init();
  tft.setRotation(dcfg.rot);
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
  tft.setBrightness(0);
  tft.fillScreen(TFT_BLACK);
  gfxMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(displayTask, "disp", 10240, nullptr, 1, nullptr, 1);
}
#endif
