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
#include "web_page.h"

static BambuPrinter printers[BB_MAX_PRINTERS];
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
  JsonArray a = doc.createNestedArray("p_printers");
  for (int i = 0; i < BB_MAX_PRINTERS; i++)
    if (used(i)) printers[i].toJson(a.createNestedObject());
  String body;
  serializeJson(doc, body);
  String resp;
  lastPushMs = millis();
  int code = rpc("bambu_push", body, resp);
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
  if (!lastPushMs || (urgent && since > 2000 + retry) || (dirty && since > 15000) || since > 30000) pushCloud();
}

// ---------------------------------------------------------------- Wi-Fi ----
static bool apOn = false;
static void startAp() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("BAMBU-BRIDGE");
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
  cfgLoad();
  bambuInitFilter();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("bambubridge");
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
  if (up && !mdnsUp) mdnsUp = MDNS.begin("bambubridge");
  if (up && !wasUp) Serial.printf("Wi-Fi up, http://%s/  http://bambubridge.local/\n", WiFi.localIP().toString().c_str());
  wasUp = up;
  if (up) {
    for (int i = 0; i < BB_MAX_PRINTERS; i++) {
      if (!used(i)) continue;
      printers[i].loop();
      web.handleClient();
    }
    cloudLoop();
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
