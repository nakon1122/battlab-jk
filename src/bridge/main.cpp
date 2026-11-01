// Bambu LAN -> Cloud bridge (ESP32-S3).
// Talks to up to 4 Bambu Lab printers over LAN MQTT and pushes their state to
// Supabase, where the web dashboard, the remote screens and LINE alerts use it.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "bb_config.h"
#include "cloud_ca_bundle.h"
#include "bambu.h"
#include "camera.h"
#include <mbedtls/base64.h>
#include "web_page.h"

static BambuPrinter printers[BB_MAX_PRINTERS];
static JpegCamera cam;
static bool camOk = false;
static WebServer web(80);
static DNSServer dns;

struct Cfg {
  String ssid, pass, wkey, name;
  PrinterCfg p[BB_MAX_PRINTERS];
} cfg;

static void cfgLoad() {
  Preferences p;
  p.begin("bbridge", true);
  cfg.ssid = p.getString("ssid", "");
  cfg.pass = p.getString("pass", "");
  cfg.wkey = p.getString("wkey", BB_DEFAULT_WRITE_KEY);
  for (int i = 0; i < BB_MAX_PRINTERS; i++) {
    String k = "p" + String(i);
    cfg.p[i].name = p.getString((k + "n").c_str(), "");
    cfg.p[i].model = p.getString((k + "m").c_str(), "");
    cfg.p[i].ip = p.getString((k + "i").c_str(), "");
    cfg.p[i].serial = p.getString((k + "s").c_str(), "");
    cfg.p[i].code = p.getString((k + "c").c_str(), "");
  }
  p.end();
  if (!cfg.wkey.length()) cfg.wkey = BB_DEFAULT_WRITE_KEY;
}
static void cfgSave() {
  Preferences p;
  p.begin("bbridge", false);
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putString("wkey", cfg.wkey);
  for (int i = 0; i < BB_MAX_PRINTERS; i++) {
    String k = "p" + String(i);
    p.putString((k + "n").c_str(), cfg.p[i].name);
    p.putString((k + "m").c_str(), cfg.p[i].model);
    p.putString((k + "i").c_str(), cfg.p[i].ip);
    p.putString((k + "s").c_str(), cfg.p[i].serial);
    p.putString((k + "c").c_str(), cfg.p[i].code);
  }
  p.end();
}
static bool used(int i) { return cfg.p[i].ip.length() && cfg.p[i].serial.length(); }

// -------------------------------------------------------------- cloud ----
static String cloudErr = "not sent yet";
static String devId, hostName, apName;  // bridge-ab12 / bambubridge-ab12 / BAMBU-BRIDGE-AB12
static void makeIds() {
  uint64_t mac = ESP.getEfuseMac();  // bytes in reverse order
  char t[8];
  snprintf(t, sizeof t, "%02x%02x", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  devId = String("bridge-") + t;
  hostName = String("bambubridge-") + t;
  String u = t;
  u.toUpperCase();
  apName = "BAMBU-BRIDGE-" + u;
}
static uint32_t lastPushMs = 0, lastPushOkMs = 0;
static uint32_t pushes = 0, pushFails = 0;

static int rpc(const char *fn, const String &body, String &resp) {
  WiFiClientSecure tls;
  tls.setCACertBundle(CLOUD_CA_BUNDLE);
  tls.setTimeout(12);
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(tls, String(BB_CLOUD_URL) + "/rest/v1/rpc/" + fn)) return -1000;
  http.addHeader("apikey", BB_CLOUD_ANON);
  http.addHeader("Authorization", String("Bearer ") + BB_CLOUD_ANON);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  resp = code > 0 ? http.getString() : String();
  http.end();
  return code;
}

static void pushCloud() {
  DynamicJsonDocument doc(24576);
  doc["p_key"] = cfg.wkey;
  doc["p_device"] = devId;
  doc["p_info"]["ip"] = WiFi.localIP().toString();
  doc["p_info"]["fw"] = BB_FW_VERSION;
  JsonArray a = doc.createNestedArray("p_printers");
  for (int i = 0; i < BB_MAX_PRINTERS; i++)
    if (used(i)) printers[i].toJson(a.createNestedObject());
  String body;
  serializeJson(doc, body);
  String resp;
  lastPushMs = millis();
  int code = rpc("bambu_push2", body, resp);
  pushes++;
  if (code == 200) {
    lastPushOkMs = millis();
    cloudErr = "";
    for (auto &p : printers) p.dirty = p.urgent = false;
  } else {
    pushFails++;
    if (resp.indexOf("invalid key") >= 0) cloudErr = "write key ไม่ถูกต้อง";
    else if (code < 0) cloudErr = "ต่ออินเทอร์เน็ตไม่ได้ (" + String(code) + ")";
    else cloudErr = "cloud error " + String(code) + ": " + resp.substring(0, 80);
  }
}

