// BattLab Cloud Screen - firmware for the ESP32-3248S035 (CYD 3.5").
// It has no Bluetooth link of its own: it signs in to BattLab Cloud with a
// pair code, shows every battery of the account and controls them (MOSFETs,
// JK parameters, LED, BMS connection) through the cloud command queue.
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "logo.h"
#include "bms_params.h"
#include "cloud_config.h"
#include "cloud_ca_bundle.h"

#define BATTLAB_FW "2.4.0-cloudscreen"

// ------------------------------------------------------------ display ----
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
static uint16_t C_BG, C_CARD, C_LINE, C_TEXT, C_MUTED, C_ACCENT, C_GOOD, C_WARN, C_BAD, C_BLUE;

static const int TAB_Y = 278, TAB_H = 42, TABS = 6, TAB_W = 80;
enum Page { P_ALL, P_HOME, P_CELLS, P_GRAPH, P_CTRL, P_SETUP };
static const char *const TAB_NAMES[TABS] = {"ALL", "HOME", "CELLS", "GRAPH", "CONTROL", "SETUP"};

// ----------------------------------------------------------- settings ----
struct DispCfg {
  uint8_t bl = 4, sleep = 0, rot = 1, lowSoc = 15, hiTemp = 55;
  bool beep = true, offAlarm = true, show = false;
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
  dcfg.offAlarm = p.getBool("offalarm", true);
  dcfg.show = p.getBool("show", false);
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
  p.putBool("offalarm", dcfg.offAlarm);
  p.putBool("show", dcfg.show);
  p.end();
}

// ------------------------------------------------------------- model ----
// Filled by the network task, read by the screen task (copied under lock).
struct DevSum {
  String id, name, state, bms, fw;
  float soc = NAN, v = NAN, i = NAN, p = NAN, t1 = NAN, t2 = NAN, mos = NAN, cmin = NAN, cmax = NAN, cap = NAN;
  int cyc = -1;
  long age = -1;
  bool chg = false, dsg = false;
  bool online() const { return age >= 0 && age < 150; }
  bool live() const { return online() && !std::isnan(v); }
};
struct BleDev {
  String name, mac;
  int rssi = 0;
  uint8_t type = 0;
  bool cand = false;
};
struct CmdInfo {
  long id = 0;
  String c, s, r;
};
struct Focus {
  String id;
  float cells[32];
  int n = 0;
  bool jkValid = false;
  float jk[bmsw::JK_PARAM_COUNT];
  bool sw[3] = {false, false, false};
  bool remote = false, autoc = false, ledOn = false, ap = false, tx = false, scanning = false;
  uint8_t fam = 0, ledMode = 0, ledColor = 0, ledLevel = 2;
  String lw, board, note, mac, proto;
  std::vector<BleDev> devs;
  std::vector<CmdInfo> cmds;
};
struct Hist {
  String id, err;
  int hours = 0;
  long bucket = 0;
  std::vector<float> soc, v, i, p, tp, dm;
  uint32_t at = 0;
  bool loading = false;
};
struct Model {
  float ein = 0, eout = 0; // kWh today of the focused battery
  bool paired = false, synced = false;
  String status = "starting", name;
  std::vector<DevSum> devs;
  Focus f;
  Hist h;
  uint32_t okAt = 0;
};
static Model M;
static SemaphoreHandle_t mx;
struct Lock {
  Lock() { xSemaphoreTake(mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mx); }
};
// requests from the screen to the network task
struct OutCmd {
  String dev, cmd, args, label;
};
static std::vector<OutCmd> outQ;
static String focusId, histWantId, netToast;
static int histWantHours = 24;
static bool fastWanted = false, histWanted = false, syncNow = false;
static volatile int otaPct = -1;
static bool checkUpdate = false;
static int verNum(const char *v) { // "2.4.0-..." -> 20400
  int a = 0, b = 0, c = 0;
  sscanf(v, "%d.%d.%d", &a, &b, &c);
  return a * 10000 + b * 100 + c;
}

// ------------------------------------------------------------ network ----
static String staSsid, staPass, token, pairCode;
static void netLoad() {
  Preferences p;
  p.begin("battlab-net", true); // home Wi-Fi shared with the monitor firmware
  staSsid = p.getString("ssid", "");
  staPass = p.getString("pass", "");
  p.end();
  p.begin("battlab-view", false); // this screen's own cloud identity
  token = p.getString("token", "");
  pairCode = p.getString("pair", "");
  focusId = p.getString("focus", "");
  if (token.length() != 64) {
    char hx[65];
    for (int i = 0; i < 32; i++) snprintf(hx + i * 2, 3, "%02x", uint8_t(esp_random()));
    token = hx;
    p.putString("token", token);
  }
  if (pairCode.length() != 9) {
    static const char A[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no I O 0 1
    String c;
    for (int i = 0; i < 8; i++) {
      if (i == 4) c += '-';
      c += A[esp_random() % 32];
    }
    pairCode = c;
    p.putString("pair", pairCode);
  }
  p.end();
}
static void saveFocus(const String &id) {
  Preferences p;
  p.begin("battlab-view", false);
  p.putString("focus", id);
  p.end();
}
static void wifiApply() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (staSsid.length()) WiFi.begin(staSsid.c_str(), staPass.c_str());
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
}
static void netSetHomeWifi(const String &ssid, const String &pass) {
  staSsid = ssid;
  staPass = pass;
  Preferences p;
  p.begin("battlab-net", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  WiFi.disconnect(false);
  wifiApply();
}
static void netScanStart() { WiFi.scanNetworks(true); }
static String netScanJson() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "";
  if (n < 0) return "[]";
  DynamicJsonDocument d(4096);
  auto a = d.to<JsonArray>();
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonObject o : a) if (s == (const char *)o["ssid"]) dup = true;
    if (dup) continue;
    auto o = a.createNestedObject();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["lock"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  String out;
  serializeJson(d, out);
  return out;
}
static float jf(JsonVariantConst v) { return v.is<float>() || v.is<int>() ? v.as<float>() : NAN; }
static void setStatus(const String &s) { Lock l; M.status = s; }
static String errText(const String &resp, int code) {
  if (code < 0) return String("no connection (") + code + ")";
  StaticJsonDocument<512> d;
  if (!deserializeJson(d, resp) && d["message"].is<const char *>()) return d["message"].as<String>();
  return String("server error ") + code;
}

static void parseSync(JsonDocument &d) {
  std::vector<DevSum> devs;
  for (JsonObjectConst o : d["devs"].as<JsonArrayConst>()) {
    DevSum s;
    s.id = o["id"] | "";
    s.name = o["n"] | "";
    s.age = o["age"] | -1L;
    JsonObjectConst x = o["d"].as<JsonObjectConst>();
    s.state = x["state"] | "";
    s.bms = x["bms"] | "";
    s.fw = x["fw"] | "";
    s.soc = jf(x["soc"]);
    s.v = jf(x["v"]);
    s.i = jf(x["i"]);
    s.p = jf(x["p"]);
    s.t1 = jf(x["t1"]);
    s.t2 = jf(x["t2"]);
    s.mos = jf(x["mos"]);
    s.cmin = jf(x["cmin"]);
    s.cmax = jf(x["cmax"]);
    s.cap = jf(x["cap"]);
    s.cyc = x["cyc"] | -1;
    s.chg = x["chg"] | false;
    s.dsg = x["dsg"] | false;
    devs.push_back(s);
  }
  Focus f;
  f.id = d["focus"] | "";
  JsonObjectConst x = d["f"].as<JsonObjectConst>();
  for (float v : x["cells"].as<JsonArrayConst>())
    if (f.n < 32) f.cells[f.n++] = v;
  JsonObjectConst jk = x["jk"].as<JsonObjectConst>();
  if (!jk.isNull()) {
    int k = 0;
    for (float v : jk["v"].as<JsonArrayConst>())
      if (k < bmsw::JK_PARAM_COUNT) f.jk[k++] = v;
    f.jkValid = k == bmsw::JK_PARAM_COUNT;
    k = 0;
    for (bool b : jk["sw"].as<JsonArrayConst>())
      if (k < 3) f.sw[k++] = b;
  }
  JsonObjectConst c = x["cfg"].as<JsonObjectConst>();
  if (!c.isNull()) {
    f.remote = true;
    f.fam = c["fam"] | 0;
    f.lw = c["lw"] | "";
    f.board = c["board"] | "";
    f.note = c["note"] | "";
    f.mac = c["mac"] | "";
    f.proto = c["proto"] | "";
    f.autoc = c["auto"] | false;
    f.ap = c["ap"] | false;
    f.tx = c["tx"] | false;
    f.scanning = c["scanning"] | false;
    JsonObjectConst led = c["led"].as<JsonObjectConst>();
    f.ledOn = led["on"] | false;
    f.ledMode = led["mode"] | 0;
    f.ledColor = led["color"] | 0;
    f.ledLevel = led["level"] | 2;
  }
  if (!f.jkValid) { // ANT / JBD: charge + discharge come with the live data
    f.sw[0] = x["chg"] | false;
    f.sw[1] = x["dsg"] | false;
  }
  for (JsonObjectConst o : x["devs"].as<JsonArrayConst>()) {
    BleDev b;
    b.name = o["name"] | "";
    b.mac = o["mac"] | "";
    b.rssi = o["rssi"] | 0;
    b.type = o["type"] | 0;
    b.cand = o["candidate"] | false;
    if (b.mac.length() == 17) f.devs.push_back(b);
  }
  std::sort(f.devs.begin(), f.devs.end(), [](const BleDev &a, const BleDev &b) {
    if (a.cand != b.cand) return a.cand;
    return a.rssi > b.rssi;
  });
  for (JsonObjectConst o : d["cmds"].as<JsonArrayConst>()) {
    CmdInfo ci;
    ci.id = o["id"] | 0L;
    ci.c = o["c"] | "";
    ci.s = o["s"] | "";
    ci.r = o["r"] | "";
    f.cmds.push_back(ci);
  }
  Lock l;
  M.paired = String(d["status"] | "") == "ok";
  M.name = d["name"] | "";
  M.devs.swap(devs);
  M.f = f;
  M.ein = d["ein"] | 0.f;
  M.eout = d["eout"] | 0.f;
  M.synced = true;
  M.okAt = millis();
  M.status = M.paired ? String("online") : String("waiting for pairing");
}

static void parseHist(JsonDocument &d, const String &id, int hours) {
  Hist h;
  h.id = id;
  h.hours = hours;
  h.bucket = d["b"] | 0L;
  auto take = [&](const char *k, std::vector<float> &out) {
    for (JsonVariantConst v : d[k].as<JsonArrayConst>()) out.push_back(jf(v));
  };
  take("soc", h.soc);
  take("v", h.v);
  take("i", h.i);
  take("p", h.p);
  take("tp", h.tp);
  take("dm", h.dm);
  h.at = millis();
  Lock l;
  M.h = h;
}

