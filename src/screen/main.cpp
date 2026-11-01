// Bambu Remote Screen - CYD (ESP32-2432S028 2.8" or ESP32-3248S035 3.5").
// Reads the printers' state from the cloud (Supabase, filled by the LAN
// bridge) and shows it anywhere with Wi-Fi.
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "bb_config.h"
#include "cloud_ca_bundle.h"
#include "ui.h"

// ------------------------------------------------------------ display ----
class LGFX : public lgfx::LGFX_Device {
#if defined(BB_CYD35)
  lgfx::Panel_ST7796 p1_;
  lgfx::Panel_ST7796 p2_;
#else
  lgfx::Panel_ILI9341 p1_;    // most ESP32-2432S028R
  lgfx::Panel_ILI9341_2 p2_;  // 2-USB (ST7789 type) variant
#endif
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  void configure(uint8_t panelType, bool invert, bool bgr) {
    lgfx::Panel_Device *panel = panelType == 1 ? (lgfx::Panel_Device *)&p2_ : (lgfx::Panel_Device *)&p1_;
    {
      auto c = bus_.config();
      c.spi_host = HSPI_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
#if defined(BB_CYD35)
      c.freq_write = 27000000;
#endif
      bus_.config(c);
      panel->setBus(&bus_);
    }
    {
      auto c = panel->config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.pin_busy = -1;
#if defined(BB_CYD35)
      c.panel_width = 320;
      c.panel_height = 480;
#else
      c.panel_width = 240;
      c.panel_height = 320;
#endif
      c.readable = true;
      c.invert = invert;
      c.rgb_order = bgr;
      c.dlen_16bit = false;
      c.bus_shared = true;
      panel->config(c);
    }
    {
      auto c = light_.config();
#if defined(BB_CYD35)
      c.pin_bl = 27;
#else
      c.pin_bl = 21;
#endif
      c.invert = false;
      c.freq = 12000;
      c.pwm_channel = 7;
      light_.config(c);
      panel->setLight(&light_);
    }
    {
      auto c = touch_.config();
      c.x_min = 300;
      c.x_max = 3900;
      c.y_min = 200;
      c.y_max = 3900;
      c.pin_int = 36;
      c.offset_rotation = 0;
      c.freq = 1000000;
#if defined(BB_CYD35)
      c.bus_shared = true;
      c.spi_host = HSPI_HOST;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_cs = 33;
#else  // 2.8": touch has its own pins (software SPI)
      c.bus_shared = false;
      c.spi_host = -1;
      c.pin_sclk = 25;
      c.pin_mosi = 32;
      c.pin_miso = 39;
      c.pin_cs = 33;
#endif
      touch_.config(c);
      panel->setTouch(&touch_);
    }
    setPanel(panel);
  }
};
static LGFX tft;
static LGFX_Sprite band(&tft);
static int SW = 320, SH = 240, BAND_H = 40;
static bool useBand = false;

// ----------------------------------------------------------- settings ----
struct Cfg {
  String ssid, pass, viewKey;
  uint8_t bright = 4, panel = 0;
  bool flip = false, invert = false, bgr = false;
} cfg;
static const uint8_t BL[6] = {0, 30, 70, 120, 180, 255};

static void cfgLoad() {
  Preferences p;
  p.begin("bbscreen", true);
  cfg.ssid = p.getString("ssid", "");
  cfg.pass = p.getString("pass", "");
  cfg.viewKey = p.getString("vkey", BB_DEFAULT_VIEW_KEY);
  cfg.bright = p.getUChar("bright", 4);
  cfg.flip = p.getBool("flip", false);
  cfg.panel = p.getUChar("panel", 0);
  cfg.invert = p.getBool("invert", false);
  cfg.bgr = p.getBool("bgr", false);
  p.end();
  if (cfg.bright < 1 || cfg.bright > 5) cfg.bright = 4;
  if (!cfg.viewKey.length()) cfg.viewKey = BB_DEFAULT_VIEW_KEY;
}
static void cfgSave() {
  Preferences p;
  p.begin("bbscreen", false);
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putString("vkey", cfg.viewKey);
  p.putUChar("bright", cfg.bright);
  p.putBool("flip", cfg.flip);
  p.putUChar("panel", cfg.panel);
  p.putBool("invert", cfg.invert);
  p.putBool("bgr", cfg.bgr);
  p.end();
}