// ---------------------------------------------------- remote control ----
static bool fastMode = false;            // someone has the dashboard open
static uint32_t lastPollMs = 0, lastFrameUpMs = 0;
static String pollErr;
static String watchSerial;
static uint32_t cmdsDone = 0, framesUp = 0;
static DynamicJsonDocument *pendingAcks = nullptr;

static int findPrinter(const String &serial) {
  for (int i = 0; i < BB_MAX_PRINTERS; i++)
    if (used(i) && cfg.p[i].serial == serial) return i;
  return -1;
}

static void addAck(long id, const String &status, const String &result) {
  if (!pendingAcks) pendingAcks = new DynamicJsonDocument(4096);
  if (!pendingAcks->is<JsonArray>()) pendingAcks->to<JsonArray>();
  if (pendingAcks->size() >= 20) return;
  JsonObject a = pendingAcks->as<JsonArray>().createNestedObject();
  a["id"] = id;
  a["status"] = status;
  a["result"]["msg"] = result;
  cmdsDone++;
}

static void runCommand(long id, const String &serial, JsonObject cmd) {
  int i = findPrinter(serial);
  if (i < 0) return addAck(id, "error", "printer not on this board");
  String type = cmd["type"] | "mqtt";
  if (type == "mqtt") {
    JsonObject pl = cmd["payload"];
    if (pl.isNull()) return addAck(id, "error", "no payload");
    printers[i].publish(id, pl);  // answer arrives through popAck()
  } else if (type == "pushall") {
    printers[i].requestPushall();
    addAck(id, "ok", "refresh requested");
  } else {
    addAck(id, "error", "unknown command type");
  }
}

static void pollCloud() {
  for (int i = 0; i < BB_MAX_PRINTERS; i++) {
    CmdAck a;
    while (used(i) && printers[i].popAck(a)) addAck(a.id, a.status, a.result);
  }
  DynamicJsonDocument body(6144);
  body["p_key"] = cfg.wkey;
  body["p_device"] = devId;
  if (pendingAcks && pendingAcks->size()) body["p_acks"] = pendingAcks->as<JsonArray>();
  else body.createNestedArray("p_acks");
  String b, resp;
  serializeJson(body, b);
  lastPollMs = millis();
  int code = rpc("bambu_poll", b, resp);
  if (code != 200) {
    pollErr = "poll " + String(code);
    return;
  }
  pollErr = "";
  if (pendingAcks) pendingAcks->clear();
  DynamicJsonDocument d(16384);
  if (deserializeJson(d, resp)) return;
  fastMode = d["fast"] | false;
  for (JsonObject c : d["cmds"].as<JsonArray>()) runCommand(c["id"] | 0L, String(c["serial"] | ""), c["cmd"].as<JsonObject>());
  // camera: first watched printer of this board that has a JPEG camera
  String w;
  for (JsonVariant v : d["watch"].as<JsonArray>()) {
    int i = findPrinter(v.as<String>());
    if (i >= 0 && printers[i].hasJpegCamera()) {
      w = v.as<String>();
      break;
    }
  }
  if (w != watchSerial) {
    watchSerial = w;
    if (!w.length()) cam.stop();
    else {
      int i = findPrinter(w);
      cam.watch(w, cfg.p[i].ip, cfg.p[i].code);
    }
  }
}

