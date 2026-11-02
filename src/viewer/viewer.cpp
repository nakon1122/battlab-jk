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
#include "bms_settings.h"
#include "cloud_config.h"
#include "cloud_ca_bundle.h"
#include "improv_serial.h"
#include <string>

#define BATTLAB_FW "2.8.2-cloudscreen"

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
  int cyc = -1, bank = 0; // bank = packs on the RS485 bus of this board
  String group;            // group set on the website ("" = none)
  float ein = NAN, eout = NAN; // energy today kWh, counted on the board
  uint8_t pf = 0;          // usage profile from the website: 0 solar, 1 EV bike, 2 EV car, 3 UPS, 4 camp
  int whkm = 0, rangeAlarm = 10;
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
struct PackSum { // one JK pack of an RS485 bank (focused board only)
  int a = 0, n = 0;
  long age = 0;
  float soc = NAN, v = NAN, i = NAN, t = NAN, mos = NAN;
  bool chg = false, dsg = false;
  float cells[32];
  bool jkValid = false, sw[3] = {false, false, false}, bt = false; // its settings, and whether the board has its Bluetooth
  float jk[bmsw::JK_PARAM_COUNT];
  String btName;
};
struct Focus {
  String id;
  std::vector<PackSum> packs;
  uint8_t busm = 0;
  String btm; // Bluetooth auto-match progress
  float cells[32];
  int n = 0;
  bool jkValid = false;
  float jk[bmsw::JK_PARAM_COUNT];
  float pv[bset::MAX_N]; // ANT / JBD settings ("pa")
  uint64_t pvHave = 0;
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
// Improv Wi-Fi over USB serial: the website's USB installer asks for the home
// Wi-Fi right after flashing (no phone needed), then opens the website with the
// pair code so the screen is added to the account in one click.
static String hwMac() {
  uint64_t mac = ESP.getEfuseMac();
  char hm[18];
  snprintf(hm, sizeof hm, "%02X:%02X:%02X:%02X:%02X:%02X", unsigned(mac & 0xff), unsigned(mac >> 8 & 0xff), unsigned(mac >> 16 & 0xff),
           unsigned(mac >> 24 & 0xff), unsigned(mac >> 32 & 0xff), unsigned(mac >> 40 & 0xff));
  return hm;
}
static void improvSend(uint8_t type, const std::vector<uint8_t> &d) {
  auto p = improv::packet(type, d);
  Serial.write(p.data(), p.size());
  Serial.write('\n');
  Serial.flush();
}
static void improvTask(void *) {
  improv::Parser ip;
  String hw = hwMac();
  std::string name = std::string("BATTLAB-SCREEN-") + (hw.substring(9, 11) + hw.substring(12, 14) + hw.substring(15, 17)).c_str();
  auto url = [&]() { return std::string("https://") + CLOUD_SITE + "/#claimv/" + pairCode.c_str(); };
  for (;;) {
    while (Serial.available()) {
      uint8_t type;
      std::vector<uint8_t> d;
      if (!ip.feed(Serial.read(), type, d) || type != improv::T_RPC || d.empty()) continue;
      bool conn = WiFi.status() == WL_CONNECTED;
      if (d[0] == improv::C_STATE) {
        improvSend(improv::T_STATE, {uint8_t(conn ? improv::S_PROVISIONED : improv::S_AUTHORIZED)});
        if (conn) improvSend(improv::T_RESULT, improv::result(improv::C_STATE, {url()}));
      } else if (d[0] == improv::C_INFO) {
        improvSend(improv::T_RESULT, improv::result(improv::C_INFO, {"BATTLAB Cloud Screen", BATTLAB_FW, "ESP32", name}));
      } else if (d[0] == improv::C_SCAN) {
        int n = -1;
        for (int k = 0; k < 3 && n < 0; k++) { n = WiFi.scanNetworks(false, false); if (n < 0) vTaskDelay(pdMS_TO_TICKS(800)); }
        for (int i = 0; i < n && i < 20; i++)
          improvSend(improv::T_RESULT, improv::result(improv::C_SCAN, {WiFi.SSID(i).c_str(), std::to_string(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"}));
        if (n >= 0) WiFi.scanDelete();
        improvSend(improv::T_RESULT, improv::result(improv::C_SCAN, {}));
      } else if (d[0] == improv::C_WIFI) {
        std::string ss, pw;
        if (!improv::wifiSettings(d, ss, pw) || ss.size() > 32 || pw.size() > 64) { improvSend(improv::T_ERROR, {improv::E_INVALID}); continue; }
        improvSend(improv::T_STATE, {improv::S_PROVISIONING});
        netSetHomeWifi(String(ss.c_str()), String(pw.c_str()));
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(250));
        if (WiFi.status() == WL_CONNECTED) {
          improvSend(improv::T_STATE, {improv::S_PROVISIONED});
          improvSend(improv::T_RESULT, improv::result(improv::C_WIFI, {url()}));
        } else {
          improvSend(improv::T_ERROR, {improv::E_CONNECT});
          improvSend(improv::T_STATE, {improv::S_AUTHORIZED});
        }
      } else improvSend(improv::T_ERROR, {improv::E_UNKNOWN_RPC});
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
// A scan fails while the station keeps retrying a network that is not there
// (ESP_ERR_WIFI_STATE -> "no networks"). Pause the retries, scan, resume.
static bool scanResume = false;
static uint8_t scanTries = 0;
static void netScanStart() {
  wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (m == WIFI_AP) WiFi.mode(WIFI_AP_STA); // keep the hotspot running
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
    scanResume = true;
    delay(150);
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true); // async, include hidden networks
}
static void scanFinished() {
  if (!scanResume) return;
  scanResume = false;
  if (staSsid.length()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
}
static String netScanJson() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return "";
  if (n == WIFI_SCAN_FAILED && scanResume && scanTries < 2) { // failed: try again once
    scanTries++;
    WiFi.scanNetworks(true, true);
    return "";
  }
  scanTries = 0;
  scanFinished();
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
    s.bank = x["bank"] | 0;
    s.group = o["g"] | "";
    s.ein = jf(x["ein"]);
    s.eout = jf(x["eout"]);
    JsonObjectConst pf = o["pf"].as<JsonObjectConst>();
    if (!pf.isNull()) {
      String t = pf["type"] | "solar";
      s.pf = t == "ev" ? 1 : t == "car" ? 2 : t == "ups" ? 3 : t == "camp" ? 4 : 0;
      s.whkm = pf["whkm"] | 0;
      s.rangeAlarm = pf["rangeAlarm"] | 10;
    }
    devs.push_back(s);
  }
  Focus f;
  f.id = d["focus"] | "";
  JsonObjectConst x = d["f"].as<JsonObjectConst>();
  for (float v : x["cells"].as<JsonArrayConst>())
    if (f.n < 32) f.cells[f.n++] = v;
  f.busm = x["busm"] | 0;
  f.btm = x["cfg"]["btm"] | "";
  for (JsonObjectConst o : x["packs"].as<JsonArrayConst>()) {
    if (f.packs.size() >= 16) break;
    PackSum k;
    k.a = o["a"] | 0;
    k.age = o["age"] | 0L;
    k.soc = jf(o["soc"]);
    k.v = jf(o["v"]);
    k.i = jf(o["i"]);
    k.t = jf(o["t1"]);
    k.mos = jf(o["mos"]);
    k.chg = o["chg"] | false;
    k.dsg = o["dsg"] | false;
    for (float v : o["cells"].as<JsonArrayConst>())
      if (k.n < 32) k.cells[k.n++] = v;
    JsonObjectConst pj = o["jk"].as<JsonObjectConst>();
    if (!pj.isNull()) {
      int q = 0;
      for (float v : pj["v"].as<JsonArrayConst>()) if (q < bmsw::JK_PARAM_COUNT) k.jk[q++] = v;
      k.jkValid = q == bmsw::JK_PARAM_COUNT;
      q = 0;
      for (bool b : pj["sw"].as<JsonArrayConst>()) if (q < 3) k.sw[q++] = b;
    } else { k.sw[0] = k.chg; k.sw[1] = k.dsg; }
    JsonObjectConst bt = x["cfg"]["bt"][String(k.a)].as<JsonObjectConst>();
    if (!bt.isNull()) { k.bt = true; k.btName = bt["name"] | ""; }
    f.packs.push_back(k);
  }
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
  JsonObjectConst pa = x["pa"].as<JsonObjectConst>();
  if (!pa.isNull()) {
    int k = 0;
    for (JsonVariantConst v : pa["v"].as<JsonArrayConst>()) {
      if (k >= bset::MAX_N) break;
      if (!v.isNull()) { f.pv[k] = v.as<float>(); f.pvHave |= uint64_t(1) << k; }
      k++;
    }
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
      if ((d.pf == 1 || d.pf == 2) && d.whkm > 0 && !std::isnan(d.cap) && d.rangeAlarm > 0) {
        float rk = d.cap * d.v / d.whkm;
        if (rk < d.rangeAlarm) { alarmText = nm + " RANGE " + int(rk + .5f) + "km"; return; }
      }
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
static void allView(const Model &m, std::vector<int> &idx, bool &grouped, int &per);
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
  std::vector<int> vidx;
  bool vgrp;
  int vper;
  allView(m, vidx, vgrp, vper);
  bool pager = page == P_ALL && (int)vidx.size() > vper;
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
  extern uint32_t regenUntil;
  extern float regenKw;
  if (millis() < regenUntil && !alarmText.length()) { // EV braking puts energy back
    String r = String("REGEN +") + fmt(regenKw, 1) + " kW";
    int w = std::min(right - 138, int(s.textWidth(r)) + 24);
    s.fillRoundRect(136, 7, w, 26, 13, C_GOOD);
    s.setTextColor(C_BG, C_GOOD);
    s.drawString(r, 148, 21);
  } else if (alarmText.length() && millis() >= toastUntil) {
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
// ---- groups (set on the website): chips on top, totals line, 4 cards per page
static int gSel = -1; // -1 = every battery
static std::vector<String> groupsOf(const Model &m) {
  std::vector<String> g;
  for (auto &d : m.devs)
    if (d.group.length() && std::find(g.begin(), g.end(), d.group) == g.end()) g.push_back(d.group);
  return g;
}
static String groupLabel(const String &g, int i) { // the screen font has no Thai: fall back to G1, G2...
  for (unsigned k = 0; k < g.length(); k++)
    if ((uint8_t)g[k] >= 128 || (uint8_t)g[k] < 32) return String("G") + (i + 1);
  return g.length() > 9 ? g.substring(0, 9) : g;
}
static void allView(const Model &m, std::vector<int> &idx, bool &grouped, int &per) {
  auto G = groupsOf(m);
  grouped = !G.empty();
  if (gSel >= int(G.size())) gSel = -1;
  idx.clear();
  for (size_t i = 0; i < m.devs.size(); i++)
    if (!grouped || gSel < 0 || m.devs[i].group == G[gSel]) idx.push_back(i);
  per = grouped ? 4 : 6;
}
static Btn cardBtnG(int k, bool grouped) { return grouped ? Btn{8 + (k % 2) * 236, 104 + (k / 2) * 86, 228, 80} : cardBtn(k); }
static Btn gChip(int i, int n) { int w = std::min(116, 464 / (n + 1)); return {8 + i * w, 44, w - 4, 28}; }
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
  if (d.bank > 1) nm = String("x") + d.bank + " " + nm; // RS485 bank of packs
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
  String key = String(B(m.paired)) + m.status + allPage + focusId + int(dcfg.lowSoc) + int(dcfg.hiTemp) + gSel;
  for (auto &d : m.devs) key += d.id + d.name + d.group + d.age / 30 + fmt(d.soc, 0) + fmt(d.v, 1) + fmt(d.i, 1) + fmt(d.t1, 0) + d.state + fmt(d.ein, 1) + fmt(d.eout, 1);
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
  std::vector<int> idx;
  bool grouped;
  int per;
  allView(m, idx, grouped, per);
  if (grouped) {
    auto G = groupsOf(m);
    int n = std::min<int>(G.size(), 6);
    button(gChip(0, n), "ALL", gSel < 0, 0, &fonts::FreeSansBold9pt7b);
    for (int i = 0; i < n; i++) button(gChip(i + 1, n), groupLabel(G[i], i), gSel == i, 0, &fonts::FreeSansBold9pt7b);
    // totals of the shown group
    float pw = 0, kwh = 0, ei = 0, eo = 0;
    int live = 0;
    bool haveE = false;
    for (int i : idx) {
      const DevSum &d = m.devs[i];
      if (!d.live()) continue;
      live++;
      if (!std::isnan(d.p)) pw += d.p;
      if (!std::isnan(d.cap) && !std::isnan(d.v)) kwh += d.cap * d.v / 1000;
      if (!std::isnan(d.ein)) { ei += d.ein; eo += d.eout; haveE = true; }
    }
    tft.fillRoundRect(8, 76, 464, 24, 8, C_CARD);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(C_TEXT, C_CARD);
    String t = String(idx.size()) + " bat  " + (pw > 0 ? "+" : "") + fmt(pw / 1000, 2) + "kW  " + fmt(kwh, 1) + "kWh left";
    if (haveE) t += "  today +" + fmt(ei, 1) + "/-" + fmt(eo, 1);
    tft.drawString(t, 16, 89);
  }
  int pages = std::max<int>(1, (idx.size() + per - 1) / per);
  if (allPage >= pages) allPage = pages - 1;
  for (int k = 0; k < per; k++) {
    size_t j = allPage * per + k;
    if (j >= idx.size()) break;
    card(cardBtnG(k, grouped), m.devs[idx[j]], m.devs[idx[j]].id == focusId);
  }
  if (pages > 1 && !grouped) {
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
static void tileDraw(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit, uint16_t accent,
                     const String &old, int roll);
static void tile(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit, uint16_t accent) {
  static int keys[12];
  static String vals[12];
  static int nk = 0;
  int key = x * 1000 + y, slot = -1;
  for (int i = 0; i < nk; i++) if (keys[i] == key) slot = i;
  if (slot < 0 && nk < 12) { slot = nk++; keys[slot] = key; vals[slot] = ""; }
  String old = slot >= 0 ? vals[slot] : String("");
  if (slot >= 0) vals[slot] = value;
  if (old.length() && old != value && value != "--" && old != "--")
    for (int r = 1; r <= 4; r++) tileDraw(x, y, w, h, lbl, value, unit, accent, old, r);
  else tileDraw(x, y, w, h, lbl, value, unit, accent, "", 0);
}
static void tileDraw(int x, int y, int w, int h, const char *lbl, const String &value, const char *unit, uint16_t accent,
                     const String &old, int roll) {
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
// under the SOC number: pulsing bolt + chevrons with a moving light
static uint16_t blend565(uint16_t a, uint16_t b, float k) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return uint16_t(int(ar + (br - ar) * k) << 11 | int(ag + (bg - ag) * k) << 5 | int(ab + (bb - ab) * k));
}
static void flowAnim(const DevSum *d) {
  static uint32_t last = 0;
  if (bigMode || !d || millis() - last < 60) return;
  last = millis();
  int dir = !d->live() ? 0 : d->i > 0.3f ? 1 : d->i < -0.3f ? -1 : 0;
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(96, 20)) return;
  s.fillSprite(C_BG);
  if (dir == 0) s.fillRoundRect(24, 8, 48, 4, 2, C_LINE);
  else {
    uint16_t on = dir > 0 ? C_GOOD : C_WARN;
    float t = millis() / 1000.f;
    float pul = .55f + .45f * (0.5f + 0.5f * sinf(t * 6.f));
    uint16_t ic = blend565(C_BG, C_WARN, pul);
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
uint32_t regenUntil = 0;
float regenKw = 0;
static float evShown = 0, evPeak = 3000, evTarget = 0;
static bool evOn = false;
static void evMeter(float p) {
  const int cx = 150, cy = 214, r1 = 126, r0 = 108;
  float z = 180 + 180 * .2f;
  tft.fillArc(cx, cy, r0, r1, 180, 360, C_LINE);
  if (p > 1) tft.fillArc(cx, cy, r0, r1, z, z + (360 - z) * std::min(1.f, p / evPeak), C_WARN);
  else if (p < -1) tft.fillArc(cx, cy, r0, r1, z - (z - 180) * std::min(1.f, -p / (evPeak * .3f)), z, C_GOOD);
  LGFX_Sprite t(&tft);
  t.setColorDepth(8);
  if (!t.createSprite(160, 74)) return;
  t.fillSprite(C_BG);
  t.setTextDatum(textdatum_t::middle_center);
  t.setFont(&fonts::FreeSansBold24pt7b);
  t.setTextColor(p < -1 ? C_GOOD : C_TEXT, C_BG);
  t.drawString(fmt(p / 1000.f, 1), 80, 30);
  t.setFont(&fonts::FreeSans9pt7b);
  t.setTextColor(C_MUTED, C_BG);
  t.drawString(p < -1 ? "kW regen" : "kW", 80, 62);
  t.pushSprite(cx - 80, cy - 100);
  t.deleteSprite();
}
static void evHome(const DevSum &d, bool force) {
  bool ok = d.live();
  evTarget = ok ? -d.p : 0;
  evPeak = std::max(evPeak, std::fabs(evTarget));
  if (force) {
    clearContent();
    evShown = 0;
    evMeter(0);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString("REGEN", 30, 226);
    tft.drawString(String(int(evPeak / 1000 + .5f)) + " kW", 268, 226);
  }
  float rk = (ok && d.whkm > 0 && !std::isnan(d.cap)) ? d.cap * d.v / d.whkm : -1;
  LGFX_Sprite b(&tft);
  b.setColorDepth(8);
  if (!b.createSprite(170, 186)) return;
  b.fillSprite(C_BG);
  b.fillRoundRect(0, 0, 170, 186, 12, C_CARD);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextDatum(textdatum_t::top_left);
  b.setTextColor(C_MUTED, C_CARD);
  b.drawString("Range left", 12, 10);
  b.setFont(&fonts::FreeSansBold24pt7b);
  bool low = rk >= 0 && d.rangeAlarm && rk < d.rangeAlarm;
  b.setTextColor(low ? C_WARN : C_TEXT, C_CARD);
  b.drawString(rk < 0 ? String("--") : String(int(rk + .5f)), 12, 32);
  b.setFont(&fonts::FreeSans9pt7b);
  b.setTextColor(C_MUTED, C_CARD);
  b.drawString(rk < 0 ? (d.whkm ? "km (no capacity)" : "set Wh/km on web") : (d.whkm ? String("km  ") + d.whkm + " Wh/km" : String("km")), 12, 76);
  int soc = ok ? int(d.soc + .5f) : 0;
  b.drawString(String("SOC ") + (ok ? String(soc) + "%" : String("--")), 12, 100);
  b.fillRoundRect(12, 122, 146, 8, 4, C_LINE);
  if (soc > 0) b.fillRoundRect(12, 122, 146 * std::min(soc, 100) / 100, 8, 4, socColor(d.soc));
  b.drawString("Voltage", 12, 140);
  b.setFont(&fonts::FreeSansBold12pt7b);
  b.setTextColor(C_TEXT, C_CARD);
  b.drawString(ok ? fmt(d.v, 1) + " V" : String("--"), 12, 158);
  b.pushSprite(300, 46);
  b.deleteSprite();
  LGFX_Sprite st(&tft);
  st.setColorDepth(8);
  if (!st.createSprite(462, 40)) return;
  st.fillSprite(C_BG);
  st.fillRoundRect(0, 0, 462, 40, 10, C_CARD);
  st.setFont(&fonts::FreeSans9pt7b);
  st.setTextDatum(textdatum_t::middle_left);
  st.setTextColor(C_TEXT, C_CARD);
  st.drawString(ok ? d.name + "  " + fmt(d.t1, 0) + "C  " + fmt(d.i, 1) + "A" : d.name + "  " + devState(d), 12, 20);
  pill(st, 304, 7, "CHG", ok && d.chg);
  pill(st, 382, 7, "DSG", ok && d.dsg);
  st.pushSprite(9, 234);
  st.deleteSprite();
  static uint32_t lastDrive = 0;
  if (ok && d.i < -1) lastDrive = millis();
  if (ok && d.i > .5f && millis() - lastDrive < 8000) { regenUntil = millis() + 1500; regenKw = d.p / 1000; }
}
static void evTick() {
  static uint32_t last = 0;
  if (!evOn || millis() - last < 60) return;
  last = millis();
  float n = evShown + (evTarget - evShown) * .35f;
  if (std::fabs(n - evShown) < 5) return;
  evShown = n;
  evMeter(evShown);
}
static void pageWipe(int dir) {
  for (int k = 1; k <= 4; k++) {
    int x = dir > 0 ? 480 - k * 120 : (k - 1) * 120;
    tft.fillRect(x, 41, 120, TAB_Y - 41, C_BG);
    tft.fillRect(dir > 0 ? x : x + 117, 41, 3, TAB_Y - 41, C_ACCENT);
    vTaskDelay(pdMS_TO_TICKS(16));
  }
  clearContent();
}
static void alarmFlash() {
  tft.setFont(&fonts::FreeSans9pt7b);
  int w = std::min(330, int(tft.textWidth(alarmText)) + 24);
  for (int k = 0; k <= 4; k++) {
    int y = -28 + k * 9;
    tft.fillRect(136, 0, w, 40, C_BG);
    tft.fillRoundRect(136, y, w, 26, 13, C_BAD);
    tft.setTextColor(C_TEXT, C_BAD);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.drawString(alarmText, 148, y + 14);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  for (int i = 0; i < 6; i++) {
    uint16_t c = i % 2 ? C_BG : C_BAD;
    for (int k = 0; k < 4; k++) tft.drawRect(k, k, 480 - 2 * k, 320 - 2 * k, c);
    vTaskDelay(pdMS_TO_TICKS(110));
  }
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
  if (!dp) { evOn = false; noFocus(m); return; }
  const DevSum &d = *dp;
  if (bigMode) { evOn = false; bigHome(d); return; }
  static String evFor;
  if (d.pf == 1 || d.pf == 2) { // motorbike / car profile
    bool f = force || !evOn || evFor != d.id;
    evOn = true;
    evFor = d.id;
    evHome(d, f);
    return;
  }
  if (evOn) { evOn = false; force = true; clearContent(); }
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
  if (force && ok && soc > 0) // sweep 0 -> SOC when the page opens
    for (int k = 1; k <= 10; k++) {
      float e = k / 10.f;
      e = 1 - (1 - e) * (1 - e) * (1 - e);
      tft.fillArc(cx, cy, r0, r1, 135, 135 + 270.f * std::min(100.f, soc) * e / 100.f, socColor(soc));
      vTaskDelay(pdMS_TO_TICKS(30));
    }
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
static int packSel = -1; // RS485 bank: address of the pack shown on CELLS
static Btn packBtn(int k, int np) { int w = 462 / np; return {9 + k * w, 46, w - 4, 34}; }
static void cellsPage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  const auto &PK = m.f.packs;
  int np = std::min<int>(PK.size(), 8), ps = -1;
  if (np) {
    for (int k = 0; k < np; k++) if (PK[k].a == packSel) ps = k;
    if (ps < 0) { ps = 0; packSel = PK[0].a; }
  }
  const float *cells = np ? PK[ps].cells : m.f.cells;
  int n = dp && dp->live() ? (np ? PK[ps].n : m.f.n) : 0;
  String key = String(dp ? dp->id : "") + n + "p" + packSel;
  for (int k = 0; k < np; k++) key += int(PK[k].soc);
  for (int i = 0; i < n; i++) key += fmt(cells[i], 3);
  if (!force && key == cellsKey) return;
  cellsKey = key;
  clearContent();
  if (!dp) { noFocus(m); return; }
  if (np) { // pack selector: "#a 52%"
    for (int k = 0; k < np; k++) {
      Btn b = packBtn(k, np);
      bool sel = k == ps, stale = PK[k].age > 90;
      String t = String("#") + PK[k].a + (np <= 6 ? " " : "") + (std::isnan(PK[k].soc) ? String("--") : String(int(PK[k].soc + .5f)) + "%");
      button(b, t, sel, 0, np > 6 ? &fonts::FreeSans9pt7b : nullptr, !stale);
      if (!sel) tft.fillRoundRect(b.x + 6, b.y + b.h - 5, std::max(2, int((b.w - 12) * std::max(0.f, std::min(100.f, std::isnan(PK[k].soc) ? 0.f : PK[k].soc)) / 100)), 3, 1, socColor(PK[k].soc));
    }
  }
  if (!n) { label(16, np ? 120 : 90, "No cell data - the battery is offline or has no BMS.", C_TEXT); return; }
  float mn = 9, mx = 0;
  int imn = 0, imx = 0;
  for (int i = 0; i < n; i++) {
    if (cells[i] < mn) mn = cells[i], imn = i;
    if (cells[i] > mx) mx = cells[i], imx = i;
  }
  bool dense = n > 24 || (np && n > 16);
  int cols = n <= 16 && !np ? 2 : 4, rows = (n + cols - 1) / cols;
  int gap = dense ? 3 : 4, gridH = np ? (dense ? 166 : 140) : dense ? 204 : 178, gy = np ? 86 : 46;
  int cw = 462 / cols - 6, ch = std::min(36, gridH / rows - gap), barH = ch < 28 ? 3 : 5;
  float lo = mx < 3.7f ? 2.5f : 2.8f, hi = mx < 3.7f ? 3.65f : 4.25f;
  LGFX_Sprite c(&tft);
  c.setColorDepth(16);
  if (!c.createSprite(cw, ch)) return;
  for (int fr = force ? 1 : 6; fr <= 6; fr++) // bars grow one after another when the page opens
  for (int i = 0; i < n; i++) {
    float v = cells[i];
    float grow = std::min(1.f, std::max(0.f, fr / 6.f * 1.5f - i * .5f / n));
    c.fillSprite(C_BG);
    c.fillRoundRect(0, 0, cw, ch, 6, C_CARD);
    float f = std::max(0.f, std::min(1.f, (v - lo) / (hi - lo))) * grow;
    c.fillRoundRect(0, ch - barH, int(cw * f), barH, 1, i == imx ? C_BAD : i == imn ? C_BLUE : C_GOOD);
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(textdatum_t::middle_left);
    c.setTextColor(C_MUTED, C_CARD);
    c.drawString(String(i + 1), 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.setTextDatum(textdatum_t::middle_right);
    c.setTextColor(i == imx ? C_BAD : i == imn ? C_BLUE : C_TEXT, C_CARD);
    c.drawString(fmt(v, 3), cw - 8, ch / 2 - (barH > 3 ? 1 : 0));
    c.pushSprite(12 + (i % cols) * (cw + 6), gy + (i / cols) * (ch + gap));
  }
  c.deleteSprite();
  int sh = dense ? 24 : 42, sy = dense ? 252 : 230;
  tft.fillRoundRect(9, sy, 462, sh, dense ? 8 : 10, C_CARD);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(C_TEXT, C_CARD);
  if (np) {
    const auto &k = PK[ps];
    tft.drawString(String("#") + k.a + (k.a == 0 && m.f.busm == 1 ? " MASTER " : "  ") + fmt(k.v, 2) + "V " + fmt(k.i, 1) +
                       "A  " + fmt(k.t, 0) + "C  " + (k.chg ? "C" : "-") + (k.dsg ? "D" : "-") + "  d" +
                       int((mx - mn) * 1000 + .5f) + "mV",
                   21, sy + sh / 2);
    return;
  }
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
static const int PROWS = 4;
static int pPage = 0;
static float pend[bset::MAX_N];
static bool pendSet[bset::MAX_N];
// one parameter view for JK (bms_params.h) and ANT / JBD (bms_settings.h)
struct PInfo { const char *name, *unit; float step, min, max; uint8_t dec; };
static int pCount(const Focus &f) {
  if (f.fam == 1) return bmsw::JK_PARAM_COUNT;
  int n;
  bset::table(f.fam, n);
  return n;
}
static int pRows = PROWS, pY0 = 88; // a bank adds a row of pack buttons above
static bool bankUi = false;
static int pPages(const Focus &f) { return std::max(1, (pCount(f) + pRows - 1) / pRows); }
static PInfo pInfo(const Focus &f, int i) {
  if (f.fam == 1) { const auto &p = bmsw::JK_PARAMS[i]; return {p.name, p.unit, p.step, p.min, p.max, p.decimals}; }
  int n;
  const bset::Def &p = bset::table(f.fam, n)[i];
  return {p.name, p.unit, p.step, p.min, p.max, p.dec};
}
static bool pReady(const Focus &f) { return f.fam == 1 ? f.jkValid : (f.fam == 2 || f.fam == 3) && f.pvHave; }
static bool pHas(const Focus &f, int i) { return f.fam == 1 || (f.pvHave >> i & 1); }
static float pVal(const Focus &f, int i) { return f.fam == 1 ? f.jk[i] : f.pv[i]; }
static Btn pMinus(int r) { return {282, pY0 + r * 38, 52, 34}; }
static Btn pPlus(int r) { return {338, pY0 + r * 38, 52, 34}; }
static Btn pSet(int r) { return {394, pY0 + r * 38, 78, 34}; }
static const Btn PP_PREV = {8, 240, 56, 34}, PP_NEXT = {128, 240, 56, 34};
// MOS
static Btn mosBtn(int i) { return bankUi ? Btn{8 + i * 156, 122, 150, 62} : Btn{8 + i * 156, 92, 150, 78}; }
static Btn mRead() { return bankUi ? Btn{8, 192, 130, 40} : Btn{8, 180, 150, 44}; }
// RS485 bank: one button per pack (shared selection with CELLS)
static Btn pkBtn(int k, int np) { int w = 464 / np; return {8 + k * w, 84, w - 4, 30}; }
static const Btn D_MATCH = {8, 88, 160, 38}, D_CLEAR = {172, 88, 100, 38};
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
  if (c == "bt_match") return "Auto match";
  if (c == "bt_clear") return "Clear matches";
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
static String fmtParam(const Focus &f, int i, float v) { PInfo p = pInfo(f, i); return fmt(v, p.dec) + " " + p.unit; }
// the selected pack of a bank seen as one battery (settings, switches)
static int bankIdx(const Focus &f) {
  int np = std::min<int>(f.packs.size(), 8);
  for (int k = 0; k < np; k++) if (f.packs[k].a == packSel) return k;
  if (np) packSel = f.packs[0].a;
  return 0;
}
static Focus packView(const Focus &f, int k) {
  Focus v = f;
  const PackSum &p = f.packs[k];
  v.fam = 1;
  v.jkValid = p.jkValid;
  for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) v.jk[i] = p.jk[i];
  for (int i = 0; i < 3; i++) v.sw[i] = p.sw[i];
  if (!p.jkValid) { v.sw[0] = p.chg; v.sw[1] = p.dsg; }
  return v;
}
static bool packWritable(const Focus &f, int k) { return f.busm != 1 || f.packs[k].bt; }
static void packRow(const Focus &f, int ps) {
  int np = std::min<int>(f.packs.size(), 8);
  for (int k = 0; k < np; k++) {
    Btn b = pkBtn(k, np);
    const PackSum &p = f.packs[k];
    String t = String("#") + p.a + (np <= 6 ? String(" ") + (std::isnan(p.soc) ? String("--") : String(int(p.soc + .5f)) + "%") : String(""));
    button(b, t, k == ps, 0, &fonts::FreeSansBold9pt7b);
    if (p.bt) tft.fillCircle(b.x + b.w - 9, b.y + 8, 3, C_BLUE);
  }
}
static void ctlPage(const Model &m, bool force) {
  const DevSum *dp = focusDev(m);
  const Focus &f = m.f;
  String key = String(dp ? dp->id : "") + ctlSub + pPage + devPage + B(f.remote) + int(f.fam) + B(f.jkValid) + B(f.sw[0]) + B(f.sw[1]) + B(f.sw[2]) +
               B(f.ledOn) + int(f.ledMode) + int(f.ledColor) + int(f.ledLevel) + B(f.ap) + B(f.tx) + B(f.scanning) + int(f.devs.size()) + f.mac +
               B(dp && dp->live()) + B(dp && dp->online());
  if (pReady(f))
    for (int i = 0; i < pCount(f); i++) key += (pHas(f, i) ? fmt(pVal(f, i), 3) : String("?")) + (pendSet[i] ? fmt(pend[i], 3) : String("-"));
  for (auto &c : f.cmds) key += String(c.id) + c.s;
  bankUi = !f.packs.empty() && ctlSub < 2;
  int ps = bankUi ? bankIdx(f) : 0;
  pRows = bankUi ? 3 : PROWS;
  pY0 = bankUi ? 120 : 88;
  if (!f.packs.empty()) {
    key += String("b") + packSel + f.btm + f.lw + int(f.busm);
    for (auto &p : f.packs) { key += String(p.a) + B(p.bt) + B(p.sw[0]) + B(p.sw[1]) + B(p.sw[2]) + B(p.jkValid); if (p.jkValid) for (float v : p.jk) key += fmt(v, 3); }
  }
  if (!force && key == ctlKey) return;
  ctlKey = key;
  clearContent();
  if (!dp) { noFocus(m); return; }
  Focus pv;
  bool canW = true;
  if (bankUi) { pv = packView(f, ps); canW = packWritable(f, ps); }
  const Focus &f2 = bankUi ? pv : f;
  for (int i = 0; i < 4; i++) button(subBtn(i), CTL_SUB[i], ctlSub == i);
  if (!f.remote) {
    label(16, 110, "This board needs firmware 2.1.0 or newer for control.", C_TEXT);
    label(16, 136, String("Board firmware: ") + (dp->fw.length() ? dp->fw : String("?")));
    return;
  }
  if (!dp->online()) label(16, 262, "Board offline - commands wait 5 min", C_WARN);
  bool live = dp->live();
  if (bankUi) { packRow(f, ps); live = live && f.packs[ps].age < 120; }
  if (ctlSub == 0) {
    const char *nm[3] = {"CHARGE", "DISCHARGE", "BALANCE"};
    for (int i = 0; i < 3; i++) {
      Btn b = mosBtn(i);
      bool avail = live && f2.fam && !(f2.fam == 3 && i == 2) && canW;
      bool on = f2.sw[i];
      bool known = f2.jkValid || i < 2;
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
    button(mRead(), bankUi ? String("READ #") + f.packs[ps].a : String("READ BMS"), false, 0, nullptr, live && canW);
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(C_MUTED, C_BG);
    if (bankUi && !canW) {
      tft.fillRoundRect(8, 192, 464, 44, 10, C_CARD);
      tft.drawRoundRect(8, 192, 464, 44, 10, C_WARN);
      tft.setTextColor(C_WARN, C_CARD);
      tft.drawString(String("#") + f.packs[ps].a + " has no Bluetooth match yet: view only.", 20, 206);
      tft.drawString("CONTROL > BOARD > AUTO MATCH", 20, 226);
    } else if (bankUi) {
      const PackSum &p = f.packs[ps];
      tft.setTextColor(f.busm == 1 ? C_BLUE : C_MUTED, C_BG);
      tft.drawString(String("#") + p.a + (p.a == 0 && f.busm == 1 ? " MASTER" : "") + (f.busm == 1 ? "  BT: " + (p.btName.length() ? p.btName : String("matched")) : String("  via RS485")), 148, 204);
      tft.setTextColor(C_MUTED, C_BG);
      tft.drawString(f.busm == 1 ? "Settings go by Bluetooth, read back" : "Tap a switch to change it", 148, 224);
    } else {
      const char *fam[5] = {"BMS not identified", "JK BMS", "ANT BMS", "JBD BMS", "DALY BMS"};
      tft.drawString(String(fam[f2.fam < 5 ? f2.fam : 0]) + (f.mac.length() ? "  " + f.mac : String("")), 168, 194);
      tft.drawString(live ? "Tap a switch to change it" : "Battery is not live", 168, 214);
    }
  } else if (ctlSub == 1) {
    if (!pReady(f2)) {
      label(16, bankUi ? 140 : 110, bankUi ? String("#") + f.packs[ps].a + ": settings not received yet" : String(f2.fam ? "Reading the BMS settings... (MOSFET > READ BMS)" : "Connect a BMS on the board first."), C_TEXT);
      if (!bankUi) label(16, 136, f2.fam == 2 ? "ANT: all settings are read; writing is experimental" : "Board firmware 2.5.0+ reads ANT / JBD settings.");
    } else {
      if (pPage >= pPages(f2)) pPage = 0;
      for (int r = 0; r < pRows; r++) {
        int i = pPage * pRows + r;
        if (i >= pCount(f2)) break;
        PInfo p = pInfo(f2, i);
        int y = pY0 + r * 38;
        tft.fillRoundRect(8, y, 270, 34, 8, C_CARD);
        tft.setFont(&fonts::FreeSans9pt7b);
        tft.setTextDatum(textdatum_t::middle_left);
        tft.setTextColor(C_MUTED, C_CARD);
        String nm = p.name;
        while (nm.length() && tft.textWidth(nm) > 140) nm.remove(nm.length() - 1);
        tft.drawString(nm, 18, y + 17);
        bool have = pHas(f2, i);
        bool edited = have && pendSet[i] && std::fabs(pend[i] - pVal(f2, i)) > p.step / 2;
        tft.setFont(&fonts::FreeSansBold9pt7b);
        tft.setTextDatum(textdatum_t::middle_right);
        tft.setTextColor(edited ? C_WARN : have ? C_TEXT : C_MUTED, C_CARD);
        tft.drawString(have ? fmtParam(f2, i, edited ? pend[i] : pVal(f2, i)) : String("not read"), 270, y + 17);
        button(pMinus(r), "-", false, 0, &fonts::FreeSansBold12pt7b);
        button(pPlus(r), "+", false, 0, &fonts::FreeSansBold12pt7b);
        button(pSet(r), "SET", edited, C_WARN);
      }
      button(PP_PREV, "<", false, 0, &fonts::FreeSansBold12pt7b);
      button(PP_NEXT, ">", false, 0, &fonts::FreeSansBold12pt7b);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextDatum(textdatum_t::middle_center);
      tft.setTextColor(C_MUTED, C_BG);
      tft.drawString(String(pPage + 1) + "/" + pPages(f2), 96, 258);
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
  } else if (!f.packs.empty()) { // RS485 bank: Bluetooth link of each pack
    button(D_MATCH, f.btm.startsWith("check") || f.btm == "scan" || f.btm == "queued" ? "MATCHING..." : "AUTO MATCH", true);
    button(D_CLEAR, "CLEAR", false);
    button(D_RST, "RESTART", false);
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextDatum(textdatum_t::middle_left);
    tft.setTextColor(C_TEXT, C_BG);
    tft.drawString("Bluetooth of each pack (for settings)", 12, 142);
    int np = std::min<int>(f.packs.size(), 4);
    for (int k = 0; k < np; k++) {
      const PackSum &p = f.packs[k];
      int y = 156 + k * 27;
      tft.fillRoundRect(8, y, 464, 25, 7, C_CARD);
      tft.setFont(&fonts::FreeSansBold9pt7b);
      tft.setTextColor(C_TEXT, C_CARD);
      tft.drawString(String("#") + p.a, 18, y + 13);
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextColor(p.bt ? C_MUTED : C_WARN, C_CARD);
      tft.drawString(p.bt ? (p.btName.length() ? p.btName : String("JK")) : String("not found yet"), 60, y + 13);
      tft.setTextDatum(textdatum_t::middle_right);
      tft.setTextColor(p.bt ? C_GOOD : C_MUTED, C_CARD);
      tft.drawString(p.bt ? "matched" : "-", 462, y + 13);
      tft.setTextDatum(textdatum_t::middle_left);
    }
    if ((int)f.packs.size() > np) label(16, 268, String("+") + (f.packs.size() - np) + " more on the website");
    if (f.btm.length()) { // "scan", "check:2/5", "done:4/5:6"
      String t = f.btm;
      if (t.startsWith("done:")) { t = t.substring(5); int c = t.indexOf(':'); t = "matched " + (c > 0 ? t.substring(0, c) : t); }
      else if (t.startsWith("check:")) t = "checking " + t.substring(6);
      else if (t == "scan" || t == "queued") t = "scanning...";
      tft.setFont(&fonts::FreeSans9pt7b);
      tft.setTextDatum(textdatum_t::middle_right);
      tft.setTextColor(C_MUTED, C_BG);
      tft.drawString(t, 468, 142);
      tft.setTextDatum(textdatum_t::middle_left);
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
  bankUi = !f.packs.empty() && ctlSub < 2;
  int ps = bankUi ? bankIdx(f) : 0;
  pRows = bankUi ? 3 : PROWS;
  pY0 = bankUi ? 120 : 88;
  Focus pv;
  bool canW = true;
  String pa = "";
  if (bankUi) {
    int np = std::min<int>(f.packs.size(), 8);
    for (int k = 0; k < np; k++)
      if (hit(pkBtn(k, np), x, y)) { packSel = f.packs[k].a; for (auto &q : pendSet) q = false; ctlKey = ""; cellsKey = ""; return; }
    pv = packView(f, ps);
    canW = packWritable(f, ps);
    pa = String(",\"a\":") + f.packs[ps].a;
    if (!canW && y > 118 && y < 238) { showToast(String("#") + f.packs[ps].a + " view only: BOARD > AUTO MATCH first", 3500); return; }
  }
  const Focus &f0 = f;
  const Focus &fz = bankUi ? pv : f0;
  if (ctlSub == 0) {
    for (int i = 0; i < 3; i++)
      if (hit(mosBtn(i), x, y) && live && fz.fam && !(fz.fam == 3 && i == 2)) {
        bool on = !(fz.sw[i] && (fz.jkValid || i < 2));
        const char *what[3] = {"Charge MOSFET", "Discharge MOSFET", "Balancer"};
        ask(String(what[i]) + (on ? "  ->  ON" : "  ->  OFF"),
            i == 1 && !on ? String("Discharge OFF cuts power to the load!") : dp->name,
            "bms_switch", String("{\"which\":") + i + ",\"on\":" + (on ? "true" : "false") + pa + "}",
            String(what[i]) + (on ? " ON" : " OFF"), i == 1 && !on);
        return;
      }
    if (hit(mRead(), x, y) && live) { sendCmd("bms_read", "{}", "Read BMS"); showToast("Reading settings..."); }
  } else if (ctlSub == 1 && pReady(fz)) {
    int pages = pPages(fz);
    if (hit(PP_PREV, x, y)) { pPage = (pPage + pages - 1) % pages; return; }
    if (hit(PP_NEXT, x, y)) { pPage = (pPage + 1) % pages; return; }
    for (int r = 0; r < pRows; r++) {
      int i = pPage * pRows + r;
      if (i >= pCount(fz)) break;
      if (!pHas(fz, i)) continue;
      PInfo p = pInfo(fz, i);
      float cur = pendSet[i] ? pend[i] : pVal(fz, i);
      if (hit(pMinus(r), x, y) || hit(pPlus(r), x, y)) {
        float v = cur + (hit(pPlus(r), x, y) ? p.step : -p.step);
        v = std::round(v / p.step) * p.step;
        pend[i] = std::max(p.min, std::min(p.max, v));
        pendSet[i] = true;
        return;
      }
      if (hit(pSet(r), x, y) && pendSet[i] && std::fabs(pend[i] - pVal(fz, i)) > p.step / 2) {
        const char *e;
        if (fz.fam == 1) {
          bmsw::JkSettings js;
          js.valid = true;
          for (int k = 0; k < bmsw::JK_PARAM_COUNT; k++) js.v[k] = fz.jk[k];
          e = bmsw::checkJkValue(js, i, pend[i]);
        } else e = bset::check(fz.fam, fz.pv, fz.pvHave, i, pend[i]);
        if (e) { showToast(String("Not allowed: ") + e, 4000); return; }
        char a[64];
        snprintf(a, sizeof a, "{\"index\":%d,\"value\":%.4f%s}", i, pend[i], pa.c_str());
        ask(String(p.name) + ":  " + fmtParam(fz, i, pVal(fz, i)) + "  ->  " + fmtParam(fz, i, pend[i]),
            bankUi ? String("Pack #") + f.packs[ps].a + (f.busm == 1 ? ": sent by Bluetooth, read back" : ": sent over RS485") : String(fz.fam == 2 ? "ANT write is experimental - it is read back" : "Check the value is right for your cells"),
            "bms_param", a, String(p.name));
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
  } else if (ctlSub == 3 && !f.packs.empty()) {
    if (hit(D_MATCH, x, y)) ask("Match Bluetooth to each pack?", "Close the JK app first - takes about 1 minute", "bt_match", "{}", "Auto match");
    else if (hit(D_CLEAR, x, y)) ask("Forget the Bluetooth matches?", "Settings become view-only until matched again", "bt_clear", "{}", "Clear matches");
    else if (hit(D_RST, x, y)) ask("Restart the board?", "Back online in about 20 s", "restart", "{}", "Restart");
    else return;
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
  tft.drawString(String("Hardware ID ") + hwMac(), 12, 254);
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
  int drawn = 0, shineWas = -1000;
  auto logoCols = [&](int x0, int x1, int shine) { // logo columns with a light band at "shine"
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
    uint32_t t = std::min<uint32_t>(millis() - t0, SPLASH_MS);
    uint32_t level = BL_LEVEL[dcfg.bl];
    tft.setBrightness(t < 700 ? level * t / 700 : level);
    int w = bw * t / SPLASH_MS;
    if (w > drawn) { tft.fillRoundRect(bx, by, w, 4, 2, C_ACCENT); drawn = w; }
    int ph = int(t % 1750);
    int shine = ph < 1200 ? -30 + (LOGO_W + 60) * ph / 1200 : -1000;
    if (shine != shineWas) {
      if (shineWas > -1000) logoCols(shineWas - 30, shineWas + 30, shine);
      if (shine > -1000) logoCols(shine - 30, shine + 30, shine);
      shineWas = shine;
    }
    if (t >= SPLASH_MS) break;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  tft.fillScreen(C_BG);
}

// --------------------------------------------------------- screen task ----
static void moveFocus(const Model &m, int dir) {
  if (m.devs.empty()) return;
  std::vector<int> L;
  bool grouped;
  int per;
  allView(m, L, grouped, per); // the group picked on ALL (or every battery)
  if (L.empty()) for (size_t i = 0; i < m.devs.size(); i++) L.push_back(i);
  int pos = 0;
  for (size_t k = 0; k < L.size(); k++)
    if (m.devs[L[k]].id == focusId) pos = k;
  int idx = L[(pos + dir + L.size()) % L.size()];
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
        std::vector<int> idx;
        bool grouped;
        int per;
        allView(m, idx, grouped, per);
        bool pager = page == P_ALL && (int)idx.size() > per;
        if (hit(B_PREV, tp.x, tp.y) || hit(B_NEXT, tp.x, tp.y)) {
          int dir = hit(B_NEXT, tp.x, tp.y) ? 1 : -1;
          if (nav) { moveFocus(m, dir); drawnPage = -1; }
          else if (pager) {
            int pages = (idx.size() + per - 1) / per;
            allPage = (allPage + dir + pages) % pages;
            drawnPage = -1;
          }
        }
      } else {
        if (page == P_ALL && m.paired) {
          std::vector<int> idx;
          bool grouped;
          int per;
          allView(m, idx, grouped, per);
          if (grouped) {
            int n = std::min<int>(groupsOf(m).size(), 6);
            for (int c = 0; c <= n; c++)
              if (hit(gChip(c, n), tp.x, tp.y)) { gSel = c - 1; allPage = 0; allKey = ""; }
          }
          for (int k = 0; k < per; k++) {
            size_t j = allPage * per + k;
            size_t i = j < idx.size() ? idx[j] : m.devs.size();
            if (i < m.devs.size() && hit(cardBtnG(k, grouped), tp.x, tp.y)) {
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
        } else if (page == P_CELLS && !m.f.packs.empty()) {
          int np = std::min<int>(m.f.packs.size(), 8);
          for (int k = 0; k < np; k++)
            if (hit(packBtn(k, np), tp.x, tp.y)) { packSel = m.f.packs[k].a; cellsKey = ""; }
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
      for (int r = 0; r < pRows; r++)
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
    if (page == P_HOME && drawnPage == P_HOME && !asleep && overlay == OV_NONE && !dlg.active) {
      if (evOn) evTick();
      else flowAnim(focusDev(m));
    }
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
        alarmFlash();
        if (dcfg.beep) beep(3);
        force = true;
      }
      alarmWas = alarmText;
      if (force) {
        if (drawnPage >= 0 && drawnPage != page) pageWipe(page > drawnPage ? 1 : -1);
        else clearContent();
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
  xTaskCreatePinnedToCore(improvTask, "improv", 6144, nullptr, 1, nullptr, 1);
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