// -------------------------------------------------------------- model ----
static Model shared;           // written by net task, guarded by mtx
static SemaphoreHandle_t mtx;
static volatile bool dataDirty = true;
static volatile bool wantFetch = false;
static bool apOn = false;
static uint32_t lastOkMs = 0;

static void copyStr(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

// "2026-11-01T08:12:49.057517+00:00" -> epoch (UTC)
static time_t parseIso(const char *s) {
  if (!s || strlen(s) < 19) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int y = Y - (M <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  time_t t = days * 86400L + h * 3600 + m * 60 + sec;
  const char *z = strpbrk(s + 19, "+-Z");
  if (z && *z != 'Z' && strlen(z) >= 6) {
    int oh = atoi(z + 1), om = atoi(z + 4);
    long off = oh * 3600 + om * 60;
    t += (*z == '+') ? -off : off;
  }
  return t;
}

static void parsePrinter(JsonObject o, Printer &p, time_t srvNow) {
  copyStr(p.serial, sizeof p.serial, o["serial"]);
  copyStr(p.name, sizeof p.name, o["name"] | o["serial"].as<const char *>());
  copyStr(p.model, sizeof p.model, o["model"] | "");
  p.online = o["online"] | false;
  copyStr(p.err, sizeof p.err, o["error_code"] | "");
  time_t up = parseIso(o["updated_at"]);
  p.ageS = (srvNow && up) ? (long)(srvNow - up) : 0;
  JsonObject s = o["state"];
  copyStr(p.state, sizeof p.state, s["gcode_state"] | "");
  copyStr(p.task, sizeof p.task, s["subtask_name"] | "");
  p.pct = s["mc_percent"] | 0;
  p.remain = s["remain"] | 0;
  p.layer = s["layer"] | 0;
  p.tlayer = s["tlayer"] | 0;
  p.noz = s["noz"][0] | 0.0f;
  p.nozT = s["noz"][1] | 0.0f;
  p.dual = s.containsKey("noz2");
  p.noz2 = s["noz2"][0] | 0.0f;
  p.noz2T = s["noz2"][1] | 0.0f;
  p.active = s["active"] | 0;
  p.bed = s["bed"][0] | 0.0f;
  p.bedT = s["bed"][1] | 0.0f;
  p.cham = s.containsKey("cham") ? s["cham"].as<float>() : -1000;
  p.fanPart = s["fan_part"] | -1;
  p.fanAux = s["fan_aux"] | -1;
  p.fanCham = s["fan_cham"] | -1;
  p.spd = s["spd"] | 2;
  JsonArray h = s["hms"];
  p.nhms = 0;
  for (JsonVariant v : h) {
    if (p.nhms < 3) copyStr(p.hms[p.nhms], sizeof p.hms[0], v.as<const char *>());
    p.nhms++;
  }
  if (p.nhms > 3) p.nhms = 3;
  p.trayNow = s["tray_now"] | 255;
  p.nams = 0;
  for (JsonObject a : s["ams"].as<JsonArray>()) {
    if (p.nams >= 4) break;
    AmsUnit &u = p.ams[p.nams++];
    u = AmsUnit();
    u.id = a["id"] | 0;
    u.hum = a["hum"] | -1;
    u.humLvl = a["lvl"] | -1;
    u.temp = a.containsKey("temp") ? a["temp"].as<float>() : -1000;
    u.dryMin = a["dry"] | 0;
    JsonArray tr = a["trays"];
    u.ntray = tr.size() > 4 ? 4 : tr.size();
    for (int i = 0; i < u.ntray; i++) {
      JsonVariant t = tr[i];
      Tray &x = u.tray[i];
      x = Tray();
      if (t.isNull() || !t["t"].as<const char *>() || !strlen(t["t"].as<const char *>())) continue;
      x.empty = false;
      copyStr(x.type, sizeof x.type, t["t"]);
      const char *c = t["c"] | "808080";
      x.rgb = strtoul(String(c).substring(0, 6).c_str(), nullptr, 16);
      x.remain = t["r"] | -1;
    }
  }
}

static String fetchErr;
static bool fetchCloud(Model &m) {
  WiFiClientSecure tls;
  tls.setCACertBundle(CLOUD_CA_BUNDLE);
  tls.setTimeout(12);
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, String(BB_CLOUD_URL) + "/rest/v1/rpc/bambu_get")) { fetchErr = "https begin failed"; return false; }
  http.addHeader("apikey", BB_CLOUD_ANON);
  http.addHeader("Authorization", String("Bearer ") + BB_CLOUD_ANON);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"p_key\":\"" + cfg.viewKey + "\",\"p_events\":10}";
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  if (code != 200) {
    if (code == 401 || code == 403 || resp.indexOf("invalid key") >= 0) fetchErr = "wrong view key";
    else if (code < 0) fetchErr = "no internet (" + String(code) + ")";
    else fetchErr = "cloud error " + String(code);
    return false;
  }
  // zero-copy parse (strings stay in `resp`): 16 printers with AMS fit in 48 KB
  DynamicJsonDocument doc(49152);
  if (!doc.capacity()) { fetchErr = "low memory"; return false; }
  DeserializationError je = deserializeJson(doc, resp.begin());
  if (je) { fetchErr = je == DeserializationError::NoMemory ? "reply too big" : "bad reply"; return false; }
  time_t srvNow = parseIso(doc["now"]);
  m.haveData = true;
  copyStr(m.bridgeName, sizeof m.bridgeName, doc["bridge"]["name"] | "");
  m.bridgeOnline = doc["bridge"]["online"] | false;
  m.devices = doc["bridge"]["devices"] | 0;
  m.devicesOnline = doc["bridge"]["devices_online"] | 0;
  time_t ls = parseIso(doc["bridge"]["last_seen"]);
  m.bridgeAgeS = (ls && srvNow) ? (long)(srvNow - ls) : -1;
  m.np = 0;
  for (JsonObject o : doc["printers"].as<JsonArray>()) {
    if (m.np >= 16) break;
    m.p[m.np] = Printer();
    parsePrinter(o, m.p[m.np++], srvNow);
  }
  m.nev = 0;
  for (JsonObject e : doc["events"].as<JsonArray>()) {
    if (m.nev >= 10) break;
    Event &x = m.ev[m.nev++];
    x = Event();
    x.id = e["id"] | 0L;
    copyStr(x.printer, sizeof x.printer, e["printer_name"] | e["serial"].as<const char *>());
    copyStr(x.kind, sizeof x.kind, e["kind"] | "");
    copyStr(x.task, sizeof x.task, e["task"] | "");
    copyStr(x.err, sizeof x.err, e["error_code"] | "");
    x.t = parseIso(e["created_at"]);
  }
  return true;
}