static void runOta(WiFiClientSecure &tls, const String &url) {
  if (!url.startsWith(String("https://") + CLOUD_SITE + "/fw/") || !url.endsWith(".bin")) {
    Lock l;
    netToast = "Update URL not allowed";
    return;
  }
  otaPct = 0;
  tls.stop();
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) otaPct = int(int64_t(cur) * 100 / total);
  });
  t_httpUpdate_return r = httpUpdate.update(tls, url);
  if (r == HTTP_UPDATE_OK) {
    otaPct = 100;
    delay(800);
    ESP.restart();
  }
  otaPct = -1;
  Lock l;
  netToast = "Update failed: " + httpUpdate.getLastErrorString();
}

static void netTask(void *) {
  WiFiClientSecure tls;
  tls.setCACertBundle(CLOUD_CA_BUNDLE);
  tls.setTimeout(10);
  HTTPClient http;
  http.setReuse(true);
  uint32_t next = 0;
  auto post = [&](const char *fn, const String &body, int &code) -> String {
    String resp;
    code = -1;
    if (http.begin(tls, String(CLOUD_URL) + "/rest/v1/rpc/" + fn)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", CLOUD_ANON_KEY);
      http.addHeader("Authorization", String("Bearer ") + CLOUD_ANON_KEY);
      code = http.POST(body);
      if (code > 0) resp = http.getString();
      http.end();
    }
    if (code < 0) {
      char why[96] = {0};
      if (tls.lastError(why, sizeof why)) setStatus(String("TLS: ") + why);
      tls.stop();
    }
    return resp;
  };
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!staSsid.length()) { setStatus("set up Wi-Fi in SETUP"); continue; }
    if (WiFi.status() != WL_CONNECTED) { setStatus("Wi-Fi connecting"); continue; }
    // 0. firmware update check (SETUP > PAIRING > CHECK UPDATE)
    bool chk;
    {
      Lock l;
      chk = checkUpdate;
      checkUpdate = false;
    }
    if (chk) {
      HTTPClient g;
      String url, ver;
      if (g.begin(tls, String("https://") + CLOUD_SITE + "/fw/manifest.json")) {
        int code = g.GET();
        if (code == 200) {
          DynamicJsonDocument d(4096);
          if (!deserializeJson(d, g.getString())) {
            ver = d["boards"]["cloudscreen"]["version"] | "";
            String rel = d["boards"]["cloudscreen"]["url"] | "";
            if (rel.length()) url = String("https://") + CLOUD_SITE + "/" + rel;
          }
        }
        g.end();
      }
      tls.stop();
      if (!ver.length()) { Lock l; netToast = "Could not read the update list"; }
      else if (verNum(ver.c_str()) <= verNum(BATTLAB_FW)) { Lock l; netToast = "Up to date (" + ver + ")"; }
      else runOta(tls, url);
      continue;
    }
    // 1. commands typed on the screen
    OutCmd o;
    bool haveCmd = false;
    {
      Lock l;
      if (!outQ.empty()) { o = outQ.front(); outQ.erase(outQ.begin()); haveCmd = true; }
    }
    if (haveCmd) {
      String body = String("{\"p_token\":\"") + token + "\",\"p_device\":\"" + o.dev +
                    "\",\"p_cmd\":\"" + o.cmd + "\",\"p_args\":" + (o.args.length() ? o.args : String("{}")) + "}";
      int code;
      String r = post("viewer_command", body, code);
      {
        Lock l;
        netToast = code == 200 ? "Sent: " + o.label + " - waiting for board" : "Not sent: " + errText(r, code);
      }
      next = millis() + 800;
      continue;
    }
    // 2. history for the GRAPH page
    String hid;
    int hh = 24;
    bool wantH;
    {
      Lock l;
      wantH = histWanted;
      hid = histWantId;
      hh = histWantHours;
      histWanted = false;
    }
    if (wantH && hid.length()) {
      { Lock l; M.h.loading = true; }
      String body = String("{\"p_token\":\"") + token + "\",\"p_device\":\"" + hid + "\",\"p_hours\":" + hh + "}";
      int code;
      String r = post("viewer_history", body, code);
      DynamicJsonDocument d(24576);
      if (code == 200 && !deserializeJson(d, r)) parseHist(d, hid, hh);
      else { Lock l; M.h.loading = false; M.h.err = errText(r, code); M.h.id = hid; M.h.hours = hh; M.h.at = millis(); }
      continue;
    }
    // 3. regular sync
    bool fast, now;
    String fid;
    {
      Lock l;
      fast = fastWanted;
      now = syncNow;
      syncNow = false;
      fid = focusId;
      for (auto &c : M.f.cmds)
        if (c.s == "pending" || c.s == "sent") fast = true;
    }
    if (!now && next && int32_t(millis() - next) < 0) continue;
    String body = String("{\"p_token\":\"") + token + "\",\"p_pair\":\"" + pairCode + "\",\"p_focus\":" +
                  (fid.length() == 36 ? "\"" + fid + "\"" : String("null")) + ",\"p_fast\":" +
                  (fast ? "true" : "false") + ",\"p_fw\":\"" BATTLAB_FW "\"}";
    int code;
    String r = post("viewer_sync", body, code);
    if (code == 200) {
      DynamicJsonDocument d(32768);
      if (!deserializeJson(d, r)) {
        parseSync(d);
        String ota = d["ota"] | "";
        if (ota.length()) { runOta(tls, ota); continue; } // started from the website
      } else setStatus("bad reply");
      next = millis() + (fast ? 2500 : 5000);
    } else {
      if (code > 0) setStatus(errText(r, code));
      else {
        bool tlsMsg;
        { Lock l; tlsMsg = M.status.startsWith("TLS"); }
        if (!tlsMsg) setStatus(String("no connection (") + code + ")");
      }
      next = millis() + 8000;
    }
  }
}

// ------------------------------------------------------------ helpers ----
static const char *B(bool b) { return b ? "1" : "0"; }
static String fmt(float v, int d) {
  if (std::isnan(v)) return "--";
  char b[16];
  snprintf(b, sizeof b, "%.*f", d, v);
  return b;
}
static String ageText(long s) {
  if (s < 0) return "never";
  if (s < 90) return String(s) + "s";
  if (s < 5400) return String(s / 60) + "m";
  if (s < 172800) return String(s / 3600) + "h";
  return String(s / 86400) + "d";
}
static String hm(float h) {
  if (std::isnan(h) || h < 0 || h > 999) return "--";
  int m = int(h * 60 + .5f);
  return m < 60 ? String(m) + "m" : String(m / 60) + "h" + (m % 60 < 10 ? "0" : "") + String(m % 60);
}
struct Btn {
  int x, y, w, h;
};
static bool hit(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
static void button(const Btn &b, const String &label, bool selected, uint16_t accent = 0,
                   const lgfx::IFont *font = nullptr, bool enabled = true) {
  uint16_t bg = selected ? (accent ? accent : C_ACCENT) : C_CARD;
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 10, bg);
  if (!selected) tft.drawRoundRect(b.x, b.y, b.w, b.h, 10, C_LINE);
  tft.setFont(font ? font : &fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(!enabled ? C_LINE : selected ? C_BG : C_TEXT, bg);
  tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2 + 1);
}
static void label(int x, int y, const String &t, uint16_t c = 0) {
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(c ? c : C_MUTED, C_BG);
  tft.drawString(t, x, y);
}
static void clearContent() { tft.fillRect(0, 41, 480, TAB_Y - 41, C_BG); }
static String toast;
static uint32_t toastUntil = 0;
static void showToast(const String &t, uint32_t ms = 3000) {
  toast = t;
  toastUntil = millis() + ms;
}
static void beep(int times) {
  for (int i = 0; i < times; i++) {
    ledcWriteTone(3, 2300);
    vTaskDelay(pdMS_TO_TICKS(110));
    ledcWriteTone(3, 0);
    vTaskDelay(pdMS_TO_TICKS(90));
  }
}
static uint16_t socColor(float soc) {
  if (std::isnan(soc)) return C_LINE;
  return soc <= 15 ? C_BAD : soc <= 35 ? C_WARN : C_GOOD;
}
static float maxTemp(const DevSum &d) {
  float t = -99;
  if (!std::isnan(d.t1)) t = std::max(t, d.t1);
  if (!std::isnan(d.t2)) t = std::max(t, d.t2);
  return t;
}
static String devState(const DevSum &d) {
  if (d.age < 0) return "NEVER SEEN";
  if (!d.online()) return "OFFLINE " + ageText(d.age);
  if (!d.live()) return "NO BMS";
  return d.i > 0.3f ? "CHARGING" : d.i < -0.3f ? "DISCHARGING" : "IDLE";
}

// ------------------------------------------------------------ alarms ----
static String alarmText;
static void evalAlarm(const Model &m) {
  alarmText = "";
  for (auto &d : m.devs) {
    String nm = d.name.length() > 10 ? d.name.substring(0, 10) : d.name;
    if (d.live()) {
      if (dcfg.lowSoc && d.soc < dcfg.lowSoc) { alarmText = nm + " LOW " + int(d.soc + .5f) + "%"; return; }
      float t = maxTemp(d);
      if (dcfg.hiTemp && t > dcfg.hiTemp) { alarmText = nm + " HOT " + int(t + .5f) + "C"; return; }
    } else if (dcfg.offAlarm && d.age >= 300 && d.age < 86400) {
      alarmText = nm + " OFFLINE";
      return;
    }
  }
}