static void uploadFrame() {
  size_t olen = 0;
  size_t need = ((cam.len() + 2) / 3) * 4 + 1;
  static char *body = nullptr;
  static size_t cap = 0;
  String pre = "{\"p_key\":\"" + cfg.wkey + "\",\"p_serial\":\"" + cam.serial() + "\",\"p_b64\":\"";
  size_t total = pre.length() + need + 4;
  if (total > cap) {
    if (body) free(body);
    body = (char *)ps_malloc(total);
    cap = body ? total : 0;
    if (!body) return;
  }
  memcpy(body, pre.c_str(), pre.length());
  if (mbedtls_base64_encode((unsigned char *)body + pre.length(), need, &olen, cam.frame(), cam.len())) return;
  size_t n = pre.length() + olen;
  body[n++] = '"';
  body[n++] = '}';
  WiFiClientSecure tls;
  tls.setCACertBundle(CLOUD_CA_BUNDLE);
  tls.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(tls, String(BB_CLOUD_URL) + "/rest/v1/rpc/bambu_frame_put")) return;
  http.addHeader("apikey", BB_CLOUD_ANON);
  http.addHeader("Authorization", String("Bearer ") + BB_CLOUD_ANON);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)body, n);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  lastFrameUpMs = millis();
  if (code == 200) {
    framesUp++;
    if (resp.indexOf("\"watching\": false") >= 0 || resp.indexOf("\"watching\":false") >= 0) {
      watchSerial = "";
      cam.stop();
    }
  } else cam.lastErr = "upload " + String(code);
}

static void remoteLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t every = (fastMode || watchSerial.length()) ? 2000 : 10000;
  if (millis() - lastPollMs > every) pollCloud();
  if (camOk && watchSerial.length()) {
    bool fresh = cam.loop();
    if (cam.len() && (fresh || millis() - lastFrameUpMs > 5000) && millis() - lastFrameUpMs > 1500) uploadFrame();
  }
}

static void cloudLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  bool urgent = false, dirty = false;
  for (int i = 0; i < BB_MAX_PRINTERS; i++)
    if (used(i)) {
      urgent |= printers[i].urgent;
      dirty |= printers[i].dirty;
    }
  uint32_t since = millis() - lastPushMs;
  uint32_t retry = cloudErr.length() && lastPushOkMs ? 10000 : 0;
  uint32_t dirtyEvery = fastMode ? 4000 : 15000;
  if (!lastPushMs || (urgent && since > 2000 + retry) || (dirty && since > dirtyEvery) || since > 30000) pushCloud();
}

// ---------------------------------------------------------------- Wi-Fi ----
static bool apOn = false;
static void startAp() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
}

static void startPrinters() {
  for (int i = 0; i < BB_MAX_PRINTERS; i++) {
    if (used(i)) printers[i].begin(cfg.p[i]);
    else printers[i].stop();
  }
}

// ------------------------------------------------------------------ web ----
static void sendJson(JsonDocument &d, int code = 200) {
  String s;
  serializeJson(d, s);
  web.send(code, "application/json", s);
}

static void apiStatus() {
  DynamicJsonDocument d(12288);
  d["fw"] = BB_FW_VERSION;
  d["device"] = devId;
  d["host"] = hostName + ".local";
  d["ap_name"] = apName;
  d["wifi"] = WiFi.status() == WL_CONNECTED;
  d["ssid"] = cfg.ssid;
  d["ip"] = WiFi.localIP().toString();
  d["rssi"] = WiFi.RSSI();
  d["ap"] = apOn;
  d["heap"] = ESP.getFreeHeap();
  d["uptime"] = millis() / 1000;
  d["cloud_ok"] = cloudErr.length() == 0 && lastPushOkMs;
  d["cloud_err"] = cloudErr;
  d["cloud_age"] = lastPushOkMs ? (long)((millis() - lastPushOkMs) / 1000) : -1;
  d["pushes"] = pushes;
  d["psram"] = psramFound() ? (long)ESP.getPsramSize() : 0;
  d["camera"] = camOk ? (watchSerial.length() ? "watching " + watchSerial + " (" + String(cam.frames) + " frames)" : String("ready")) : cam.lastErr;
  d["cam_err"] = cam.lastErr;
  d["remote"] = pollErr.length() ? pollErr : String(fastMode ? "dashboard open" : "idle");
  d["cmds_done"] = cmdsDone;
  d["frames_up"] = framesUp;
  d["push_fails"] = pushFails;
  d["wkey_tail"] = cfg.wkey.length() > 4 ? cfg.wkey.substring(cfg.wkey.length() - 4) : "";
  d["view_key"] = BB_DEFAULT_VIEW_KEY;
  d["dashboard"] = BB_DASHBOARD_URL;
  JsonArray a = d.createNestedArray("printers");
  for (int i = 0; i < BB_MAX_PRINTERS; i++) {
    JsonObject o = a.createNestedObject();
    o["slot"] = i;
    o["name"] = cfg.p[i].name;
    o["model"] = cfg.p[i].model;
    o["ip"] = cfg.p[i].ip;
    o["serial"] = cfg.p[i].serial;
    o["has_code"] = cfg.p[i].code.length() > 0;
    o["used"] = used(i);
    if (!used(i)) continue;
    const BambuPrinter &p = printers[i];
    o["connected"] = p.connected;
    o["err"] = p.lastErr;
    o["reports"] = p.reports;
    o["rx_age"] = p.lastRxMs ? (long)((millis() - p.lastRxMs) / 1000) : -1;
    if (p.st.have) {
      o["state"] = p.st.gcode;
      o["pct"] = p.st.pct;
      o["remain"] = p.st.remain;
      o["task"] = p.st.task;
      o["noz"] = p.st.noz;
      o["bed"] = p.st.bed;
    }
  }
  sendJson(d);
}