static void netTask(void *) {
  static Model work;  // ~11 KB: keep off the task stack
  uint32_t lastFetch = 0;
  bool ntp = false;
  for (;;) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !ntp) {
      configTzTime(BB_TZ, "pool.ntp.org", "time.google.com");
      ntp = true;
    }
    uint32_t period = 5000;
    if (up && (wantFetch || millis() - lastFetch > period || !lastFetch)) {
      wantFetch = false;
      lastFetch = millis();
      xSemaphoreTake(mtx, portMAX_DELAY);
      work = shared;
      xSemaphoreGive(mtx);
      bool ok = fetchCloud(work);
      xSemaphoreTake(mtx, portMAX_DELAY);
      if (ok) {
        shared = work;
        lastOkMs = millis();
        shared.cloudOk = true;
        shared.cloudErr[0] = 0;
      } else {
        shared.cloudOk = false;
        copyStr(shared.cloudErr, sizeof shared.cloudErr, fetchErr.c_str());
      }
      xSemaphoreGive(mtx);
      dataDirty = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ------------------------------------------------------------- portal ----
static WebServer web(80);
static DNSServer dns;
static const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Bambu Screen</title><style>body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e8edf2;margin:0;padding:16px;max-width:520px}
h1{font-size:20px;color:#00ae42}label{display:block;margin:14px 0 4px;color:#8a96a3;font-size:14px}
input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #303a48;background:#161c24;color:#e8edf2;font-size:16px}
button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:#00ae42;color:#fff;font-size:16px;font-weight:600}
.row{display:flex;gap:8px}.row>*{flex:1}small{color:#8a96a3}</style></head><body>)";

static void handleRoot() {
  int n = WiFi.scanComplete();
  String opts;
  if (n > 0)
    for (int i = 0; i < n && i < 20; i++) {
      String s = WiFi.SSID(i);
      if (!s.length() || opts.indexOf(">" + s + "<") >= 0) continue;
      opts += "<option" + String(s == cfg.ssid ? " selected" : "") + ">" + s + "</option>";
    }
  if (n == WIFI_SCAN_FAILED || n == 0) WiFi.scanNetworks(true);
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Bambu Remote Screen</h1><form method=post action=/save>";
  h += "<label>Home Wi-Fi</label><select name=ssid_sel onchange=\"document.getElementsByName('ssid')[0].value=this.value\"><option value=''>-- pick a network --</option>" + opts + "</select>";
  h += "<input name=ssid placeholder='or type the name' value='" + cfg.ssid + "' style='margin-top:6px'>";
  h += "<label>Wi-Fi password</label><input name=pass type=password placeholder='(unchanged)'>";
  h += "<label>View key (from the bridge)</label><input name=vkey value='" + cfg.viewKey + "'>";
  h += "<label>Screen panel (only if colours or image look wrong)</label><div class=row><select name=panel>";
  h += String("<option value=0") + (cfg.panel == 0 ? " selected" : "") + ">Type A (default)</option>";
  h += String("<option value=1") + (cfg.panel == 1 ? " selected" : "") + ">Type B</option></select>";
  h += String("<select name=invert><option value=0") + (!cfg.invert ? " selected" : "") + ">Normal</option><option value=1" + (cfg.invert ? " selected" : "") + ">Invert colours</option></select>";
  h += String("<select name=bgr><option value=0") + (!cfg.bgr ? " selected" : "") + ">RGB</option><option value=1" + (cfg.bgr ? " selected" : "") + ">BGR</option></select></div>";
  h += "<button>Save and restart</button></form><p><small>Firmware " BB_FW_VERSION "</small></p></body></html>";
  web.send(200, "text/html", h);
}
static void handleSave() {
  if (web.arg("ssid").length()) cfg.ssid = web.arg("ssid");
  if (web.arg("pass").length()) cfg.pass = web.arg("pass");
  if (web.arg("vkey").length()) cfg.viewKey = web.arg("vkey");
  cfg.viewKey.trim();
  cfg.panel = web.arg("panel").toInt() == 1;
  cfg.invert = web.arg("invert") == "1";
  cfg.bgr = web.arg("bgr") == "1";
  cfgSave();
  web.send(200, "text/html", String(FPSTR(PAGE_HEAD)) + "<h1>Saved</h1><p>The screen restarts and joins <b>" + cfg.ssid + "</b>.</p></body></html>");
  delay(800);
  ESP.restart();
}
static void startAp() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("BAMBU-SCREEN");
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  WiFi.scanNetworks(true);
  dataDirty = true;
}
static void webSetup() {
  web.on("/", handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([] {
    if (apOn) {
      web.sendHeader("Location", "http://192.168.4.1/");
      web.send(302, "text/plain", "");
    } else web.send(404, "text/plain", "not found");
  });
  web.begin();
}

// ----------------------------------------------------------------- UI ----
static UiState ui;
static long lastEventId = -1;

static void render(const Model &m) {
  bool setup = apOn && !m.wifi;
  if (useBand) {
    for (int y = 0; y < SH; y += BAND_H) {
      if (setup) uiDrawSetupAp(band, SW, SH, y, m);
      else uiDraw(band, SW, SH, y, m, ui);
      band.pushSprite(0, y);
    }
  } else {
    tft.startWrite();
    if (setup) uiDrawSetupAp(tft, SW, SH, 0, m);
    else uiDraw(tft, SW, SH, 0, m, ui);
    tft.endWrite();
  }
}

static void applyRotation() {
  tft.setRotation(cfg.flip ? 3 : 1);
  SW = tft.width();
  SH = tft.height();
}

static void loadTouchCal() {
  Preferences p;
  p.begin("bbscreen", true);
  uint16_t cal[8];
  bool have = p.getBytes("cal", cal, sizeof cal) == sizeof cal;
  p.end();
  if (have) tft.setTouchCalibrate(cal);
}
static void calibrate() {
  uint16_t cal[8];
  tft.fillScreen(uiBg());
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString("Touch calibration", SW / 2, SH / 2 - 20);
  tft.setFont(&fonts::DejaVu12);
  tft.drawString("Tap the tip of each corner arrow", SW / 2, SH / 2 + 10);
  tft.calibrateTouch(cal, (uint16_t)TFT_GREEN, uiBg(), 20);
  tft.setTouchCalibrate(cal);
  Preferences p;
  p.begin("bbscreen", false);
  p.putBytes("cal", cal, sizeof cal);
  p.end();
}

void setup() {
  Serial.begin(115200);
  cfgLoad();
  tft.configure(cfg.panel, cfg.invert, cfg.bgr);
  tft.init();
  applyRotation();
  tft.setBrightness(BL[cfg.bright]);
  loadTouchCal();
  ui.bright = cfg.bright;
  BAND_H = SW >= 480 ? 32 : 40;
  band.setColorDepth(16);
  useBand = band.createSprite(SW, BAND_H) != nullptr;
  mtx = xSemaphoreCreateMutex();
  copyStr(shared.fw, sizeof shared.fw, BB_FW_VERSION);
  copyStr(shared.keyTail, sizeof shared.keyTail, cfg.viewKey.length() > 4 ? cfg.viewKey.substring(cfg.viewKey.length() - 4).c_str() : "");
  copyStr(shared.ssid, sizeof shared.ssid, cfg.ssid.c_str());
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  if (cfg.ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  } else {
    startAp();
  }
  webSetup();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t bootMs = millis(), lastDraw = 0, toastAt = 0, downAt = 0;
  static bool down = false;
  static int dx0 = 0, dy0 = 0, lx = 0, ly = 0;
  if (apOn) dns.processNextRequest();
  web.handleClient();

  bool up = WiFi.status() == WL_CONNECTED;
  // no Wi-Fi for 60 s after boot -> open the setup hotspot (and keep trying)
  if (!up && !apOn && millis() - bootMs > 60000) startAp();

  // --- touch: tap on release, swipe on the printer page
  lgfx::touch_point_t tp;
  bool t = tft.getTouch(&tp) > 0;
  if (t) {
    if (!down) { down = true; dx0 = tp.x; dy0 = tp.y; downAt = millis(); }
    lx = tp.x; ly = tp.y;
  } else if (down) {
    down = false;
    static Model m;
    xSemaphoreTake(mtx, portMAX_DELAY);
    m = shared;
    xSemaphoreGive(mtx);
    int dx = lx - dx0;
    Action a = A_NONE;
    if (abs(dx) > SW / 5 && abs(ly - dy0) < SH / 4 && ui.page == PG_ONE) {
      uiSwipe(dx < 0 ? 1 : -1, m, ui);
      a = A_REDRAW;
    } else if (millis() - downAt < 1500) {
      a = uiTouch(SW, SH, dx0, dy0, m, ui);
    }
    switch (a) {
      case A_BRIGHT_DN:
      case A_BRIGHT_UP:
        cfg.bright = ui.bright;
        tft.setBrightness(BL[cfg.bright]);
        cfgSave();
        break;
      case A_ROTATE:
        cfg.flip = !cfg.flip;
        cfgSave();
        applyRotation();
        break;
      case A_CALIBRATE: calibrate(); break;
      case A_WIFI_SETUP: startAp(); break;
      case A_REFRESH: wantFetch = true; break;
      default: break;
    }
    if (a != A_NONE) dataDirty = true;
  }

  // --- redraw on new data, on input, or every 20 s for the clock
  if (dataDirty || millis() - lastDraw > 20000) {
    dataDirty = false;
    static Model m;
    xSemaphoreTake(mtx, portMAX_DELAY);
    m = shared;
    xSemaphoreGive(mtx);
    m.wifi = up;
    m.apMode = apOn;
    if (up) {
      copyStr(m.ip, sizeof m.ip, WiFi.localIP().toString().c_str());
      copyStr(m.ssid, sizeof m.ssid, WiFi.SSID().c_str());
      m.rssi = WiFi.RSSI();
    }
    m.fetchAgeS = lastOkMs ? (long)((millis() - lastOkMs) / 1000) : -1;
    time_t now = time(nullptr);
    m.now = now > 1700000000 ? now : 0;
    // new event -> banner
    if (m.nev && m.ev[0].id != lastEventId) {
      if (lastEventId >= 0 && m.ev[0].id > lastEventId) {
        uiMakeToast(m.ev[0], ui);
        if (ui.toast[0]) toastAt = millis();
      }
      lastEventId = m.ev[0].id;
    }
    if (ui.toast[0] && millis() - toastAt > 30000) ui.toast[0] = 0;
    render(m);
    lastDraw = millis();
  }
  delay(15);
}