// ------------------------------------------------------ header + tabs ----
static const Btn B_PREV = {372, 4, 50, 32}, B_NEXT = {426, 4, 50, 32};
static int allPage = 0;
static void header(const Model &m, int page) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(8);
  if (!s.createSprite(480, 40)) return;
  s.fillSprite(C_BG);
  {
    static lgfx::rgb565_t line[LOGO_S_W];
    uint8_t br = 11, bgc = 17, bb = 23;
    for (int y = 0; y < LOGO_S_H; y++) {
      const uint8_t *row = LOGO_S_GRAY + size_t(y) * LOGO_S_W;
      for (int x = 0; x < LOGO_S_W; x++) {
        uint8_t v = row[x];
        line[x] = lgfx::rgb565_t(br + (255 - br) * v / 255, bgc + (255 - bgc) * v / 255, bb + (255 - bb) * v / 255);
      }
      s.pushImage(8, 5 + y, LOGO_S_W, 1, line);
    }
  }
  bool ok = m.status == "online";
  s.fillCircle(124, 20, 6, ok ? C_GOOD : m.status.startsWith("waiting") || m.status.startsWith("Wi-Fi") ? C_WARN : C_BAD);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  bool nav = page != P_ALL && page != P_SETUP && m.devs.size() > 1;
  bool pager = page == P_ALL && m.devs.size() > 6;
  int right = nav || pager ? 364 : 420;
  String txt;
  if (millis() < toastUntil) txt = toast;
  else if (!ok) txt = "Cloud: " + m.status;
  else if (page == P_ALL || page == P_SETUP) {
    int live = 0;
    float pw = 0;
    for (auto &d : m.devs)
      if (d.live()) { live++; pw += d.p; }
    txt = String(m.devs.size()) + " batteries  " + live + " live";
    if (live) txt += std::fabs(pw) >= 1000 ? "  " + fmt(pw / 1000, 2) + "kW" : "  " + fmt(pw, 0) + "W";
  } else {
    for (auto &d : m.devs)
      if (d.id == m.f.id) txt = d.name + "  " + devState(d);
  }
  if (alarmText.length() && millis() >= toastUntil) {
    int w = std::min(right - 138, int(s.textWidth(alarmText)) + 24);
    s.fillRoundRect(136, 7, w, 26, 13, C_BAD);
    s.setTextColor(C_TEXT, C_BAD);
    s.drawString(alarmText, 148, 21);
  } else {
    while (txt.length() && s.textWidth(txt) > right - 140) txt.remove(txt.length() - 1);
    s.setTextColor(C_TEXT, C_BG);
    s.drawString(txt, 138, 21);
  }
  time_t now = time(nullptr);
  if (now > 1700000000) { // clock (Thailand time from NTP)
    struct tm t;
    localtime_r(&now, &t);
    char hhmm[8];
    snprintf(hhmm, sizeof hhmm, "%02d:%02d", t.tm_hour, t.tm_min);
    s.setFont(&fonts::FreeSansBold9pt7b);
    s.setTextDatum(textdatum_t::middle_right);
    s.setTextColor(C_TEXT, C_BG);
    if (!(nav || pager)) s.drawString(hhmm, 472, 21);
    s.setFont(&fonts::FreeSans9pt7b);
  }
  if (nav || pager) {
    for (int k = 0; k < 2; k++) {
      Btn b = k ? B_NEXT : B_PREV;
      s.fillRoundRect(b.x, b.y, b.w, b.h, 8, C_CARD);
      s.drawRoundRect(b.x, b.y, b.w, b.h, 8, C_LINE);
      s.setFont(&fonts::FreeSansBold12pt7b);
      s.setTextDatum(textdatum_t::middle_center);
      s.setTextColor(C_TEXT, C_CARD);
      s.drawString(k ? ">" : "<", b.x + b.w / 2, b.y + b.h / 2 + 1);
    }
  }
  s.drawFastHLine(0, 39, 480, C_LINE);
  s.pushSprite(0, 0);
  s.deleteSprite();
}
static void tabs(int page) {
  tft.fillRect(0, TAB_Y, 480, TAB_H, C_CARD);
  tft.drawFastHLine(0, TAB_Y, 480, C_LINE);
  tft.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < TABS; i++) {
    bool sel = i == page;
    tft.setFont(sel ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
    if (sel) tft.fillRect(i * TAB_W + 8, TAB_Y + 1, TAB_W - 16, 3, C_ACCENT);
    tft.setTextColor(sel ? C_ACCENT : C_MUTED, C_CARD);
    tft.drawString(TAB_NAMES[i], i * TAB_W + TAB_W / 2, TAB_Y + TAB_H / 2 + 2);
  }
}

// --------------------------------------------------------- ALL page ----
static String allKey;
static Btn cardBtn(int k) { return {8 + (k % 2) * 236, 46 + (k / 2) * 76, 228, 72}; }
static void card(const Btn &b, const DevSum &d, bool sel) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(b.w, b.h)) return;
  s.fillSprite(C_BG);
  bool alarm = d.live() && ((dcfg.lowSoc && d.soc < dcfg.lowSoc) || (dcfg.hiTemp && maxTemp(d) > dcfg.hiTemp));
  s.fillRoundRect(0, 0, b.w, b.h, 10, C_CARD);
  if (sel || alarm) s.drawRoundRect(0, 0, b.w, b.h, 10, alarm ? C_BAD : C_ACCENT);
  if (sel && !alarm) s.drawRoundRect(1, 1, b.w - 2, b.h - 2, 9, C_ACCENT);
  s.fillCircle(14, 15, 5, d.live() ? C_GOOD : d.online() ? C_WARN : C_BAD);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  s.setTextColor(C_TEXT, C_CARD);
  String nm = d.name;
  while (nm.length() && s.textWidth(nm) > 140) nm.remove(nm.length() - 1);
  s.drawString(nm, 26, 15);
  s.setFont(&fonts::FreeSansBold12pt7b);
  s.setTextDatum(textdatum_t::middle_right);
  s.setTextColor(d.live() ? C_TEXT : C_MUTED, C_CARD);
  s.drawString(d.live() ? String(int(d.soc + .5f)) + "%" : String("--"), b.w - 10, 16);
  s.setFont(&fonts::FreeSans9pt7b);
  s.setTextDatum(textdatum_t::middle_left);
  if (d.live()) {
    s.setTextColor(C_TEXT, C_CARD);
    s.drawString(fmt(d.v, 1) + "V  " + fmt(d.i, 1) + "A  " + fmt(d.p, 0) + "W", 10, 37);
    s.setTextColor(C_MUTED, C_CARD);
    String l3 = String(d.i > 0.3f ? "CHG" : d.i < -0.3f ? "DSG" : "IDLE") + "  " + fmt(d.t1, 0) + "C";
    if (!std::isnan(d.cmax) && !std::isnan(d.cmin)) l3 += "  d" + String(int((d.cmax - d.cmin) * 1000 + .5f)) + "mV";
    s.drawString(l3, 10, 54);
  } else {
    s.setTextColor(d.online() ? C_WARN : C_MUTED, C_CARD);
    s.drawString(devState(d), 10, 37);
    s.setTextColor(C_MUTED, C_CARD);
    s.drawString(d.online() ? String("board online, connect a BMS") : String("last seen ") + ageText(d.age) + " ago", 10, 54);
  }
  int w = d.live() ? int((b.w - 20) * std::max(0.f, std::min(100.f, d.soc)) / 100) : 0;
  s.fillRoundRect(10, b.h - 6, b.w - 20, 3, 1, C_LINE);
  if (w > 0) s.fillRoundRect(10, b.h - 6, w, 3, 1, socColor(d.soc));
  s.pushSprite(b.x, b.y);
  s.deleteSprite();
}
static void pairScreen(const Model &m) {
  clearContent();
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString("PAIR CODE FOR THIS SCREEN", 12, 50);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(pairCode, 12, 100);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(String("1. On your phone open  ") + CLOUD_SITE, 12, 148);
  tft.drawString("2. Sign in, tap  Add device  and enter the code", 12, 174);
  tft.drawString("3. Every battery of that account appears here", 12, 200);
  tft.fillCircle(18, 240, 6, m.status == "waiting for pairing" ? C_WARN : C_BAD);
  tft.drawString(String("Cloud: ") + m.status, 32, 241);
}
static void allPageDraw(const Model &m, bool force) {
  String key = String(B(m.paired)) + m.status + allPage + focusId + int(dcfg.lowSoc) + int(dcfg.hiTemp);
  for (auto &d : m.devs) key += d.id + d.name + d.age / 30 + fmt(d.soc, 0) + fmt(d.v, 1) + fmt(d.i, 1) + fmt(d.t1, 0) + d.state;
  if (!force && key == allKey) return;
  allKey = key;
  if (!m.paired) {
    if (m.synced || m.status != "starting") pairScreen(m);
    return;
  }
  clearContent();
  if (m.devs.empty()) {
    label(16, 90, "No batteries in this account yet.", C_TEXT);
    label(16, 118, "Add a BattLab board on the website with its pair code.");
    return;
  }
  int pages = (m.devs.size() + 5) / 6;
  if (allPage >= pages) allPage = pages - 1;
  for (int k = 0; k < 6; k++) {
    size_t i = allPage * 6 + k;
    if (i >= m.devs.size()) break;
    card(cardBtn(k), m.devs[i], m.devs[i].id == focusId);
  }
  if (pages > 1) {
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::bottom_right);
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString(String(allPage + 1) + "/" + pages, 472, TAB_Y - 1);
  }
}