static void apiSave() {
  DynamicJsonDocument d(4096);
  if (deserializeJson(d, web.arg("plain"))) {
    web.send(400, "text/plain", "bad json");
    return;
  }
  bool wifiChanged = false;
  if (d.containsKey("ssid")) {
    String s = d["ssid"].as<String>();
    s.trim();
    if (s != cfg.ssid) wifiChanged = true;
    cfg.ssid = s;
  }
  if (d["pass"].as<String>().length()) {
    cfg.pass = d["pass"].as<String>();
    wifiChanged = true;
  }
  if (d["wkey"].as<String>().length()) {
    cfg.wkey = d["wkey"].as<String>();
    cfg.wkey.trim();
  }
  if (d.containsKey("printers")) {
    JsonArray a = d["printers"];
    for (int i = 0; i < BB_MAX_PRINTERS && i < (int)a.size(); i++) {
      JsonObject o = a[i];
      PrinterCfg &c = cfg.p[i];
      c.name = o["name"] | "";
      c.model = o["model"] | "";
      c.ip = o["ip"] | "";
      c.serial = o["serial"] | "";
      c.name.trim();
      c.ip.trim();
      c.serial.trim();
      c.serial.toUpperCase();
      String code = o["code"] | "";
      code.trim();
      if (code.length()) c.code = code;
      if (o["clear"] | false) c = PrinterCfg();
    }
  }
  cfgSave();
  web.send(200, "application/json", "{\"ok\":true}");
  if (wifiChanged) {
    delay(500);
    ESP.restart();
  }
  startPrinters();
  lastPushMs = 0;  // push the new printer list right away
}

static void apiLine() {
  DynamicJsonDocument in(2048);
  deserializeJson(in, web.arg("plain"));
  String action = in["action"] | "status";
  String resp;
  int code;
  DynamicJsonDocument body(2048);
  body["p_key"] = cfg.wkey;
  if (action == "save") {
    body["p_token"] = in["token"] | "";
    body["p_to"] = in["to"] | "";
    JsonArray k = body.createNestedArray("p_kinds");
    for (JsonVariant v : in["kinds"].as<JsonArray>()) k.add(v.as<const char *>());
    String b;
    serializeJson(body, b);
    code = rpc("bambu_set_line", b, resp);
  } else {
    String b;
    serializeJson(body, b);
    code = rpc(action == "test" ? "bambu_test_line" : "bambu_line_status", b, resp);
  }
  if (code == 200) web.send(200, "application/json", resp);
  else web.send(502, "application/json", String("{\"error\":") + (resp.length() ? resp : String("\"cloud ") + code + "\"") + "}");
}

// all bridge boards + printers on this account (to see which board has which printer)
static void apiSite() {
  String resp;
  int code = rpc("bambu_get", String("{\"p_key\":\"") + BB_DEFAULT_VIEW_KEY + "\",\"p_events\":0}", resp);
  if (code != 200) {
    web.send(502, "application/json", "{\"error\":\"cloud " + String(code) + "\"}");
    return;
  }
  // keep it small: devices + printer name/device/online
  DynamicJsonDocument in(32768), out(8192);
  if (deserializeJson(in, resp)) {
    web.send(502, "application/json", "{\"error\":\"bad reply\"}");
    return;
  }
  out["devices"] = in["devices"];
  JsonArray pa = out.createNestedArray("printers");
  for (JsonObject p : in["printers"].as<JsonArray>()) {
    JsonObject o = pa.createNestedObject();
    o["name"] = p["name"];
    o["model"] = p["model"];
    o["device"] = p["device"];
    o["online"] = p["online"];
    o["serial"] = p["serial"];
  }
  out["me"] = devId;
  sendJson(out);
}