// --------------------------------------------------------- HOME page ----
static const DevSum *focusDev(const Model &m) {
  for (auto &d : m.devs)
    if (d.id == m.f.id) return &d;
  return nullptr;
}
static void tile(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit, uint16_t accent) {
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
static void pill(LGFX_Sprite &s, int x, int y, const char *t, bool on) {
  uint16_t bg = on ? C_GOOD : C_LINE;
  s.fillRoundRect(x, y, 66, 26, 13, bg);
  s.setTextColor(on ? C_BG : C_MUTED, bg);
  s.setFont(&fonts::FreeSansBold9pt7b);
  s.setTextDatum(textdatum_t::middle_center);
  s.drawString(t, x + 33, y + 14);
}
static String homeKey;
static bool bigMode = false;
static void bigHome(const DevSum &d) {
  bool ok = d.live();
  int soc = ok ? int(d.soc + .5f) : -1;
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  if (!b.createSprite(480, 236)) return;
  b.fillSprite(C_BG);
  b.setTextDatum(textdatum_t::middle_center);
  b.setFont(&fonts::FreeSansBold24pt7b);
  b.setTextSize(2.4f);
  b.setTextColor(C_TEXT, C_BG);
  b.drawString(soc < 0 ? String("--") : String(soc) + "%", 240, 74);
  b.setTextSize(1);
  b.fillRoundRect(40, 132, 400, 14, 7, C_LINE);
  if (soc > 0) b.fillRoundRect(40, 132, 4 * std::min(soc, 100), 14, 7, socColor(d.soc));
  b.setFont(&fonts::FreeSansBold18pt7b);
  b.setTextColor(d.i < 0 ? C_WARN : C_GOOD, C_BG);
  b.drawString(ok ? fmt(d.p, 0) + " W" : String("--"), 130, 186);
  b.setTextColor(C_TEXT, C_BG);
  b.drawString(ok ? fmt(d.v, 1) + " V" : String("--"), 350, 186);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextColor(C_MUTED, C_BG);
  b.drawString(ok ? d.name + "   " + fmt(d.i, 1) + " A   " + fmt(d.t1, 0) + " C" : d.name + "  " + devState(d), 240, 222);
  b.pushSprite(0, 41);
  b.deleteSprite();
}
// moving chevrons under the SOC number: up = charging, down = discharging
static void flowAnim(const DevSum *d) {
  static uint32_t last = 0;
  if (bigMode || !d || millis() - last < 140) return;
  last = millis();
  int dir = !d->live() ? 0 : d->i > 0.3f ? 1 : d->i < -0.3f ? -1 : 0;
  LGFX_Sprite s(&tft);
  s.setColorDepth(8);
  if (!s.createSprite(64, 18)) return;
  s.fillSprite(C_BG);
  int phase = (millis() / 140) % 3;
  if (dir == 0) s.fillRoundRect(8, 7, 48, 4, 2, C_LINE);
  else
    for (int k = 0; k < 3; k++) {
      int idx = dir > 0 ? 2 - k : k;
      uint16_t c = idx == phase ? (dir > 0 ? C_GOOD : C_WARN) : C_LINE;
      int x = 6 + k * 20;
      if (dir > 0) s.fillTriangle(x, 14, x + 16, 14, x + 8, 3, c);
      else s.fillTriangle(x, 3, x + 16, 3, x + 8, 14, c);
    }
  s.pushSprite(106 - 32, 150 + 37);
  s.deleteSprite();
}
static void noFocus(const Model &m) {
  clearContent();
  label(16, 90, m.paired ? "Pick a battery on the ALL tab." : "Pair this screen first (ALL tab).", C_TEXT);
}
static void homePage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  String key = dp ? dp->id + dp->age / 10 + fmt(dp->soc, 0) + fmt(dp->v, 2) + fmt(dp->i, 1) + fmt(dp->t1, 0) + fmt(dp->mos, 0) +
                        fmt(dp->cap, 1) + B(dp->chg) + B(dp->dsg) + m.f.n + fmt(dp->cmin, 3) + fmt(dp->cmax, 3)
                  : String("none") + B(m.paired);
  key += fmt(m.ein, 2) + fmt(m.eout, 2) + String(m.h.at) + B(bigMode);
  if (!force && key == homeKey) return;
  homeKey = key;
  if (!dp) { noFocus(m); return; }
  const DevSum &d = *dp;
  if (bigMode) { bigHome(d); return; }
  if ((m.h.id != d.id || millis() - m.h.at > 300000) && !m.h.loading) { // SOC sparkline data
    Lock l;
    if (!histWanted) { histWanted = true; histWantId = d.id; histWantHours = m.h.id == d.id && m.h.hours ? m.h.hours : 24; M.h.loading = true; }
  }
  bool ok = d.live();
  // gauge
  const int cx = 106, cy = 150, r1 = 86, r0 = 71;
  float soc = ok ? d.soc : NAN;
  tft.fillRect(0, 41, 210, 236, C_BG);
  tft.fillArc(cx, cy, r0, r1, 135, 405, C_LINE);
  if (ok && soc > 0) tft.fillArc(cx, cy, r0, r1, 135, 135 + 270.f * std::min(100.f, soc) / 100.f, socColor(soc));
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.drawString(ok ? String(int(soc + .5f)) + "%" : String("--"), cx, cy - 4);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(ok ? C_MUTED : C_WARN, C_BG);
  tft.drawString(ok ? String("SOC") : devState(d), cx, cy + (ok ? 25 : 34));
  int x0 = 214, x1 = 346, w = 126, h = 56;
  tile(x0, 46, w, h, "VOLTAGE", ok ? fmt(d.v, 2) : "--", "V", C_ACCENT);
  tile(x1, 46, w, h, "CURRENT", ok ? fmt(d.i, 1) : "--", "A", d.i < 0 ? C_WARN : C_GOOD);
  tile(x0, 106, w, h, "POWER", ok ? fmt(d.p, 0) : "--", "W", C_BLUE);
  tile(x1, 106, w, h, "REMAIN", ok ? fmt(d.cap, 1) : "--", "Ah", C_ACCENT);
  tile(x0, 166, w, h, "TEMP", ok ? fmt(d.t1, 0) : "--", "C", C_WARN);
  tile(x1, 166, w, h, "MOS", ok ? fmt(d.mos, 0) : "--", "C", C_WARN);
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  if (!b.createSprite(462, 46)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 462, 46, 10, C_CARD);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::middle_left);
  b.setTextColor(C_TEXT, C_CARD);
  String line;
  if (ok) {
    if (m.f.n) line = String(m.f.n) + "S ";
    if (!std::isnan(d.cmax)) line += "d" + String(int((d.cmax - d.cmin) * 1000 + .5f)) + "mV ";
    float full = (!std::isnan(d.cap) && d.soc > 3) ? d.cap / (d.soc / 100.f) : NAN;
    if (d.i > 0.3f && !std::isnan(full)) line += " full " + hm((full - d.cap) / d.i);
    else if (d.i < -0.3f && !std::isnan(d.cap)) line += " empty " + hm(d.cap / -d.i);
    if (d.cyc >= 0) line += "  " + String(d.cyc) + "cyc";
  } else {
    line = d.online() ? "Board online - connect a BMS (CONTROL > BOARD)" : "Offline - last data " + ageText(d.age) + " ago";
  }
  if (ok) {
    b.drawString(line, 12, 14);
    b.setTextColor(C_MUTED, C_CARD);
    b.drawString("today +" + fmt(m.ein, 2) + " / -" + fmt(m.eout, 2) + " kWh", 12, 33);
    // SOC sparkline from the cloud history
    if (m.h.id == d.id && m.h.soc.size() > 1) {
      const int sx = 196, sw = 112, sy = 8, sh = 30;
      int n = m.h.soc.size(), px = -1, py = 0;
      b.drawRect(sx, sy, sw, sh + 1, C_LINE);
      for (int k = 0; k < n; k++) {
        float v = m.h.soc[k];
        if (std::isnan(v)) { px = -1; continue; }
        int x = sx + k * (sw - 1) / (n - 1), y = sy + sh - int(sh * std::max(0.f, std::min(100.f, v)) / 100);
        if (px >= 0) b.drawLine(px, py, x, y, C_ACCENT);
        px = x;
        py = y;
      }
      b.setFont(&fonts::Font0);
      b.setTextDatum(textdatum_t::top_left);
      b.drawString(String("SOC ") + m.h.hours + "h", sx + 3, sy + 2);
      b.setFont(&fonts::FreeSans9pt7b);
      b.setTextDatum(textdatum_t::middle_left);
    }
    pill(b, 316, 10, "CHG", d.chg);
    pill(b, 388, 10, "DSG", d.dsg);
  } else {
    b.drawString(line, 12, 23);
  }
  b.pushSprite(9, 228);
  b.deleteSprite();
}

// -------------------------------------------------------- CELLS page ----
static String cellsKey;
static void cellsPage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  int n = dp && dp->live() ? m.f.n : 0;
  String key = String(dp ? dp->id : "") + n;
  for (int i = 0; i < n; i++) key += fmt(m.f.cells[i], 3);
  if (!force && key == cellsKey) return;
  cellsKey = key;
  clearContent();
  if (!dp) { noFocus(m); return; }
  if (!n) { label(16, 90, "No cell data - the battery is offline or has no BMS.", C_TEXT); return; }
  float mn = 9, mx = 0;
  int imn = 0, imx = 0;
  for (int i = 0; i < n; i++) {
    if (m.f.cells[i] < mn) mn = m.f.cells[i], imn = i;
    if (m.f.cells[i] > mx) mx = m.f.cells[i], imx = i;
  }
  bool dense = n > 24;
  int cols = n <= 16 ? 2 : 4, rows = (n + cols - 1) / cols;
  int gap = dense ? 3 : 4, gridH = dense ? 204 : 178;
  int cw = 462 / cols - 6, ch = std::min(36, gridH / rows - gap), barH = ch < 28 ? 3 : 5;
  float lo = mx < 3.7f ? 2.5f : 2.8f, hi = mx < 3.7f ? 3.65f : 4.25f;
  LGFX_Sprite c(&tft);
  c.setColorDepth(16);
  if (!c.createSprite(cw, ch)) return;
  for (int i = 0; i < n; i++) {
    float v = m.f.cells[i];
    c.fillSprite(C_BG);
    c.fillRoundRect(0, 0, cw, ch, 6, C_CARD);
    float f = std::max(0.f, std::min(1.f, (v - lo) / (hi - lo)));
    c.fillRoundRect(0, ch - barH, int(cw * f), barH, 1, i == imx ? C_BAD : i == imn ? C_BLUE : C_GOOD);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(textdatum_t::middle_left);
    c.setTextColor(C_MUTED, C_CARD);
    c.drawString(String(i + 1), 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.setTextDatum(textdatum_t::middle_right);
    c.setTextColor(i == imx ? C_BAD : i == imn ? C_BLUE : C_TEXT, C_CARD);
    c.drawString(fmt(v, 3), cw - 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.pushSprite(12 + (i % cols) * (cw + 6), 46 + (i / cols) * (ch + gap));
  }
  c.deleteSprite();
  int sh = dense ? 24 : 42, sy = dense ? 252 : 230;
  tft.fillRoundRect(9, sy, 462, sh, dense ? 8 : 10, C_CARD);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.drawString(String("min ") + fmt(mn, 3) + " (#" + (imn + 1) + ")  max " + fmt(mx, 3) + " (#" + (imx + 1) +
                     ")  delta " + int((mx - mn) * 1000 + .5f) + " mV",
                 21, sy + sh / 2);
}

// -------------------------------------------------------- GRAPH page ----
static int gMetric = 0, gRange = 1;
static const int G_HOURS[3] = {6, 24, 168};
static const char *const G_RANGE[3] = {"6H", "24H", "7D"};
static const char *const G_NAMES[5] = {"SOC", "VOLT", "AMP", "WATT", "TEMP"};
static const char *const G_UNITS[5] = {"%", "V", "A", "W", "C"};
static Btn gBtn(int i) { return {8 + i * 77, 46, 73, 34}; }
static const Btn G_RANGE_B = {398, 46, 74, 34};
static String graphKey;
static void graphPage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  if (dp && (m.h.id != dp->id || m.h.hours != G_HOURS[gRange] || millis() - m.h.at > 120000) && !m.h.loading) {
    Lock l;
    if (!histWanted) {
      histWanted = true;
      histWantId = dp->id;
      histWantHours = G_HOURS[gRange];
      M.h.loading = true;
    }
  }
  String key = String(dp ? dp->id : "") + gMetric + gRange + m.h.at + B(m.h.loading) + m.h.id + m.h.hours;
  if (!force && key == graphKey) return;
  graphKey = key;
  clearContent();
  if (!dp) { noFocus(m); return; }
  for (int i = 0; i < 5; i++) button(gBtn(i), G_NAMES[i], gMetric == i);
  button(G_RANGE_B, G_RANGE[gRange], true, C_BLUE);
  const std::vector<float> *pv[5] = {&m.h.soc, &m.h.v, &m.h.i, &m.h.p, &m.h.tp};
  const std::vector<float> &v = *pv[gMetric];
  tft.fillRoundRect(8, 86, 464, 188, 10, C_CARD);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_MUTED, C_CARD);
  bool fresh = m.h.id == dp->id && m.h.hours == G_HOURS[gRange];
  if (!fresh || (m.h.loading && v.empty())) { tft.drawString("Loading history...", 22, 150); return; }
  if (m.h.err.length() && v.empty()) { tft.drawString(m.h.err, 22, 150); return; }
  float lo = 1e9, hi = -1e9, last = NAN;
  int cnt = 0;
  for (float x : v)
    if (!std::isnan(x)) { lo = std::min(lo, x); hi = std::max(hi, x); last = x; cnt++; }
  if (cnt < 2) { tft.drawString("Not enough history yet (1 point per minute while live)", 22, 150); return; }
  if (gMetric == 0) { lo = 0; hi = 100; }
  if (hi - lo < 1e-3f) { hi += 1; lo -= 1; }
  float pad = (hi - lo) * 0.08f;
  if (gMetric) { lo -= pad; hi += pad; }
  int dec = gMetric == 1 ? 2 : gMetric == 2 || gMetric == 4 ? 1 : 0;
  tft.setTextColor(C_TEXT, C_CARD);
  tft.drawString(String(G_NAMES[gMetric]) + "  now " + fmt(last, dec) + G_UNITS[gMetric], 20, 100);
  const int px = 58, py = 116, pw = 404, ph = 132;
  tft.setTextDatum(textdatum_t::middle_right);
  tft.setTextColor(C_MUTED, C_CARD);
  for (int k = 0; k <= 2; k++) {
    int y = py + ph * k / 2;
    tft.drawFastHLine(px, y, pw, C_LINE);
    tft.drawString(fmt(hi - (hi - lo) * k / 2, dec > 1 ? 1 : dec), px - 6, y);
  }
  // time axis: buckets end "now"
  int n = v.size();
  long b = m.h.bucket > 0 ? m.h.bucket : 60;
  float spanH = G_HOURS[gRange];
  auto X = [&](int i) { return px + int(pw - (float)(n - 1 - i) * b / 3600.f / spanH * pw); };
  if (lo < 0 && hi > 0 && gMetric >= 2) {
    int y0 = py + int(ph * (hi / (hi - lo)));
    tft.drawFastHLine(px, y0, pw, C_MUTED);
  }
  int lx = -1, ly = -1;
  for (int i = 0; i < n; i++) {
    if (std::isnan(v[i])) { lx = -1; continue; }
    int x = X(i), y = py + int(ph * (hi - v[i]) / (hi - lo));
    if (x < px) { lx = -1; continue; }
    if (lx >= 0) {
      tft.drawLine(lx, ly, x, y, C_ACCENT);
      tft.drawLine(lx, ly + 1, x, y + 1, C_ACCENT);
    }
    lx = x;
    ly = y;
  }
  tft.setTextColor(C_MUTED, C_CARD);
  const char *ticks[3][3] = {{"-6h", "-3h", "now"}, {"-24h", "-12h", "now"}, {"-7d", "-3.5d", "now"}};
  for (int k = 0; k < 3; k++) {
    tft.setTextDatum(k == 0 ? textdatum_t::top_left : k == 1 ? textdatum_t::top_center : textdatum_t::top_right);
    tft.drawString(ticks[gRange][k], px + pw * k / 2, py + ph + 7);
  }
  tft.setTextDatum(textdatum_t::middle_right);
  tft.drawString(String("min ") + fmt(gMetric ? lo + pad : lo, dec) + "  max " + fmt(gMetric ? hi - pad : hi, dec), 460, 100);
}

// ------------------------------------------------------ CONTROL page ----
static int ctlSub = 0; // 0 MOS, 1 PARAMS, 2 LED, 3 BOARD
static const char *const CTL_SUB[4] = {"MOSFET", "PARAMS", "LED", "BOARD"};
static Btn subBtn(int i) { return {8 + i * 117, 46, 113, 34}; }
static const Btn STATUS_AREA = {196, 240, 276, 34};
static String ctlKey;
// parameters
static const int PROWS = 4, PPAGES = (bmsw::JK_PARAM_COUNT + PROWS - 1) / PROWS;
static int pPage = 0;
static float pend[bmsw::JK_PARAM_COUNT];
static bool pendSet[bmsw::JK_PARAM_COUNT];
static Btn pMinus(int r) { return {282, 88 + r * 38, 52, 34}; }
static Btn pPlus(int r) { return {338, 88 + r * 38, 52, 34}; }
static Btn pSet(int r) { return {394, 88 + r * 38, 78, 34}; }
static const Btn PP_PREV = {8, 240, 56, 34}, PP_NEXT = {128, 240, 56, 34};
// MOS
static Btn mosBtn(int i) { return {8 + i * 156, 92, 150, 78}; }
static const Btn M_READ = {8, 180, 150, 44};
// LED
static const Btn L_ON = {8, 88, 120, 40}, L_MINUS = {296, 88, 52, 40}, L_PLUS = {420, 88, 52, 40};
static Btn lMode(int i) { return {8 + i * 117, 134, 113, 38}; }
static Btn lColor(int i) { return {10 + i * 58, 180, 50, 50}; }
static const char *const LED_MODES[4] = {"RAINBOW", "SOLID", "BREATHE", "STATUS"};
static const uint8_t LED_PAL[8][3] = {{255, 0, 0}, {255, 80, 0}, {255, 190, 0}, {0, 255, 40},
                                      {0, 220, 255}, {0, 50, 255}, {170, 0, 255}, {255, 255, 255}};
// BOARD
static const Btn D_SCAN = {8, 88, 112, 38}, D_AUTO = {124, 88, 112, 38}, D_DISC = {240, 88, 124, 38},
                 D_RST = {368, 88, 104, 38}, D_TX = {8, 132, 150, 34}, D_AP = {164, 132, 170, 34};
static Btn dRow(int i) { return {8, 172 + i * 33, 464, 30}; }
static int devPage = 0;