static void apiForget() {
  DynamicJsonDocument in(512);
  deserializeJson(in, web.arg("plain"));
  String dev = in["device"] | "";
  if (!dev.length() || dev == devId) {
    web.send(400, "application/json", "{\"error\":\"bad device\"}");
    return;
  }
  String resp;
  int code = rpc("bambu_forget_device", "{\"p_key\":\"" + cfg.wkey + "\",\"p_device\":\"" + dev + "\"}", resp);
  web.send(code == 200 ? 200 : 502, "application/json", code == 200 ? resp : String("{\"error\":\"cloud\"}"));
}

static void apiCtl() {
  DynamicJsonDocument in(512);
  deserializeJson(in, web.arg("plain"));
  String k = in["key"] | "";
  k.trim();
  String resp;
  int code = rpc("bambu_set_ctl", "{\"p_key\":\"" + cfg.wkey + "\",\"p_new\":\"" + k + "\"}", resp);
  web.send(code == 200 ? 200 : 400, "application/json", code == 200 ? String("{\"ok\":true}") : String("{\"error\":\"รหัสต้องยาว 6 ตัวขึ้นไป\"}"));
}

static void apiScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    web.send(200, "application/json", "{\"running\":true}");
    return;
  }
  if (n < 0) {
    WiFi.scanNetworks(true);
    web.send(200, "application/json", "{\"running\":true}");
    return;
  }
  DynamicJsonDocument d(4096);
  JsonArray a = d.createNestedArray("nets");
  for (int i = 0; i < n && i < 25; i++) {
    JsonObject o = a.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void webSetup() {
  web.on("/", [] { web.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  web.on("/api/status", apiStatus);
  web.on("/api/save", HTTP_POST, apiSave);
  web.on("/api/line", HTTP_POST, apiLine);
  web.on("/api/scan", apiScan);
  web.on("/api/site", apiSite);
  web.on("/api/ctl", HTTP_POST, apiCtl);
  web.on("/api/forget", HTTP_POST, apiForget);
  web.on("/api/pushall", [] {
    for (auto &p : printers) p.requestPushall();
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/restart", [] {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });
  web.onNotFound([] {
    if (apOn) {
      web.sendHeader("Location", "http://192.168.4.1/");
      web.send(302, "text/plain", "");
    } else web.send(404, "text/plain", "not found");
  });
  web.begin();
}

// ------------------------------------------------------------- arduino ----
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nBambu bridge " BB_FW_VERSION);
  makeIds();
  cfgLoad();
  bambuInitFilter();
  camOk = cam.begin();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostName.c_str());
  if (cfg.ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  } else {
    startAp();
  }
  webSetup();
  startPrinters();
}

void loop() {
  static uint32_t bootMs = millis(), lastLog = 0;
  static bool mdnsUp = false, wasUp = false;
  if (apOn) dns.processNextRequest();
  web.handleClient();
  bool up = WiFi.status() == WL_CONNECTED;
  if (!up && !apOn && millis() - bootMs > 60000) startAp();
  if (up && !mdnsUp) mdnsUp = MDNS.begin(hostName.c_str());
  if (up && !wasUp) Serial.printf("Wi-Fi up, http://%s/  http://%s.local/\n", WiFi.localIP().toString().c_str(), hostName.c_str());
  wasUp = up;
  if (up) {
    for (int i = 0; i < BB_MAX_PRINTERS; i++) {
      if (!used(i)) continue;
      printers[i].loop();
      web.handleClient();
    }
    cloudLoop();
    remoteLoop();
  }
  if (millis() - lastLog > 30000) {
    lastLog = millis();
    Serial.printf("heap %u  min %u  cloud %s\n", ESP.getFreeHeap(), ESP.getMinFreeHeap(), cloudErr.c_str());
    for (int i = 0; i < BB_MAX_PRINTERS; i++)
      if (used(i))
        Serial.printf("  %s %s %s %s %d%%\n", cfg.p[i].name.c_str(), printers[i].connected ? "up" : "down",
                      printers[i].lastErr.c_str(), printers[i].st.gcode.c_str(), printers[i].st.pct);
  }
  delay(2);
}