struct Confirm {
  bool active = false;
  String line1, line2, cmd, args, label;
  bool warn = false;
} dlg;
static const Btn C_CANCEL = {70, 196, 160, 46}, C_OK = {250, 196, 160, 46};
static void drawConfirm() {
  tft.fillRoundRect(40, 62, 400, 196, 14, C_CARD);
  tft.drawRoundRect(40, 62, 400, 196, 14, dlg.warn ? C_BAD : C_WARN);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_WARN, C_CARD);
  tft.drawString("Send to battery?", 240, 90);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.drawString(dlg.line1, 240, 130);
  tft.setTextColor(dlg.warn ? C_BAD : C_MUTED, C_CARD);
  tft.drawString(dlg.line2, 240, 160);
  button(C_CANCEL, "CANCEL", false);
  button(C_OK, "SEND", true, dlg.warn ? C_BAD : C_WARN);
}
static void ask(const String &l1, const String &l2, const String &cmd, const String &args, const String &lbl, bool warn = false) {
  dlg = Confirm();
  dlg.active = true;
  dlg.line1 = l1;
  dlg.line2 = l2;
  dlg.cmd = cmd;
  dlg.args = args;
  dlg.label = lbl;
  dlg.warn = warn;
  drawConfirm();
}
static void sendCmd(const String &cmd, const String &args, const String &lbl) {
  Lock l;
  if (!focusId.length()) return;
  if (outQ.size() >= 6) { netToast = "Too many commands waiting"; return; }
  outQ.push_back({focusId, cmd, args, lbl});
}
static String cmdLabel(const String &c) {
  if (c == "bms_switch") return "MOSFET";
  if (c == "bms_param") return "Parameter";
  if (c == "led") return "LED";
  if (c == "bms_scan") return "Scan";
  if (c == "bms_connect") return "Connect";
  if (c == "bms_auto") return "Auto connect";
  if (c == "bms_disconnect") return "Disconnect";
  if (c == "bms_read") return "Read BMS";
  if (c == "radio_max") return "TX power";
  if (c == "wifi_ap") return "Hotspot";
  if (c == "restart") return "Restart";
  return c;
}
static void statusLine(const Model &m) {
  tft.fillRect(STATUS_AREA.x, STATUS_AREA.y, STATUS_AREA.w, STATUS_AREA.h, C_BG);
  if (m.f.cmds.empty()) return;
  const CmdInfo &c = m.f.cmds.front();
  uint16_t col = c.s == "done" ? C_GOOD : c.s == "error" || c.s == "expired" ? C_BAD : C_WARN;
  String st = c.s == "pending" ? "waiting" : c.s == "sent" ? "received" : c.s;
  String t = cmdLabel(c.c) + ": " + st + (c.r.length() && c.s != "done" ? " - " + c.r : String(""));
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_right);
  tft.setTextColor(col, C_BG);
  while (t.length() && tft.textWidth(t) > STATUS_AREA.w) t.remove(t.length() - 1);
  tft.drawString(t, STATUS_AREA.x + STATUS_AREA.w, STATUS_AREA.y + STATUS_AREA.h / 2);
}
static String fmtParam(int i, float v) { return fmt(v, bmsw::JK_PARAMS[i].decimals) + " " + bmsw::JK_PARAMS[i].unit; }
static void ctlPage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  const Focus &f = m.f;
  String key = String(dp ? dp->id : "") + ctlSub + pPage + devPage + B(f.remote) + int(f.fam) + B(f.jkValid) + B(f.sw[0]) + B(f.sw[1]) + B(f.sw[2]) +
               B(f.ledOn) + int(f.ledMode) + int(f.ledColor) + int(f.ledLevel) + B(f.ap) + B(f.tx) + B(f.scanning) + int(f.devs.size()) + f.mac +
               B(dp && dp->live()) + B(dp && dp->online());
  if (f.jkValid)
    for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) key += fmt(f.jk[i], 3) + (pendSet[i] ? fmt(pend[i], 3) : String("-"));
  for (auto &c : f.cmds) key += String(c.id) + c.s;
  if (!force && key == ctlKey) return;
  ctlKey = key;
  clearContent();
  if (!dp) { noFocus(m); return; }
  for (int i = 0; i < 4; i++) button(subBtn(i), CTL_SUB[i], ctlSub == i);
  if (!f.remote) {
    label(16, 110, "This board needs firmware 2.1.0 or newer for control.", C_TEXT);
    label(16, 136, String("Board firmware: ") + (dp->fw.length() ? dp->fw : String("?")));
    return;
  }
  if (!dp->online()) label(16, 262, "Board offline - commands wait 5 min", C_WARN);
  bool live = dp->live();
  if (ctlSub == 0) {
    const char *nm[3] = {"CHARGE", "DISCHARGE", "BALANCE"};
    for (int i = 0; i < 3; i++) {
      Btn b = mosBtn(i);
      bool avail = live && f.fam && !(f.fam == 3 && i == 2);
      bool on = f.sw[i];
      bool known = f.jkValid || i < 2;
      uint16_t bg = !avail ? C_CARD : on && known ? C_GOOD : C_CARD;
      tft.fillRoundRect(b.x, b.y, b.w, b.h, 12, bg);
      tft.drawRoundRect(b.x, b.y, b.w, b.h, 12, avail ? (on ? C_GOOD : C_LINE) : C_LINE);
      tft.setTextDatum(textdatum_t::middle_center);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextColor(on && avail && known ? C_BG : C_MUTED, bg);
      tft.drawString(nm[i], b.x + b.w / 2, b.y + 22);
      tft.setFont(&fonts::FreeSansBold18pt7b);
      tft.setTextColor(!avail ? C_LINE : on && known ? C_BG : C_TEXT, bg);
      tft.drawString(!known ? "?" : on ? "ON" : "OFF", b.x + b.w / 2, b.y + 52);
    }
    button(M_READ, "READ BMS", false, 0, nullptr, live);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(C_MUTED, C_BG);
    const char *fam[4] = {"BMS not identified", "JK BMS", "ANT BMS", "JBD BMS"};
    tft.drawString(String(fam[f.fam < 4 ? f.fam : 0]) + (f.mac.length() ? "  " + f.mac : String("")), 168, 194);
    tft.drawString(live ? "Tap a switch to change it" : "Battery is not live", 168, 214);
  } else if (ctlSub == 1) {
    if (f.fam != 1 || !f.jkValid) {
      label(16, 110, f.fam == 1 ? "Reading JK settings... (MOSFET > READ BMS)" : "Parameters can be edited on JK BMS.", C_TEXT);
      label(16, 136, f.fam == 2 || f.fam == 3 ? "ANT / JBD: use the MOSFET tab." : "Connect a JK BMS on the board first.");
    } else {
      for (int r = 0; r < PROWS; r++) {
        int i = pPage * PROWS + r;
        if (i >= bmsw::JK_PARAM_COUNT) break;
        const auto &p = bmsw::JK_PARAMS[i];
        int y = 88 + r * 38;
        tft.fillRoundRect(8, y, 270, 34, 8, C_CARD);
        tft.setFont(&fonts::FreeSans9pt7b);
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(C_MUTED, C_CARD);
        tft.drawString(p.name, 18, y + 17);
        bool edited = pendSet[i] && std::fabs(pend[i] - f.jk[i]) > p.step / 2;
        tft.setFont(&fonts::FreeSansBold9pt7b);
        tft.setTextDatum(textdatum_t::middle_right);
        tft.setTextColor(edited ? C_WARN : C_TEXT, C_CARD);
        tft.drawString(fmtParam(i, edited ? pend[i] : f.jk[i]), 270, y + 17);
        button(pMinus(r), "-", false, 0, &fonts::FreeSansBold12pt7b);
        button(pPlus(r), "+", false, 0, &fonts::FreeSansBold12pt7b);
        button(pSet(r), "SET", edited, C_WARN);
      }
      button(PP_PREV, "<", false, 0, &fonts::FreeSansBold12pt7b);
      button(PP_NEXT, ">", false, 0, &fonts::FreeSansBold12pt7b);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextDatum(textdatum_t::middle_center);
      tft.setTextColor(C_MUTED, C_BG);
      tft.drawString(String(pPage + 1) + "/" + PPAGES, 96, 258);
    }
  } else if (ctlSub == 2) {
    button(L_ON, f.ledOn ? "LED ON" : "LED OFF", f.ledOn, C_GOOD);
    button(L_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
    button(L_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
    for (int i = 0; i < 4; i++) {
      int hgt = 10 + i * 6;
      tft.fillRoundRect(362 + i * 13, 120 - hgt, 9, hgt, 3, i < f.ledLevel ? C_ACCENT : C_LINE);
    }
    label(146, 108, "Brightness");
    for (int i = 0; i < 4; i++) button(lMode(i), LED_MODES[i], f.ledMode == i);
    for (int i = 0; i < 8; i++) {
      Btn b = lColor(i);
      uint16_t c = tft.color565(LED_PAL[i][0], LED_PAL[i][1], LED_PAL[i][2]);
      tft.fillCircle(b.x + 25, b.y + 25, 21, c);
      if (f.ledColor == i) tft.drawCircle(b.x + 25, b.y + 25, 24, C_TEXT);
    }
  } else {
    button(D_SCAN, f.scanning ? "SCANNING" : "SCAN BMS", f.scanning);
    button(D_AUTO, "AUTO", f.autoc);
    button(D_DISC, "DISCONNECT", false);
    button(D_RST, "RESTART", false);
    button(D_TX, f.tx ? "TX MAX" : "TX LOW", f.tx, C_GOOD);
    button(D_AP, f.ap ? "HOTSPOT ON" : "HOTSPOT OFF", f.ap, C_GOOD);
    label(342, 149, f.board == "s3" ? "ESP32-S3" : f.board == "cyd35" ? "CYD 3.5" : f.board);
    if (f.devs.empty()) {
      label(16, 190, "Tap SCAN BMS, then pick a battery below.");
      label(16, 214, String("Now: ") + (f.mac.length() ? f.mac + " " + f.proto : String("none")));
    } else {
      int pages = (f.devs.size() + 1) / 2;
      if (devPage >= pages) devPage = 0;
      for (int i = 0; i < 2; i++) {
        size_t k = devPage * 2 + i;
        if (k >= f.devs.size()) break;
        Btn r = dRow(i);
        bool cur = f.devs[k].mac.equalsIgnoreCase(f.mac);
        tft.fillRoundRect(r.x, r.y, r.w, r.h, 8, C_CARD);
        if (cur) tft.drawRoundRect(r.x, r.y, r.w, r.h, 8, C_GOOD);
        tft.setFont(&fonts::FreeSansBold9pt7b);
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(f.devs[k].cand ? C_TEXT : C_MUTED, C_CARD);
        String nm = f.devs[k].name.length() ? f.devs[k].name : String("(no name)");
        if (nm.length() > 22) nm = nm.substring(0, 22);
        tft.drawString(nm, r.x + 10, r.y + 16);
        tft.setFont(&fonts::FreeSans9pt7b);
        tft.setTextDatum(textdatum_t::middle_right);
        tft.setTextColor(C_MUTED, C_CARD);
        tft.drawString(f.devs[k].mac + "  " + f.devs[k].rssi, r.x + r.w - 10, r.y + 16);
      }
      if (pages > 1) button(PP_PREV, ">", false, 0, &fonts::FreeSansBold12pt7b);
    }
  }
  statusLine(m);
}
static void ctlTouch(const Model &m, int x, int y) {
  for (int i = 0; i < 4; i++)
    if (hit(subBtn(i), x, y)) { ctlSub = i; return; }
  const Focus &f = m.f;
  const DevSum *dp = focusDev(m);
  if (!dp || !f.remote) return;
  bool live = dp->live();
  if (ctlSub == 0) {
    for (int i = 0; i < 3; i++)
      if (hit(mosBtn(i), x, y) && live && f.fam && !(f.fam == 3 && i == 2)) {
        bool on = !(f.sw[i] && (f.jkValid || i < 2));
        const char *what[3] = {"Charge MOSFET", "Discharge MOSFET", "Balancer"};
        ask(String(what[i]) + (on ? "  ->  ON" : "  ->  OFF"),
            i == 1 && !on ? String("Discharge OFF cuts power to the load!") : dp->name,
            "bms_switch", String("{\"which\":") + i + ",\"on\":" + (on ? "true" : "false") + "}",
            String(what[i]) + (on ? " ON" : " OFF"), i == 1 && !on);
        return;
      }
    if (hit(M_READ, x, y) && live) { sendCmd("bms_read", "{}", "Read BMS"); showToast("Reading settings..."); }
  } else if (ctlSub == 1 && f.fam == 1 && f.jkValid) {
    if (hit(PP_PREV, x, y)) { pPage = (pPage + PPAGES - 1) % PPAGES; return; }
    if (hit(PP_NEXT, x, y)) { pPage = (pPage + 1) % PPAGES; return; }
    for (int r = 0; r < PROWS; r++) {
      int i = pPage * PROWS + r;
      if (i >= bmsw::JK_PARAM_COUNT) break;
      const auto &p = bmsw::JK_PARAMS[i];
      float cur = pendSet[i] ? pend[i] : f.jk[i];
      if (hit(pMinus(r), x, y) || hit(pPlus(r), x, y)) {
        float v = cur + (hit(pPlus(r), x, y) ? p.step : -p.step);
        v = std::round(v / p.step) * p.step;
        pend[i] = std::max(p.min, std::min(p.max, v));
        pendSet[i] = true;
        return;
      }
      if (hit(pSet(r), x, y) && pendSet[i] && std::fabs(pend[i] - f.jk[i]) > p.step / 2) {
        bmsw::JkSettings js;
        js.valid = true;
        for (int k = 0; k < bmsw::JK_PARAM_COUNT; k++) js.v[k] = f.jk[k];
        if (const char *e = bmsw::checkJkValue(js, i, pend[i])) { showToast(String("Not allowed: ") + e, 4000); return; }
        char a[64];
        snprintf(a, sizeof a, "{\"index\":%d,\"value\":%.4f}", i, pend[i]);
        ask(String(p.name) + ":  " + fmtParam(i, f.jk[i]) + "  ->  " + fmtParam(i, pend[i]),
            "Check the value is right for your cells", "bms_param", a, String(p.name));
        return;
      }
    }
  } else if (ctlSub == 2) {
    if (hit(L_ON, x, y)) sendCmd("led", f.ledOn ? "{\"on\":false}" : "{\"on\":true}", "LED");
    else if (hit(L_MINUS, x, y) && f.ledLevel > 1) sendCmd("led", String("{\"level\":") + (f.ledLevel - 1) + "}", "LED level");
    else if (hit(L_PLUS, x, y) && f.ledLevel < 4) sendCmd("led", String("{\"level\":") + (f.ledLevel + 1) + "}", "LED level");
    else {
      for (int i = 0; i < 4; i++)
        if (hit(lMode(i), x, y)) { sendCmd("led", String("{\"on\":true,\"mode\":") + i + "}", "LED mode"); return; }
      for (int i = 0; i < 8; i++)
        if (hit(lColor(i), x, y)) { sendCmd("led", String("{\"on\":true,\"color\":") + i + "}", "LED colour"); return; }
      return;
    }
  } else if (ctlSub == 3) {
    if (hit(D_SCAN, x, y)) sendCmd("bms_scan", "{}", "Scan BMS");
    else if (hit(D_AUTO, x, y)) sendCmd("bms_auto", "{}", "Auto connect");
    else if (hit(D_DISC, x, y)) ask("Disconnect the BMS?", dp->name, "bms_disconnect", "{}", "Disconnect");
    else if (hit(D_RST, x, y)) ask("Restart the board?", "Back online in about 20 s", "restart", "{}", "Restart");
    else if (hit(D_TX, x, y)) sendCmd("radio_max", f.tx ? "{\"on\":false}" : "{\"on\":true}", "TX power");
    else if (hit(D_AP, x, y)) sendCmd("wifi_ap", f.ap ? "{\"on\":false}" : "{\"on\":true}", "Hotspot");
    else if (f.devs.size() > 2 && hit(PP_PREV, x, y)) devPage++;
    else {
      for (int i = 0; i < 2; i++) {
        size_t k = devPage * 2 + i;
        if (k < f.devs.size() && hit(dRow(i), x, y)) {
          const BleDev &b = f.devs[k];
          ask(String("Connect to ") + (b.name.length() ? b.name : b.mac), b.mac + "  protocol AUTO", "bms_connect",
              String("{\"mac\":\"") + b.mac + "\",\"type\":" + b.type + ",\"protocol\":\"AUTO\"}", "Connect");
          return;
        }
      }
      return;
    }
  } else {
    return;
  }
  { Lock l; syncNow = true; }
}

// -------------------------------------------------------- SETUP page ----
static const Btn S_BL_MINUS = {150, 46, 52, 40}, S_BL_PLUS = {300, 46, 52, 40}, S_ROTATE = {370, 46, 104, 40},
                 S_SOC_MINUS = {150, 138, 52, 40}, S_SOC_PLUS = {270, 138, 52, 40}, S_BEEP = {340, 138, 134, 40},
                 S_T_MINUS = {150, 184, 52, 40}, S_T_PLUS = {270, 184, 52, 40}, S_OFF = {340, 184, 134, 40},
                 S_WIFI = {8, 230, 88, 42}, S_PAIR = {101, 230, 88, 42}, S_SHOW = {194, 230, 92, 42},
                 S_CAL = {291, 230, 92, 42}, S_RESTART = {388, 230, 86, 42};
static Btn sleepBtn(int i) { return {150 + i * 82, 92, 76, 40}; }
static String setupKey;
static void valueBox(int x, int y, const String &v) {
  tft.fillRect(x, y, 64, 40, C_BG);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(v, x + 32, y + 21);
}
static void applyBacklight() { tft.setBrightness(BL_LEVEL[dcfg.bl]); }
static bool wantCalibrate = false, wantRotate = false;
static void setupPage(bool force) {
  String key = String(int(dcfg.bl)) + int(dcfg.sleep) + int(dcfg.rot) + int(dcfg.lowSoc) + "/" + int(dcfg.hiTemp) + B(dcfg.beep) + B(dcfg.offAlarm) + B(dcfg.show) +
               B(WiFi.status() == WL_CONNECTED);
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
  label(12, 158, "Low SOC alarm");
  button(S_SOC_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  valueBox(204, 138, dcfg.lowSoc ? String(dcfg.lowSoc) + "%" : String("off"));
  button(S_SOC_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  button(S_BEEP, dcfg.beep ? "BEEP ON" : "BEEP OFF", dcfg.beep, C_GOOD);
  label(12, 204, "High temp alarm");
  button(S_T_MINUS, "-", false, 0, &fonts::FreeSansBold12pt7b);
  valueBox(204, 184, dcfg.hiTemp ? String(dcfg.hiTemp) + "C" : String("off"));
  button(S_T_PLUS, "+", false, 0, &fonts::FreeSansBold12pt7b);
  button(S_OFF, dcfg.offAlarm ? "OFFLINE ON" : "OFFLINE OFF", dcfg.offAlarm, C_GOOD);
  button(S_WIFI, "WI-FI", WiFi.status() == WL_CONNECTED, C_GOOD);
  button(S_PAIR, "PAIRING", false);
  button(S_SHOW, dcfg.show ? "SHOW ON" : "SHOW OFF", dcfg.show, C_GOOD);
  button(S_CAL, "CALIB", false);
  button(S_RESTART, "RESTART", false);
}

// --------------------------------------------- overlays: Wi-Fi + pairing ----
enum Overlay { OV_NONE, OV_WIFI, OV_KB, OV_PAIR };
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
static uint8_t kbLayer = 0;
static const char *const KB_ROWS[4][4] = {{"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"},
                                          {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
                                          {"1234567890", "!@#$%^&*()", "-_=+[]{};", ":'\".,?/"},
                                          {"1234567890", "~`|\\<>!@#$", "%^&*()-_=", "+;:'\",."}};
static const Btn U_CHECK = {302, 46, 84, 40};
static const Btn W_SCAN = {302, 46, 84, 40}, W_BACK = {390, 46, 84, 40}, W_UP = {430, 94, 44, 68}, W_DOWN = {430, 168, 44, 68};
static Btn netRow(int i) { return {12, 94 + i * 36, 412, 32}; }
static Btn kbKey(int row, int i) {
  static const int x0[4] = {6, 6, 29, 78};
  return {x0[row] + i * 47, 108 + row * 42, 43, 38};
}
static const Btn K_SHIFT = {6, 234, 66, 38}, K_DEL = {410, 234, 64, 38}, K_SYM = {6, 278, 72, 38},
                 K_SPACE = {82, 278, 184, 38}, K_CANCEL = {270, 278, 92, 38}, K_OK = {366, 278, 108, 38};
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
  while (t.length() && tft.textWidth(t) > 440) t = t.substring(1);
  tft.drawString(t + "_", 16, 83);
}
static void kbDraw() {
  tft.fillRect(0, 41, 480, 320 - 41, C_BG);
  label(8, 52, String("Password for  ") + kbSsid);
  kbDrawText();
  for (int r = 0; r < 4; r++) {
    const char *row = KB_ROWS[kbLayer][r];
    for (int i = 0; row[i]; i++) button(kbKey(r, i), String(row[i]), false, 0, &fonts::FreeSans12pt7b);
  }
  button(K_SHIFT, kbLayer >= 2 ? "#+=" : (kbLayer ? "abc" : "ABC"), kbLayer == 1 || kbLayer == 3);
  button(K_DEL, "DEL", false);
  button(K_SYM, kbLayer >= 2 ? "abc" : "?123", false);
  button(K_SPACE, "space", false);
  button(K_CANCEL, "CANCEL", false);
  button(K_OK, "CONNECT", true, C_GOOD);
}
static void wifiOverlayDraw(bool force) {
  bool conn = WiFi.status() == WL_CONNECTED;
  String scan = netScanJson();
  String key = scan + staSsid + B(conn) + netPage;
  if (!force && key == ovKey) return;
  ovKey = key;
  if (scan.length()) parseNets(scan);
  clearContent();
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(conn ? C_GOOD : C_TEXT, C_BG);
  tft.drawString(conn ? "Connected: " + staSsid : String(staSsid.length() ? "Connecting: " + staSsid : "Pick your Wi-Fi"), 12, 48);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(conn ? WiFi.localIP().toString() + "   " + WiFi.RSSI() + " dBm" : String("This screen needs internet"), 12, 68);
  button(W_SCAN, scan.length() ? "SCAN" : "...", !scan.length());
  button(W_BACK, "BACK", false);
  if (!scan.length()) { label(20, 140, "Scanning for Wi-Fi networks..."); return; }
  if (nets.empty()) { label(20, 140, "No networks found. Tap SCAN."); return; }
  int pages = (nets.size() + 3) / 4;
  if (netPage >= pages) netPage = pages - 1;
  for (int i = 0; i < 4; i++) {
    size_t k = netPage * 4 + i;
    if (k >= nets.size()) break;
    Btn r = netRow(i);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, 8, C_CARD);
    if (nets[k].ssid == staSsid) tft.drawRoundRect(r.x, r.y, r.w, r.h, 8, conn ? C_GOOD : C_WARN);
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
static void pairOverlayDraw(const Model &m, bool force) {
  String key = m.status + B(m.paired) + m.name + B(WiFi.status() == WL_CONNECTED);
  if (!force && key == ovKey) return;
  ovKey = key;
  clearContent();
  button(W_BACK, "BACK", false);
  button(U_CHECK, "UPDATE", false);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::top_left);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(m.paired ? "PAIRED AS  " + m.name : String("PAIR CODE"), 12, 52);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextColor(m.paired ? C_GOOD : C_ACCENT, C_BG);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.drawString(pairCode, 12, 104);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(String("Website: ") + CLOUD_SITE, 12, 150);
  tft.drawString(m.paired ? String("Remove it on the website to pair with another account.")
                          : String("Sign in there, tap Add device, enter the code."), 12, 174);
  tft.setTextColor(C_MUTED, C_BG);
  tft.drawString(String("Cloud: ") + m.status, 12, 206);
  tft.drawString(String("Firmware ") + BATTLAB_FW + "   IP " + WiFi.localIP().toString() + "   RAM " +
                     (ESP.getFreeHeap() / 1024) + "k",
                 12, 232);
}
static bool overlayTouch(int x, int y) {
  if (overlay == OV_PAIR) {
    if (hit(W_BACK, x, y)) { overlay = OV_NONE; return true; }
    if (hit(U_CHECK, x, y)) { Lock l; checkUpdate = true; netToast = "Checking for a new firmware..."; }
    return false;
  }
  if (overlay == OV_WIFI) {
    if (hit(W_BACK, x, y)) { overlay = OV_NONE; return true; }
    if (hit(W_SCAN, x, y)) { netScanStart(); ovKey = ""; return false; }
    if (nets.size() > 4 && hit(W_UP, x, y)) { if (netPage > 0) netPage--; ovKey = ""; return false; }
    if (nets.size() > 4 && hit(W_DOWN, x, y)) { if ((netPage + 1) * 4 < (int)nets.size()) netPage++; ovKey = ""; return false; }
    for (int i = 0; i < 4; i++) {
      size_t k = netPage * 4 + i;
      if (k < nets.size() && hit(netRow(i), x, y)) {
        if (nets[k].lock) {
          kbSsid = nets[k].ssid;
          kbText = "";
          kbLayer = 0;
          overlay = OV_KB;
          kbDraw();
        } else {
          netSetHomeWifi(nets[k].ssid, "");
          showToast("Connecting to " + nets[k].ssid);
          ovKey = "";
        }
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
          if (kbLayer == 1) { kbLayer = 0; kbDraw(); } else kbDrawText();
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
static void setupTouch(int x, int y) {
  bool changed = true;
  if (hit(S_BL_MINUS, x, y)) { if (dcfg.bl > 1) dcfg.bl--; applyBacklight(); }
  else if (hit(S_BL_PLUS, x, y)) { if (dcfg.bl < 5) dcfg.bl++; applyBacklight(); }
  else if (hit(S_ROTATE, x, y)) { wantRotate = true; changed = false; }
  else if (hit(S_SOC_MINUS, x, y)) dcfg.lowSoc = dcfg.lowSoc >= 5 ? dcfg.lowSoc - 5 : 0;
  else if (hit(S_SOC_PLUS, x, y)) dcfg.lowSoc = dcfg.lowSoc <= 45 ? dcfg.lowSoc + 5 : 50;
  else if (hit(S_T_MINUS, x, y)) dcfg.hiTemp = dcfg.hiTemp > 30 ? dcfg.hiTemp - 5 : 0;
  else if (hit(S_T_PLUS, x, y)) dcfg.hiTemp = dcfg.hiTemp == 0 ? 30 : dcfg.hiTemp < 90 ? dcfg.hiTemp + 5 : 90;
  else if (hit(S_BEEP, x, y)) { dcfg.beep = !dcfg.beep; if (dcfg.beep) beep(1); }
  else if (hit(S_OFF, x, y)) dcfg.offAlarm = !dcfg.offAlarm;
  else if (hit(S_SHOW, x, y)) { dcfg.show = !dcfg.show; showToast(dcfg.show ? "Show mode: HOME rotates through all batteries" : "Show mode off"); }
  else if (hit(S_WIFI, x, y)) { overlay = OV_WIFI; ovKey = ""; if (!netScanJson().length() || nets.empty()) netScanStart(); changed = false; }
  else if (hit(S_PAIR, x, y)) { overlay = OV_PAIR; ovKey = ""; changed = false; }
  else if (hit(S_CAL, x, y)) { wantCalibrate = true; changed = false; }
  else if (hit(S_RESTART, x, y)) { showToast("Restarting..."); delay(300); ESP.restart(); }
  else {
    changed = false;
    for (int i = 0; i < 4; i++)
      if (hit(sleepBtn(i), x, y)) { dcfg.sleep = i; changed = true; }
  }
  if (changed) dcfgSave();
}

// ------------------------------------------------- calibrate + splash ----
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
static void splash() {
  const uint32_t SPLASH_MS = 3500;
  tft.setBrightness(0);
  tft.fillScreen(TFT_BLACK);
  const int lx = (480 - LOGO_W) / 2, ly = 86;
  static lgfx::rgb565_t line[LOGO_W];
  for (int y = 0; y < LOGO_H; y++) {
    const uint8_t *row = LOGO_GRAY + size_t(y) * LOGO_W;
    for (int x = 0; x < LOGO_W; x++) line[x] = lgfx::rgb565_t(row[x], row[x], row[x]);
    tft.pushImage(lx, ly + y, LOGO_W, 1, line);
  }
  tft.setFont(&fonts::FreeSansBold9pt7b);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(C_ACCENT, TFT_BLACK);
  tft.drawString("CLOUD SCREEN", 240, 238);
  const uint16_t barBg = tft.color565(40, 40, 40);
  const int bx = 140, by = 262, bw = 200;
  tft.fillRoundRect(bx, by, bw, 4, 2, barBg);
  uint32_t t0 = millis();
  int drawn = 0;
  for (;;) {
    uint32_t t = std::min<uint32_t>(millis() - t0, SPLASH_MS);
    uint32_t level = BL_LEVEL[dcfg.bl];
    tft.setBrightness(t < 700 ? level * t / 700 : level);
    int w = bw * t / SPLASH_MS;
    if (w > drawn) { tft.fillRoundRect(bx, by, w, 4, 2, C_ACCENT); drawn = w; }
    if (t >= SPLASH_MS) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  tft.fillScreen(C_BG);
}

// --------------------------------------------------------- screen task ----
static void moveFocus(const Model &m, int dir) {
  if (m.devs.empty()) return;
  int idx = 0;
  for (size_t i = 0; i < m.devs.size(); i++)
    if (m.devs[i].id == focusId) idx = i;
  idx = (idx + dir + m.devs.size()) % m.devs.size();
  {
    Lock l;
    focusId = m.devs[idx].id;
    syncNow = true;
  }
  saveFocus(focusId);
  for (auto &p : pendSet) p = false;
  showToast(m.devs[idx].name, 1500);
}
static void uiTask(void *) {
  splash();
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
  int page = P_ALL, drawnPage = -1;
  uint32_t lastDraw = 0, lastTouch = 0, lastActivity = millis();
  bool wasDown = false, asleep = false;
  String alarmWas;
  Model m;
  uint32_t lastCopy = 0;
  if (!staSsid.length()) {
    overlay = OV_WIFI;
    netScanStart();
  }
  int otaShown = -2;
  for (;;) {
    if (otaPct >= 0) {
      int pct = otaPct;
      if (otaShown == -2) {
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
      if (pct != otaShown) {
        otaShown = pct;
        if (pct > 0) tft.fillRoundRect(60, 156, 360 * pct / 100, 18, 9, C_ACCENT);
        tft.fillRect(200, 178, 80, 22, C_BG);
        tft.setFont(&fonts::FreeSansBold9pt7b);
        tft.setTextColor(C_TEXT, C_BG);
        tft.drawString(String(pct) + "%", 240, 189);
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (otaShown != -2) { otaShown = -2; drawnPage = -1; }
    uint32_t now = millis();
    if (now - lastCopy > 250) {
      lastCopy = now;
      Lock l;
      m = M;
      if (netToast.length()) { showToast(netToast, 3500); netToast = ""; }
      fastWanted = page == P_CTRL;
      // first battery becomes the focus
      if (M.paired && !M.devs.empty()) {
        bool found = false;
        for (auto &d : M.devs) found |= d.id == focusId;
        if (!found) { focusId = M.devs[0].id; syncNow = true; }
      }
    }
    lgfx::touch_point_t tp;
    bool down = tft.getTouch(&tp) > 0;
    if (down && !wasDown && now - lastTouch > 220) {
      lastTouch = now;
      lastActivity = now;
      if (asleep) {
        asleep = false;
        applyBacklight();
      } else if (overlay == OV_KB) {
        if (overlayTouch(tp.x, tp.y)) drawnPage = -1;
      } else if (dlg.active) {
        if (hit(C_CANCEL, tp.x, tp.y)) { dlg.active = false; showToast("Cancelled"); drawnPage = -1; }
        else if (hit(C_OK, tp.x, tp.y)) {
          dlg.active = false;
          sendCmd(dlg.cmd, dlg.args, dlg.label);
          if (dlg.cmd == "bms_param") for (auto &p : pendSet) p = false;
          showToast("Sending: " + dlg.label);
          drawnPage = -1;
        }
      } else if (overlay != OV_NONE && tp.y > 42 && tp.y < TAB_Y) {
        if (overlayTouch(tp.x, tp.y)) drawnPage = -1;
        lastDraw = 0;
      } else if (tp.y >= TAB_Y) {
        int t = tp.x / TAB_W;
        if (t >= TABS) t = TABS - 1;
        if (t >= 0) page = t;
        if (overlay != OV_NONE) { overlay = OV_NONE; drawnPage = -1; }
      } else if (tp.y < 40) {
        bool nav = page != P_ALL && page != P_SETUP && m.devs.size() > 1;
        bool pager = page == P_ALL && m.devs.size() > 6;
        if (hit(B_PREV, tp.x, tp.y) || hit(B_NEXT, tp.x, tp.y)) {
          int dir = hit(B_NEXT, tp.x, tp.y) ? 1 : -1;
          if (nav) { moveFocus(m, dir); drawnPage = -1; }
          else if (pager) {
            int pages = (m.devs.size() + 5) / 6;
            allPage = (allPage + dir + pages) % pages;
            drawnPage = -1;
          }
        }
      } else {
        if (page == P_ALL && m.paired) {
          for (int k = 0; k < 6; k++) {
            size_t i = allPage * 6 + k;
            if (i < m.devs.size() && hit(cardBtn(k), tp.x, tp.y)) {
              {
                Lock l;
                focusId = m.devs[i].id;
                syncNow = true;
              }
              saveFocus(focusId);
              for (auto &p : pendSet) p = false;
              page = P_HOME;
            }
          }
        } else if (page == P_HOME && (bigMode || (tp.x < 210 && tp.y < 228))) {
          bigMode = !bigMode;
          drawnPage = -1;
        } else if (page == P_GRAPH) {
          for (int i = 0; i < 5; i++)
            if (hit(gBtn(i), tp.x, tp.y)) gMetric = i;
          if (hit(G_RANGE_B, tp.x, tp.y)) gRange = (gRange + 1) % 3;
        } else if (page == P_CTRL) ctlTouch(m, tp.x, tp.y);
        else if (page == P_SETUP) {
          setupTouch(tp.x, tp.y);
          if (overlay != OV_NONE) drawnPage = -1;
        }
        lastDraw = 0;
      }
    }
    // hold -/+ on parameters to repeat
    static uint32_t pressStart = 0, lastRepeat = 0;
    if (down && !wasDown) pressStart = now;
    if (down && wasDown && !asleep && !dlg.active && page == P_CTRL && ctlSub == 1 && now - pressStart > 500 &&
        now - lastRepeat > 110) {
      for (int r = 0; r < PROWS; r++)
        if (hit(pMinus(r), tp.x, tp.y) || hit(pPlus(r), tp.x, tp.y)) {
          lastRepeat = now;
          ctlTouch(m, tp.x, tp.y);
          lastDraw = 0;
        }
    }
    wasDown = down;
    // show mode: HOME steps through every battery (wall display)
    static uint32_t showAt = 0;
    if (dcfg.show && page == P_HOME && !dlg.active && overlay == OV_NONE && m.devs.size() > 1 && now - lastTouch > 15000 &&
        now - showAt > 8000) {
      showAt = now;
      moveFocus(m, 1);
      drawnPage = -1;
    }
    if (page == P_HOME && drawnPage == P_HOME && !asleep && overlay == OV_NONE && !dlg.active) flowAnim(focusDev(m));
    if (wantCalibrate || wantRotate) {
      if (wantRotate) {
        dcfg.rot = dcfg.rot == 1 ? 3 : 1;
        dcfgSave();
        tft.setRotation(dcfg.rot);
      }
      calibrate();
      wantCalibrate = wantRotate = false;
      drawnPage = -1;
    }
    uint32_t sleepMs = uint32_t(SLEEP_MIN[dcfg.sleep]) * 60000u;
    if (!asleep && sleepMs && now - lastActivity > sleepMs && !alarmText.length()) {
      asleep = true;
      tft.setBrightness(0);
    }
    if (!dlg.active && overlay != OV_KB && (page != drawnPage || now - lastDraw > 500)) {
      bool force = page != drawnPage;
      evalAlarm(m);
      if (alarmText.length() && alarmText != alarmWas) {
        lastActivity = now;
        if (asleep) { asleep = false; applyBacklight(); }
        if (dcfg.beep) beep(3);
      }
      alarmWas = alarmText;
      if (force) {
        clearContent();
        tabs(page);
        allKey = homeKey = cellsKey = graphKey = ctlKey = setupKey = ovKey = "";
      }
      if (!asleep) {
        header(m, page);
        if (overlay == OV_WIFI) wifiOverlayDraw(force);
        else if (overlay == OV_PAIR) pairOverlayDraw(m, force);
        else switch (page) {
            case P_ALL: allPageDraw(m, force); break;
            case P_HOME: homePage(m, force); break;
            case P_CELLS: cellsPage(m, force); break;
            case P_GRAPH: graphPage(m, force); break;
            case P_CTRL: ctlPage(m, force); break;
            default: setupPage(force); break;
          }
      }
      drawnPage = page;
      lastDraw = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void setup() {
  Serial.begin(115200);
  {
    esp_reset_reason_t rr = esp_reset_reason();
    bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
    Preferences b;
    b.begin("battlab-boot", false);
    uint8_t fails = crash ? b.getUChar("fails", 0) + 1 : 0;
    b.putUChar("fails", fails);
    b.end();
    const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t desc;
    if (fails >= 3 && other && esp_ota_get_partition_description(other, &desc) == ESP_OK &&
        esp_ota_set_boot_partition(other) == ESP_OK) {
      Preferences c;
      c.begin("battlab-boot", false);
      c.putUChar("fails", 0);
      c.end();
      ESP.restart();
    }
  }
  mx = xSemaphoreCreateMutex();
  dcfgLoad();
  netLoad();
  Preferences p;
  p.begin("battlab-disp", true);
  bool invert = p.getBool("inv", false), rgb = p.getBool("rgb", false);
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
  wifiApply();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
  Serial.printf("\nBATTLAB " BATTLAB_FW "\nPair code %s\n", pairCode.c_str());
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 1, nullptr, 1);
}
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
  static bool bootOk = false;
  if (!bootOk && millis() > 60000) {
    bootOk = true;
    Preferences b;
    b.begin("battlab-boot", false);
    b.putUChar("fails", 0);
    b.end();
  }
}
