#include "jk_protocol.h"
#include "bms_other.h"
#include "bms_params.h"
#include "bms_settings.h"
#include "jk_rs485.h"
#include "improv_serial.h"
#include <algorithm>
#include "bms_daly.h"
#include <mbedtls/sha256.h>
#include <Update.h>
#include "jk_auth.h"
#include "ble_channels.h"
#include "ui.h"
#include "display.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
#include <vector>
#include "cloud_config.h"
#include "cloud_ca_bundle.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#ifdef BATTLAB_CYD35
#define BATTLAB_FW "2.8.7-cyd35"
#else
#define BATTLAB_FW "2.8.7-s3"
#endif
WebServer web(80);
// Why the chip restarted last time (shown on screen / API to diagnose resets).
const char *bleTxText();
static bool radioMax = true;
static const char *resetReasonText = "unknown";
static bool resetAbnormal = false;
static void readResetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: resetReasonText = "power on"; break;
    case ESP_RST_SW: resetReasonText = "software restart"; break;
    case ESP_RST_PANIC: resetReasonText = "CRASH (panic)"; resetAbnormal = true; break;
    case ESP_RST_INT_WDT: resetReasonText = "CRASH (interrupt watchdog)"; resetAbnormal = true; break;
    case ESP_RST_TASK_WDT: resetReasonText = "CRASH (task watchdog)"; resetAbnormal = true; break;
    case ESP_RST_WDT: resetReasonText = "CRASH (watchdog)"; resetAbnormal = true; break;
    case ESP_RST_BROWNOUT: resetReasonText = "BROWNOUT (power too weak)"; resetAbnormal = true; break;
    case ESP_RST_EXT: resetReasonText = "reset button"; break;
    case ESP_RST_DEEPSLEEP: resetReasonText = "wake from sleep"; break;
    default: resetReasonText = "other"; break;
  }
}
Preferences prefs;
SemaphoreHandle_t lock;
struct Config {
  String mac, protocol;
  String bluetoothCode, parameterCode; // Session only; never NVS, log or API response.
  uint8_t type = 0;
  bool automatic = true;
} cfg;
struct Shared {
  jk::Sample sample;
  jk::Parameters params;
  bool linked = false, have = false, haveParams = false, scanning = false;
  uint32_t stamp = 0, paramStamp = 0, good = 0, bad = 0, rejected = 0,
           retries = 0;
  int rssi = 0;
  String model, hardware, software, detected;
  String connectionAuth = "NOT_SET", parameterAuth = "LOCKED";
  uint16_t writeHandle=0, notifyHandle=0;
  int bleError=0;
  uint32_t rxBytes=0, rxChunks=0, txOk=0, txFail=0;
  bool writeWithResponse=false;
  String lastRxHex, gatt, probe = "idle", lastRxChar, probeLog;
  bmsw::JkSettings jk;      // last JK settings frame (0x01)
  uint32_t jkStamp = 0;
  uint8_t family = 0;       // 0 unknown, 1 JK, 2 ANT, 3 JBD, 4 DALY
  String lastWrite;         // result of the last BMS write, for the UI
  bool probeLogFrozen = false;
  uint32_t ftype[4] = {0, 0, 0, 0}; // JK frames by type (0 other, 1 settings, 2 cells, 3 info)
  String f2hex;                     // last JK cell frame, for checking the layout remotely
  uint32_t links = 0, lastLinkMs = 0; // link losses and how long the last link lasted
  String subLog, peerName;            // CCCD subscribe results, GAP device name of the BMS
  String codeBt, codeSet;             // JK passcodes from the device-info frame (owner recovery)
  String state = "UNCONFIGURED", devices = "[]";
  uint32_t devStamp = 0;
  float pv[bset::MAX_N] = {};   // ANT / JBD protection settings (bms_settings.h)
  uint64_t pvHave = 0;          // bit i = pv[i] was read from the BMS    // when the last BLE scan finished
#ifndef BATTLAB_DISPLAY
  // RS485 bus with several packs (index = DIP / Modbus address 0..15)
  struct Pack { jk::Sample s; uint32_t stamp = 0; bmsw::JkSettings js; uint32_t setStamp = 0; String model; char serial[12] = {0}; String codeBt, codeSet; };
  Pack packs[16];
#endif
  uint8_t busMode = 0;          // 0 single pack, 1 listening to a master (DIP 0), 2 board polls the packs
  uint8_t bankN = 0;            // packs seen recently (>1 = bank view)
} shared;
volatile uint16_t bleMtu = 0; // negotiated ATT MTU of the current BMS link
bool scanWanted = false, configChanged = false, readWanted = false;
uint32_t generation = 0;
bool persistWanted = false;
// BMS link: Bluetooth (default) or the RS485 cable (JK, ESP32-S3 only)
struct LinkCfg {
  bool rs485 = false;
  int8_t rx = 18, tx = 17;
  uint32_t baud = 115200;
  uint8_t addr = 1;
  uint8_t bus = 0;   // several packs on the bus: 0 auto, 1 listen only, 2 board polls
  uint8_t count = 1; // poll mode: packs at addr .. addr+count-1
  uint8_t brand = 0; // 0 auto, 1 JK, 2 DALY
} linkCfg;
// Usage profile (set from the website): changes the screen layout and alarms.
struct ProfileCfg { uint8_t type = 0; uint16_t whkm = 0; uint8_t rangeAlarm = 10; } profCfg; // 0 solar,1 ev,2 car,3 ups,4 camp
static const char *const PROF_NAME[5] = {"solar", "ev", "car", "ups", "camp"};
#ifndef BATTLAB_DISPLAY
// Bluetooth link to each pack of an RS485 bank (used to change its settings)
struct BtLink { char mac[18] = {0}; uint8_t type = 0; char name[20] = {0}; };
static BtLink btMap[16];
static volatile bool btMatchWanted = false, btClearWanted = false;
static String btmStatus;          // progress of the auto-match, for the website
struct PackJob { uint8_t a; bool isSwitch; uint8_t idx; float value; bool on; uint32_t id; };
static PackJob pjQ[4];
static uint8_t pjN = 0;
struct PwEntry { uint8_t a = 0; String what; uint8_t st = 0; uint32_t at = 0; uint32_t id = 0; }; // st: 0 bluetooth, 1 sent, 2 ok, 3 no change, 4 no bluetooth
static PwEntry pwLog[6];
static uint32_t pwSeq = 0;
static void pwSet(uint32_t id, uint8_t a, const String &what, uint8_t st);
#endif
static volatile bool linkRs485 = false;
static volatile bool otaBusy = false; // firmware update running: Bluetooth stays idle
static volatile bool bleOtaBusy = false; // firmware arriving over the Bluetooth setup link
static volatile int otaPct = -1;
int otaProgress() { return otaPct; }
static String devName = "BATTLAB"; // BATTLAB-XXXXXX: hotspot + Bluetooth name = last 6 hex digits of the chip MAC
static String hwMac;                // chip MAC (the "MAC:" line shown while flashing)
static volatile uint32_t serialSetupAt = 0; // a computer is setting the board up over USB (website)
void energyGet(float &in, float &out);       // energy today, kWh (defined further down)
void bleSetupStart();               // Bluetooth setup service (defined below)
class Guard {
public:
  Guard() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(lock); }
};
void json(int code, JsonDocument &d) {
  String s;
  serializeJson(d, s);
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "application/json", s);
}
void error(int code, const char *msg) {
  StaticJsonDocument<192> d;
  d["error"] = msg;
  json(code, d);
}
bool localRequest() {
  // Only the hotspot address or this board's home-network address are valid
  // hosts/origins (blocks DNS-rebinding and cross-site requests).
  String h = web.hostHeader();
  String lan = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-");
  if (h != "192.168.4.1" && h != "192.168.4.1:80" && h != lan && h != lan + ":80") {
    error(403, "Invalid Host");
    return false;
  }
  String o = web.header("Origin");
  if (o.length() && o != "http://192.168.4.1" && o != "http://" + lan) {
    error(403, "Invalid Origin");
    return false;
  }
  return true;
}
void stateApi() {
  DynamicJsonDocument d(8192);
  {
    Guard g;
    d["state"] = shared.state;
    d["ble_connected"] = shared.linked;
    d["connection_auth"] = shared.connectionAuth;
    d["parameter_auth"] = shared.parameterAuth;
    d["parameter_write_supported"] = shared.family == 1 && shared.jk.valid;
    d["bms_family"] = shared.family;
    d["last_write"] = shared.lastWrite;
    d["write_handle"] = shared.writeHandle;
    d["notify_handle"] = shared.notifyHandle;
    d["ble_error"] = shared.bleError;
    d["rx_bytes"] = shared.rxBytes;
    d["rx_chunks"] = shared.rxChunks;
    d["last_rx_hex"] = shared.lastRxHex;
    d["tx_ok"] = shared.txOk;
    d["tx_fail"] = shared.txFail;
    d["write_with_response"] = shared.writeWithResponse;
    d["gatt"] = shared.gatt;
    d["probe"] = shared.probe;
    d["last_rx_char"] = shared.lastRxChar;
    bool fresh = shared.linked && shared.have &&
                 uint32_t(millis() - shared.stamp) < 15000;
    d["live"] = fresh;
    d["sample_ms"] = shared.have ? shared.stamp : 0;
    d["age_ms"] = nullptr;
    if (shared.have)
      d["age_ms"] = uint32_t(millis() - shared.stamp);
    d["model"] = shared.model;
    d["hardware"] = shared.hardware;
    d["software"] = shared.software;
    d["mac"] = cfg.mac;
    d["protocol"] = cfg.protocol;
    d["address_type"] = cfg.type;
    d["auto_connect"] = cfg.automatic;
    d["detected_protocol"] = shared.detected;
    d["rssi"] = shared.rssi;
    d["good_frames"] = shared.good;
    d["checksum_errors"] = shared.bad;
    d["rejected_samples"] = shared.rejected;
    d["retries"] = shared.retries;
    d["scanning"] = shared.scanning;
    if (fresh) {
      auto &s = shared.sample;
      auto v = d.createNestedObject("values");
      v["soc"] = s.soc;
      v["voltage"] = s.voltage;
      v["current"] = s.current;
      v["power"] = s.voltage * s.current;
      v["capacity"] = s.capacity;
      v["full"] = s.full;
      v["cycles"] = s.cycles;
      v["temp1"] = s.temp1;
      v["temp2"] = s.temp2;
      v["mos"] = s.mos;
      v["charge"] = s.charge;
      v["discharge"] = s.discharge;
      auto a = v.createNestedArray("cells");
      for (auto c : s.cells) {
        if (c > 0)
          a.add(c);
        else
          a.add(nullptr);
      }
    }
#ifndef BATTLAB_DISPLAY
    if (linkRs485 && (shared.bankN > 1 || shared.busMode == 1)) { // RS485 bank = one group
      auto b = d.createNestedObject("bank");
      b["n"] = shared.bankN;
      b["master"] = shared.busMode == 1;
      auto pa = b.createNestedArray("packs");
      uint32_t now = millis();
      for (int a = 0; a < 16; a++) {
        const auto &pk = shared.packs[a];
        if (!pk.stamp || now - pk.stamp > 600000) continue;
        auto o = pa.createNestedObject();
        o["a"] = a;
        o["age"] = (now - pk.stamp) / 1000;
        o["soc"] = pk.s.soc;
        o["v"] = pk.s.voltage;
        o["i"] = pk.s.current;
        o["t"] = std::max(pk.s.temp1, pk.s.temp2);
        o["cap"] = pk.s.capacity;
        o["full"] = pk.s.full;
        o["chg"] = pk.s.charge;
        o["dsg"] = pk.s.discharge;
        float mn = 9, mx = 0;
        for (float c : pk.s.cells) if (c > 0) { mn = std::min(mn, c); mx = std::max(mx, c); }
        if (mx > 0) { o["cmin"] = mn; o["cmax"] = mx; }
      }
    }
#endif
    d["parameters_valid"] = shared.linked && shared.haveParams;
    if (shared.haveParams && shared.linked) {
      auto p = d.createNestedObject("parameters");
      p["uvp"] = shared.params.uvp;
      p["uvpr"] = shared.params.uvpr;
      p["ovp"] = shared.params.ovp;
      p["ovpr"] = shared.params.ovpr;
      p["balance_delta"] = shared.params.balance;
      p["charge_limit"] = shared.params.charge;
      p["discharge_limit"] = shared.params.discharge;
      d["parameter_age_ms"] = uint32_t(millis() - shared.paramStamp);
    }
  }
  {
    float ei, eo;
    energyGet(ei, eo);
    d["energy_in"] = ei;   // kWh today (charge)
    d["energy_out"] = eo;  // kWh today (discharge)
    d["energy_day_ok"] = time(nullptr) > 1700000000;
  }
  d["heap"] = ESP.getFreeHeap();
  d["heap_min"] = ESP.getMinFreeHeap();
  d["heap_max_block"] = ESP.getMaxAllocHeap();
  d["reset_reason"] = resetReasonText;
  d["ble_tx"] = bleTxText();
  d["radio_max"] = radioMax;
  d["uptime_ms"] = millis();
  d["firmware"] = BATTLAB_FW;
  json(200, d);
}
bool validMac(String m) {
  if (m.length() != 17)
    return false;
  for (int i = 0; i < 17; i++)
    if (i % 3 == 2 ? m[i] != ':' : !isxdigit(m[i]))
      return false;
  return true;
}
String asciiField(const uint8_t *b, size_t n) {
  String s;
  for (size_t i = 0; i < n && b[i]; i++)
    s += b[i] >= 32 && b[i] < 127 ? char(b[i]) : '?';
  return s;
}
// ---- Radio transmit power ------------------------------------------------
// MAX (default): Bluetooth at the chip maximum, Wi-Fi at 19.5 dBm.
// LOW: Bluetooth +3 dBm, Wi-Fi 8.5 dBm, for weak power supplies.
#if CONFIG_IDF_TARGET_ESP32S3
static const esp_power_level_t BLE_MAX_LEVEL = ESP_PWR_LVL_P21; // S3: about +20 dBm
static const char *BLE_MAX_TEXT = "+20 dBm";
#else
static const esp_power_level_t BLE_MAX_LEVEL = ESP_PWR_LVL_P9;  // ESP32: +9 dBm
static const char *BLE_MAX_TEXT = "+9 dBm";
#endif
// radioMax is declared near the top of the file (default: true)
static volatile bool bleTxApplyWanted = false;
bool radioMaxPower() { return radioMax; }
const char *bleTxText() { return radioMax ? BLE_MAX_TEXT : "+3 dBm"; }
static void applyBleTxPower() { // call from the BLE task after init / connect
  esp_power_level_t l = radioMax ? BLE_MAX_LEVEL : ESP_PWR_LVL_P3;
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, l);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, l);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, l);
  for (int i = 0; i < 3; i++)
    esp_ble_tx_power_set(esp_ble_power_type_t(ESP_BLE_PWR_TYPE_CONN_HDL0 + i), l);
}
static void applyWifiTxPower() {
  if (WiFi.getMode() != WIFI_OFF)
    WiFi.setTxPower(radioMax ? WIFI_POWER_19_5dBm : WIFI_POWER_8_5dBm);
}
void radioSetMaxPower(bool on) {
  radioMax = on;
  Preferences p;
  p.begin("battlab-jk", false);
  p.putBool("txmax", on);
  p.end();
  applyWifiTxPower();
  bleTxApplyWanted = true; // BLE task applies it
}

// ---- BMS write commands (queued by web/screen, executed by the BLE task) --
struct BmsCmd {
  bool isSwitch;
  uint8_t idx;   // parameter index or switch index
  float value;   // parameter value
  bool on;       // switch state
};
static BmsCmd cmdQ[4];
static uint8_t cmdAddrQ[4]; // RS485 bus: pack address for the command (0xFF = the configured one)
static uint8_t cmdN = 0;
static const char *queueCmd(const BmsCmd &c, uint8_t addr = 0xFF) {
  Guard g;
  if (!shared.linked) return "BMS not connected";
  if (cmdN >= 4) return "busy, try again";
  cmdAddrQ[cmdN] = addr;
  cmdQ[cmdN++] = c;
  return nullptr;
}
// Returns nullptr when queued, otherwise the reason it was refused.
const char *bmsWriteParam(int idx, float value, int addr) {
  if (addr >= 0) { // one pack of an RS485 bank
#ifdef BATTLAB_DISPLAY
    return "pack address needs the ESP32-S3 RS485 board";
#else
    Guard g;
    if (!linkRs485 || addr > 15) return "pack address 0-15 on RS485 only";
    if (shared.family != 1) return "settings: JK only";
    if (idx < 0 || idx >= bmsw::JK_PARAM_COUNT) return "unknown setting";
    const auto &pk = shared.packs[addr];
    if (pk.js.valid && uint32_t(millis() - pk.setStamp) < 600000)
      if (const char *e = bmsw::checkJkValue(pk.js, idx, value)) return e;
    if (shared.busMode == 1) { // master runs the bus: go over Bluetooth
      if (!btMap[addr].mac[0]) return "pack has no Bluetooth match yet - run auto match";
      if (pjN >= 4) return "busy, try again";
      uint32_t id = ++pwSeq;
      pjQ[pjN++] = {uint8_t(addr), false, uint8_t(idx), value, false, id};
      const auto &p = bmsw::JK_PARAMS[idx];
      pwLog[5] = PwEntry(); // make room
      for (int k = 5; k > 0; k--) pwLog[k] = pwLog[k - 1];
      pwLog[0].a = addr; pwLog[0].what = String(p.name) + " -> " + String(value, int(p.decimals)) + " " + p.unit;
      pwLog[0].st = 0; pwLog[0].at = millis(); pwLog[0].id = id;
      return nullptr;
    }
    if (!pk.js.valid) return "settings of this pack not read yet";
#endif
  } else {
    Guard g;
    if (shared.family == 2 || shared.family == 3) {
      int n;
      bset::table(shared.family, n);
      if (idx < 0 || idx >= n) return "unknown setting";
      if (!(shared.pvHave >> idx & 1)) return "setting not read yet - read the BMS first";
      if (const char *e = bset::check(shared.family, shared.pv, shared.pvHave, idx, value)) return e;
    } else {
      if (shared.family == 4) return "DALY: only the MOS switches can be changed for now";
      if (shared.family != 1) return "BMS type not identified yet";
      if (uint32_t(millis() - shared.jkStamp) > 600000 || !shared.jk.valid)
        return "settings not read yet";
      if (const char *e = bmsw::checkJkValue(shared.jk, idx, value)) return e;
    }
  }
  return queueCmd({false, uint8_t(idx), value, false}, addr >= 0 ? uint8_t(addr) : 0xFF);
}
const char *bmsSetSwitch(int which, bool on, int addr) {
  if (which < 0 || which >= bmsw::SW_COUNT) return "unknown switch";
  if (addr >= 0) {
#ifdef BATTLAB_DISPLAY
    return "pack address needs the ESP32-S3 RS485 board";
#else
    Guard g;
    if (!linkRs485 || addr > 15) return "pack address 0-15 on RS485 only";
    if (!shared.packs[addr].stamp) return "pack not seen on the bus";
    if (shared.family == 4 && which == bmsw::SW_BALANCER) return "balancer switch: not on DALY wire protocol";
    if (shared.busMode == 1) {
      if (!btMap[addr].mac[0]) return "pack has no Bluetooth match yet - run auto match";
      if (pjN >= 4) return "busy, try again";
      uint32_t id = ++pwSeq;
      pjQ[pjN++] = {uint8_t(addr), true, uint8_t(which), 0, on, id};
      for (int k = 5; k > 0; k--) pwLog[k] = pwLog[k - 1];
      pwLog[0].a = addr; pwLog[0].what = String(which == 0 ? "Charge" : which == 1 ? "Discharge" : "Balancer") + (on ? " ON" : " OFF");
      pwLog[0].st = 0; pwLog[0].at = millis(); pwLog[0].id = id;
      return nullptr;
    }
#endif
  } else {
    Guard g;
    if (!shared.family) return "BMS type not identified yet";
    if (shared.family == 3 && which == bmsw::SW_BALANCER) return "not available on JBD";
  }
  return queueCmd({true, uint8_t(which), 0, on}, addr >= 0 ? uint8_t(addr) : 0xFF);
}

#ifndef BATTLAB_DISPLAY
static void btSave() { // "a,mac,type,name;..."
  String o;
  for (int a = 0; a < 16; a++)
    if (btMap[a].mac[0]) o += String(a) + "," + btMap[a].mac + "," + btMap[a].type + "," + btMap[a].name + ";";
  Preferences p;
  p.begin("battlab-jk", false);
  p.putString("btmap", o);
  p.end();
}
static void btLoad() {
  Preferences p;
  p.begin("battlab-jk", true);
  String s = p.getString("btmap", "");
  p.end();
  int i = 0;
  while (i < (int)s.length()) {
    int e = s.indexOf(';', i);
    if (e < 0) break;
    String it = s.substring(i, e);
    i = e + 1;
    int c1 = it.indexOf(','), c2 = it.indexOf(',', c1 + 1), c3 = it.indexOf(',', c2 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0) continue;
    int a = it.substring(0, c1).toInt();
    String mac = it.substring(c1 + 1, c2);
    if (a < 0 || a > 15 || mac.length() != 17) continue;
    strncpy(btMap[a].mac, mac.c_str(), 17);
    btMap[a].type = it.substring(c2 + 1, c3).toInt();
    strncpy(btMap[a].name, it.substring(c3 + 1).c_str(), sizeof(btMap[a].name) - 1);
  }
}
static void pwSet(uint32_t id, uint8_t a, const String &what, uint8_t st) {
  Guard g;
  for (auto &e : pwLog)
    if (e.id == id) { e.st = st; e.at = millis(); if (what.length()) e.what = what; return; }
}
static void serialOf(const uint8_t *b, char *out) { // JK device info frame: serial number at byte 86, 11 chars
  int k = 0;
  for (int i = 0; i < 11; i++) { char c = b[86 + i]; if (c >= 33 && c < 127) out[k++] = c; }
  out[k] = 0;
}
#endif
void bleWorker(void *) {
#ifdef BATTLAB_CYD35
  // Start the Bluetooth radio after Wi-Fi and the boot splash have settled,
  // so both radios never ramp up at the same moment (current spike).
  vTaskDelay(pdMS_TO_TICKS(5500));
#endif
  NimBLEDevice::init(devName.c_str());
  NimBLEDevice::setMTU(247);
  applyBleTxPower();
  bleSetupStart();
  auto scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setMaxResults(24);
  NimBLEClient *client = NimBLEDevice::createClient();
  client->setConnectTimeout(8);
  jk::Parser parser;
  jk::AutoDetector detector;
  uint32_t discoveryAt = 0;
  bool needsSelection = false;
  Config c;
  uint32_t lastTry = 0, lastPoll = 0, lastInfo = 0, seen = 0;
  NimBLERemoteCharacteristic *ch = nullptr;
  bool gotInfo = false, gotCellFrame = false;
  uint32_t linkedAt = 0, lastCellFrame = 0, lastCellRequest = 0;
  // ANT / JBD settings: read one register every 150 ms; verify writes by reading back
  int prStep = -1, vIdx = -1;
  bool prDone = false;
  float vVal = 0;
  uint32_t prAt = 0, vAt = 0;
  uint8_t cellRequests = 0;
  // BMS family: 0 = not identified yet, 1 = JK, 2 = ANT, 3 = JBD/Xiaoxiang
  uint8_t fam = 0;
  bool linkedLocal = false;
  NimBLERemoteCharacteristic *antCh = nullptr, *jbdCh = nullptr;
  bms::AntFramer antF;
  bms::JbdFramer jbdF;
  // DALY: new (0xD2 Modbus-like) and older (0xA5) protocols over FFF0 / FFF2
  NimBLERemoteCharacteristic *dalyCh = nullptr;
  daly::D2Framer dD2;
  daly::A5Framer dA5;
  daly::A5State dA5st;
  uint8_t dalySub = 0, dalyIdx = 0; // 1 = D2, 2 = A5
  jk::Sample jbdSample;
  bool jbdHaveBasic = false;
  uint32_t identAt = 0, identStartedAt = 0, lastOtherPoll = 0;
  bool peerIsAnt = false;
  uint8_t identIdx = 0, jbdToggle = 0;
  bool fullProbeDone = false;
  auto forcedFamily = [](const String &p) -> uint8_t {
    if (p == "ANT") return 2;
    if (p == "JBD") return 3;
    if (p == "DALY") return 4;
    if (p == "JK02_24S" || p == "JK02_32S") return 1;
    return 0;
  };
  auto receive = [&](const uint8_t *b) {
    if (!fam && (b[4] == 2 || b[4] == 3)) fam = 1;
    Guard g;
    shared.ftype[b[4] <= 3 ? b[4] : 0]++;
    if (b[4] == 2) {
      static const char hx[] = "0123456789ABCDEF";
      String h;
      h.reserve(600);
      for (int i = 0; i < 300; i++) { h += hx[b[i] >> 4]; h += hx[b[i] & 15]; }
      shared.f2hex = h;
    }
    if (generation != seen)
      return;
    shared.good = parser.good;
    shared.bad = parser.bad;
    if (b[4] == 2) {
      // v1.3: JK BMS does not require the app code to stream telemetry over
      // BLE. The code check is informational only and never blocks reading.
      gotCellFrame = true;
      lastCellFrame = millis();
      jk::Sample s;
      bool accepted = false;
      if (c.protocol == "AUTO") {
        int layout = detector.accept(b, s);
        if (layout) {
          String detected = layout == 24 ? "JK02_24S" : "JK02_32S";
          if (shared.detected != detected) {
            shared.haveParams = false;
            shared.detected = detected;
            readWanted = true;
          }
          accepted = true;
          if (!persistWanted && shared.state != "LIVE")
            persistWanted = true;
        } else {
          shared.have = false;
          shared.state = "VERIFYING_PROTOCOL";
        }
      } else {
        accepted = jk::decode(b, c.protocol == "JK02_32S", s);
        shared.detected = accepted ? c.protocol : "";
      }
      if (accepted) {
        shared.sample = s;
        shared.have = true;
        shared.stamp = millis();
        shared.state = "LIVE";
      } else {
        shared.rejected++;
        shared.have = false;
        if (c.protocol != "AUTO")
          shared.state = "PROFILE_MISMATCH";
      }
    } else if (b[4] == 3) {
      shared.model = asciiField(b + 6, 16);
      shared.hardware = asciiField(b + 22, 8);
      shared.software = asciiField(b + 30, 8);
      gotInfo = true;
      Serial.printf("[BLE] device info: model=%s hw=%s sw=%s\n",
                    shared.model.c_str(), shared.hardware.c_str(),
                    shared.software.c_str());
      // Informational code check only; telemetry is read either way.
      std::string expectedConnection, expectedParameter;
      {
        std::string cb, cs;
        shared.codeBt = jk::readCode(b, 300, 62, cb) ? String(cb.c_str()) : "";
        shared.codeSet = jk::readCode(b, 300, 118, cs) ? String(cs.c_str()) : "";
      }
      if (!c.bluetoothCode.length())
        shared.connectionAuth = "NOT_SET";
      else if (!jk::readCode(b, 300, 62, expectedConnection))
        shared.connectionAuth = "UNSUPPORTED_INFO";
      else if (jk::sameCode(c.bluetoothCode.c_str(), expectedConnection))
        shared.connectionAuth = "VERIFIED";
      else
        shared.connectionAuth = "WRONG_CODE";
      if (shared.state != "LIVE")
        shared.state = "WAITING_DATA";
      shared.parameterAuth = "LOCKED";
      if (shared.connectionAuth == "VERIFIED" && c.parameterCode.length()) {
        if (!jk::readCode(b, 300, 118, expectedParameter))
          shared.parameterAuth = "UNSUPPORTED_INFO";
        else shared.parameterAuth = jk::sameCode(c.parameterCode.c_str(), expectedParameter) ? "VERIFIED_READ_ONLY" : "WRONG_CODE";
      }
    } else if (b[4] == 1) {
      bmsw::JkSettings js;
      if (bmsw::parseJkSettings(b, js)) {
        shared.jk = js;
        shared.jkStamp = millis();
      }
    }
    if (b[4] == 1 && (c.protocol != "AUTO" || shared.detected.length())) {
      jk::Parameters p;
      if (jk::parameters(b, p)) {
        shared.params = p;
        shared.haveParams = true;
        shared.paramStamp = millis();
      }
    }
  };
  // NimBLE callback only queues bytes; parsing is performed by this task.
  struct Chunk {
    uint16_t n;
    uint8_t data[512];
  };
  static QueueHandle_t queue = xQueueCreate(40, sizeof(Chunk));
  static volatile uint32_t dropped = 0, notifyBytes = 0, notifyChunks = 0;
  static volatile uint8_t lastRx[48];
  static volatile size_t lastRxLen = 0;
  bool withResp = false;
  static NimBLERemoteCharacteristic *volatile lastRxFrom = nullptr;
  static volatile uint8_t winBuf[160];
  static volatile size_t winLen = 0, winChunks = 0;
  uint32_t goodBefore = 0;
  int jkStep = -1;
  // Probe table: which bytes to send to which characteristic until the BMS
  // answers. Covers JK (0x97/0x96), JBD/Xiaoxiang, ANT and Daly requests.
  struct Probe { const char *name; uint8_t len; uint8_t data[20]; };
  static Probe probes[5];
  {
    auto j7 = jk::request(0x97), j6 = jk::request(0x96);
    probes[0] = {"JK_0x97", 20, {}}; memcpy(probes[0].data, j7.data(), 20);
    probes[1] = {"JK_0x96", 20, {}}; memcpy(probes[1].data, j6.data(), 20);
    probes[2] = {"JBD_03", 7, {0xDD,0xA5,0x03,0x00,0xFF,0xFD,0x77}};
    probes[3] = {"ANT_01", 10, {0x7E,0xA1,0x01,0x00,0x00,0xBE,0x18,0x55,0xAA,0x55}};
    probes[4] = {"DALY_90", 13, {0xA5,0x80,0x90,0x08,0,0,0,0,0,0,0,0,0xBD}};
  }
  struct Step { NimBLERemoteCharacteristic *target; uint8_t probe; bool resp; };
  std::vector<Step> steps;
  int probeStep = -1;
  uint32_t probeAt = 0;
  auto notify = [](NimBLERemoteCharacteristic *from, uint8_t *p, size_t n, bool) {
    lastRxFrom = from;
    for (size_t i = 0; i < n && winLen < sizeof(winBuf); i++)
      winBuf[winLen++] = p[i];
    winChunks++;
    if (n > 512)
      return;
    static Chunk q;
    q.n = n;
    memcpy(q.data, p, n);
    notifyBytes += n;
    notifyChunks++;
    lastRxLen = n < sizeof(lastRx) ? n : sizeof(lastRx);
    memcpy((void *)lastRx, p, lastRxLen);
    if (xQueueSend(queue, &q, 0) != pdTRUE)
      dropped++;
  };
#ifndef BATTLAB_DISPLAY
  // ---- RS485 bank: Bluetooth is used only to match packs and to change their settings ----
  static uint8_t pjBuf[300], pjBuf2[300];
  auto pjConnect = [&](const char *mac, uint8_t type, NimBLERemoteCharacteristic *&w) -> bool {
    w = nullptr;
    if (client->isConnected()) client->disconnect();
    xQueueReset(queue);
    if (!client->connect(NimBLEAddress(mac, type))) return false;
    NimBLERemoteService *sv = client->getService(NimBLEUUID(uint16_t(0xffe0)));
    NimBLERemoteCharacteristic *c1 = sv ? sv->getCharacteristic(NimBLEUUID(uint16_t(0xffe1))) : nullptr;
    if (!c1 || !c1->canNotify() || !c1->subscribe(true, notify)) { client->disconnect(); return false; }
    w = c1;
    return true;
  };
  auto pjWait = [&](jk::Parser &P, uint8_t want, uint32_t ms, uint8_t *out) -> bool {
    uint32_t t0 = millis();
    bool got = false;
    Chunk ck;
    while (!got && millis() - t0 < ms && client->isConnected()) {
      if (xQueueReceive(queue, &ck, pdMS_TO_TICKS(50)) == pdTRUE)
        P.feed(ck.data, ck.n, [&](const uint8_t *b) { if (b[4] == want && !got) { memcpy(out, b, 300); got = true; } });
    }
    return got;
  };
  auto pjSend = [&](NimBLERemoteCharacteristic *w, uint8_t cmd) {
    auto b = jk::request(cmd);
    w->writeValue(b.data(), b.size(), !w->canWriteNoResponse());
  };
  auto setBtm = [&](const String &st) { Guard g; btmStatus = st; };
  auto runMatch = [&]() {
    setBtm("scan");
    scan->clearResults();
    NimBLEScanResults res = scan->start(8, false);
    struct Cand { std::string mac; uint8_t type; std::string name; };
    std::vector<Cand> cands;
    for (int i = 0; i < res.getCount() && cands.size() < 20; i++) {
      auto dev = res.getDevice(i);
      String up = dev.getName().c_str();
      up.toUpperCase();
      if (up.startsWith("JK") || (dev.haveServiceUUID() && dev.isAdvertisingService(NimBLEUUID(uint16_t(0xffe0)))))
        cands.push_back({dev.getAddress().toString(), dev.getAddressType(), dev.getName().substr(0, 19)});
    }
    jk::Sample ps[16];
    bool have[16];
    char ser[16][12];
    {
      Guard g;
      uint32_t now = millis();
      for (int k = 0; k < 16; k++) {
        have[k] = shared.packs[k].stamp && now - shared.packs[k].stamp < 180000;
        ps[k] = shared.packs[k].s;
        memcpy(ser[k], shared.packs[k].serial, 12);
      }
    }
    struct Got { int c; char serial[12]; jk::Sample s; bool hv; };
    std::vector<Got> got;
    for (size_t j = 0; j < cands.size(); j++) {
      setBtm(String("check:") + (j + 1) + "/" + cands.size());
      NimBLERemoteCharacteristic *w;
      if (!pjConnect(cands[j].mac.c_str(), cands[j].type, w)) continue;
      jk::Parser P;
      Got g{int(j), {0}, jk::Sample(), false};
      pjSend(w, 0x97);
      if (pjWait(P, 3, 3000, pjBuf)) serialOf(pjBuf, g.serial);
      pjSend(w, 0x96);
      if (pjWait(P, 2, 4000, pjBuf)) g.hv = jk::decode(pjBuf, true, g.s) || jk::decode(pjBuf, false, g.s);
      client->disconnect();
      if (g.serial[0] || g.hv) got.push_back(g);
      vTaskDelay(pdMS_TO_TICKS(300));
    }
    int asg[16];
    for (int k = 0; k < 16; k++) asg[k] = -1;
    std::vector<bool> used(got.size(), false);
    for (int k = 0; k < 16; k++) // 1) exact serial number
      if (have[k] && ser[k][0])
        for (size_t g = 0; g < got.size(); g++)
          if (!used[g] && !strcmp(ser[k], got[g].serial)) { asg[k] = g; used[g] = true; break; }
    for (int k = 0; k < 16; k++) { // 2) fingerprint: cell voltages, cycles, SOC
      if (!have[k] || asg[k] >= 0) continue;
      float best = 1e9, second = 1e9;
      int bi = -1;
      for (size_t g = 0; g < got.size(); g++) {
        if (used[g] || !got[g].hv) continue;
        const jk::Sample &a = ps[k], &b = got[g].s;
        if (a.count != b.count || !a.count) continue;
        float d = 0;
        for (unsigned i = 0; i < 32; i++) d += std::fabs(a.cells[i] - b.cells[i]) * 1000;
        d = d / a.count + std::fabs(float(a.cycles) - float(b.cycles)) * 200 + std::fabs(a.soc - b.soc) * 20;
        if (d < best) { second = best; best = d; bi = g; } else if (d < second) second = d;
      }
      if (bi >= 0 && best < 60 && second - best > 10) { asg[k] = bi; used[bi] = true; }
    }
    int n = 0, total = 0;
    {
      Guard g;
      for (int k = 0; k < 16; k++) {
        if (have[k]) total++;
        if (asg[k] < 0) continue;
        const Cand &cd = cands[got[asg[k]].c];
        btMap[k] = BtLink();
        strncpy(btMap[k].mac, cd.mac.c_str(), 17);
        btMap[k].type = cd.type;
        strncpy(btMap[k].name, cd.name.c_str(), sizeof(btMap[k].name) - 1);
        n++;
      }
    }
    btSave();
    setBtm(String("done:") + n + "/" + total + ":" + cands.size());
  };
  auto runJob = [&](const PackJob &j) {
    BtLink L;
    {
      Guard g;
      L = btMap[j.a];
    }
    NimBLERemoteCharacteristic *w = nullptr;
    pwSet(j.id, j.a, "", 0);
    if (!L.mac[0] || (!pjConnect(L.mac, L.type, w) && !pjConnect(L.mac, L.type, w))) { pwSet(j.id, j.a, "", 4); return; }
    jk::Parser P;
    pjSend(w, 0x97);
    pjWait(P, 3, 2000, pjBuf);
    pjSend(w, 0x96);
    bmsw::JkSettings before;
    bool hb = pjWait(P, 1, 5000, pjBuf) && bmsw::parseJkSettings(pjBuf, before);
    bool is32 = true;
    if (pjWait(P, 2, 2500, pjBuf2)) { jk::Sample t; is32 = jk::decode(pjBuf2, true, t) || !jk::decode(pjBuf2, false, t); }
    std::array<uint8_t, 20> f;
    if (j.isSwitch) f = bmsw::jkWriteFrame(bmsw::JK_SW_REG[j.idx], j.on ? 1 : 0);
    else {
      const auto &p = bmsw::JK_PARAMS[j.idx];
      if (hb) {
        const char *e = bmsw::checkJkValue(before, j.idx, j.value);
        if (e) { client->disconnect(); pwSet(j.id, j.a, String("refused: ") + e, 3); return; }
      }
      f = bmsw::jkWriteFrame(is32 ? p.reg32 : p.reg24, bmsw::jkRaw(j.idx, j.value));
    }
    w->writeValue(f.data(), f.size(), !w->canWriteNoResponse());
    pwSet(j.id, j.a, "", 1);
    delay(600);
    xQueueReset(queue);
    P.reset();
    pjSend(w, 0x96);
    bmsw::JkSettings after;
    bool ha = pjWait(P, 1, 6000, pjBuf) && bmsw::parseJkSettings(pjBuf, after);
    bool same = false;
    if (ha) same = j.isSwitch ? after.sw[j.idx] == j.on : std::fabs(after.v[j.idx] - j.value) <= bmsw::JK_PARAMS[j.idx].step / 2 + 1e-4f;
    client->disconnect();
    {
      Guard g;
      if (ha) { shared.packs[j.a].js = after; shared.packs[j.a].setStamp = millis(); }
      shared.lastWrite = String("Pack #") + j.a + (same ? ": verified" : ha ? ": BMS kept the old value" : ": no answer after writing");
    }
    pwSet(j.id, j.a, "", same ? 2 : 3);
  };
  auto packJobs = [&]() {
    if (btClearWanted) {
      btClearWanted = false;
      { Guard g; for (auto &b : btMap) b = BtLink(); btmStatus = ""; }
      btSave();
    }
    if (btMatchWanted) { btMatchWanted = false; runMatch(); }
    PackJob j;
    bool have = false;
    {
      Guard g;
      if (pjN) { j = pjQ[0]; memmove(pjQ, pjQ + 1, sizeof(PackJob) * (pjN - 1)); pjN--; have = true; }
    }
    if (have) runJob(j);
  };
#endif
  for (;;) {
    if (otaBusy || bleOtaBusy) { // firmware download: Bluetooth client stays idle
      if (client->isConnected()) client->disconnect();
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }
    if (linkRs485) { // BMS on the RS485 cable: Bluetooth only for bank jobs
#ifndef BATTLAB_DISPLAY
      if (client->isConnected()) client->disconnect();
      packJobs();
#endif
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (bleTxApplyWanted) {
      bleTxApplyWanted = false;
      applyBleTxPower();
    }
    bool change, wantScan, wantRead;
    {
      Guard g;
      change = configChanged;
      configChanged = false;
      wantScan = scanWanted;
      scanWanted = false;
      wantRead = readWanted;
      readWanted = false;
      c = cfg;
      seen = generation;
    }
    if (change) {
      needsSelection = false;
      discoveryAt = 0;
      detector.reset();
      if (client->isConnected())
        client->disconnect();
      ch = nullptr;
      parser.reset();
      detector.reset();
      xQueueReset(queue);
      lastTry = 0;
      {
        Guard g;
        shared.have = false;
        shared.haveParams = false;
        shared.detected = "";
        shared.model = "";
        shared.hardware = "";
        shared.software = "";
        shared.linked = false;
      }
    }
    bool autoScan = c.automatic && !c.mac.length() && !needsSelection &&
                    (!discoveryAt || uint32_t(millis() - discoveryAt) > 20000);
    if (wantScan || autoScan) {
      if (client->isConnected())
        client->disconnect();
      ch = nullptr;
      {
        Guard g;
        shared.scanning = true;
        shared.linked = false;
        shared.connectionAuth = "WAITING_INFO";
        shared.parameterAuth = "LOCKED";
        shared.writeHandle = shared.notifyHandle = 0;
        shared.have = false;
        shared.haveParams = false;
        shared.state = "SCANNING";
      }
      auto results = scan->start(4, false);
      DynamicJsonDocument d(6144);
      auto arr = d.to<JsonArray>();
      unsigned candidates = 0;
      String candidateMac;
      uint8_t candidateType = 0;
      for (int i = 0; i < results.getCount(); i++) {
        auto dev = results.getDevice(i);
        auto o = arr.createNestedObject();
        auto name = dev.getName();
        if (name.size() > 48)
          name.resize(48);
        String upper = name.c_str();
        upper.toUpperCase();
        bool candidate =
            upper.startsWith("JK") || upper.startsWith("ANT") ||
            upper.startsWith("JBD") || upper.startsWith("XIAOXIANG") ||
            upper.startsWith("DL-") || upper.startsWith("DALY") ||
            (dev.haveServiceUUID() &&
             (dev.isAdvertisingService(NimBLEUUID(uint16_t(0xffe0))) ||
              dev.isAdvertisingService(NimBLEUUID(uint16_t(0xff00)))));
        o["candidate"] = candidate;
        if (candidate) {
          candidates++;
          candidateMac = dev.getAddress().toString().c_str();
          candidateType = dev.getAddressType();
        }
        o["name"] = name;
        o["mac"] = dev.getAddress().toString();
        o["type"] = dev.getAddressType();
        o["rssi"] = dev.getRSSI();
      }
      String s;
      serializeJson(d, s);
      scan->clearResults();
      {
        Guard g;
        shared.devices = s;
        shared.devStamp = millis() | 1;
        shared.scanning = false;
        if (generation == seen && autoScan) {
          if (candidates == 1) {
            cfg.mac = candidateMac;
            cfg.type = candidateType;
            cfg.protocol = "AUTO";
            configChanged = true;
            generation++;
            shared.state = "AUTO_FOUND";
          } else {
            needsSelection = candidates > 1;
            shared.state = candidates ? "SELECT_BMS" : "NO_JK_FOUND";
          }
        } else if (generation == seen)
          shared.state = "DISCONNECTED";
      }
      discoveryAt = millis();
      lastTry = millis();
      if (autoScan) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }
    if (!client->isConnected()) {
      linkedLocal = false;
      // NimBLE frees all remote attribute objects on (re)connect: drop every
      // pointer we hold to them.
      lastRxFrom = nullptr;
      antCh = jbdCh = nullptr;
      {
        Guard g;
        shared.family = 0;
        shared.pvHave = 0;
        cmdN = 0; // never replay a queued write onto a new connection
      }
      steps.clear();
      probeStep = -1;
      prStep = -1;
      prDone = false;
      vIdx = -1;
      if (ch)
        { Guard g; shared.links++; shared.lastLinkMs = millis() - linkedAt; }
        Serial.printf("[BLE] link lost after %lu ms (frames ok=%lu)\n",
                      (unsigned long)(millis() - linkedAt),
                      (unsigned long)parser.good);
      ch = nullptr;
      {
        Guard g;
        shared.linked = false;
        shared.connectionAuth = "WAITING_INFO";
        shared.parameterAuth = "LOCKED";
        shared.writeHandle = shared.notifyHandle = 0;
        shared.have = false;
        shared.haveParams = false;
        if (!c.mac.length() && !c.automatic)
          shared.state = "UNCONFIGURED";
        else if (shared.state == "LIVE")
          shared.state = "DISCONNECTED";
      }
      if (c.mac.length() &&
          (!lastTry || uint32_t(millis() - lastTry) > 10000)) {
        lastTry = millis();
        {
          Guard g;
          shared.state = "CONNECTING";
          shared.retries++;
        }
        parser.reset();
        detector.reset();
        {
          Guard g;
          shared.detected = "";
        }
        xQueueReset(queue);
        if (client->connect(NimBLEAddress(c.mac.c_str(), c.type))) {
          // Discover the whole GATT table ONCE (refresh=true). Every later
          // lookup uses the cached objects (refresh=false): refreshing again
          // deletes the old objects and leaves earlier pointers dangling,
          // which crashed the classic ESP32 right after connecting.
          String gatt;
          if (auto svcs = client->getServices(true))
            for (auto sv : *svcs) {
              gatt += sv->getUUID().toString().c_str();
              gatt += "{";
              if (auto cs = sv->getCharacteristics(true))
                for (auto cc : *cs) {
                  gatt += cc->getUUID().toString().c_str();
                  gatt += ":";
                  if (cc->canRead()) gatt += "R";
                  if (cc->canWrite()) gatt += "W";
                  if (cc->canWriteNoResponse()) gatt += "w";
                  if (cc->canNotify()) gatt += "N";
                  if (cc->canIndicate()) gatt += "I";
                  gatt += " ";
                }
              gatt += "} ";
            }
          NimBLERemoteService *service = nullptr;
          if (auto svcs = client->getServices(false))
            for (auto sv : *svcs)
              if (sv->getUUID() == NimBLEUUID(uint16_t(0xffe0))) service = sv;
          ch = nullptr;
          NimBLERemoteCharacteristic *notifyCh = nullptr;
          if (service) {
            auto chars = service->getCharacteristics(false);
            if (chars) jk::selectChannels(*chars, ch, notifyCh,
              [](NimBLERemoteCharacteristic *candidate) {
                return candidate->getUUID() == NimBLEUUID(uint16_t(0xffe1));
              });
          }
          Serial.printf("[BLE] connected %s, service=%s tx=%s rx=%s mtu=%u\n",
                        c.mac.c_str(), service ? "FFE0" : "none",
                        ch ? "ok" : "none", notifyCh ? "ok" : "none",
                        (unsigned)client->getMTU());
          bleMtu = client->getMTU();
          Serial.printf("[BLE] GATT: %s\n", gatt.c_str());
          bool subscribed = false;
          String subLog;
          String peer;
          if (auto gap = client->getService(NimBLEUUID(uint16_t(0x1800))))
            if (auto nm = gap->getCharacteristic(NimBLEUUID(uint16_t(0x2a00))))
              if (nm->canRead()) peer = nm->readValue().c_str();
          steps.clear();
          probeStep = -1;
          if (auto svcs = client->getServices(false))
            for (auto sv : *svcs) {
              // Only the BMS data services (JK/ANT FFE0, JBD FF00, DALY FFF0). Never touch
              // anything else: the new JK module (ZX-D30, Nordic) exposes its own
              // firmware-update service (FE59) and vendor services.
              if (!(sv->getUUID() == NimBLEUUID(uint16_t(0xffe0)) ||
                    sv->getUUID() == NimBLEUUID(uint16_t(0xff00)) ||
                    sv->getUUID() == NimBLEUUID(uint16_t(0xfff0))))
                continue;
              auto cs = sv->getCharacteristics(false);
              if (!cs) continue;
              for (auto cc : *cs) {
                bool jkOnlyFfe1 = sv->getUUID() == NimBLEUUID(uint16_t(0xffe0)) &&
                                  cc->getUUID() != NimBLEUUID(uint16_t(0xffe1)) &&
                                  sv->getCharacteristic(NimBLEUUID(uint16_t(0xffe1)));
                if ((cc->canNotify() || cc->canIndicate()) && !jkOnlyFfe1) {
                  bool ok = false;
                  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
                    // CCCD write with response (a write command is ignored by some stacks)
                    ok = cc->canNotify() ? cc->subscribe(true, notify, true)
                                         : cc->subscribe(false, notify, true);
                    if (!ok) delay(200);
                  }
                  Serial.printf("[BLE] subscribe %s -> %s\n",
                                cc->getUUID().toString().c_str(), ok ? "ok" : "FAILED");
                  subLog += String(cc->getUUID().toString().c_str()) + (ok ? ":ok " : ":FAIL ");
                  subscribed |= ok;
                }
                for (uint8_t pi = 0; pi < 5; pi++) {
                  if (cc->canWriteNoResponse()) steps.push_back({cc, pi, false});
                  if (cc->canWrite()) steps.push_back({cc, pi, true});
                }
              }
            }
          {
            size_t n1 = steps.size();
            for (size_t i = 0; i < n1; i++)
              if (steps[i].probe <= 1 &&
                  steps[i].target->getUUID() == NimBLEUUID(uint16_t(0xffe1)))
                steps.push_back(steps[i]);
          }
          Serial.printf("[BLE] probe plan: %u steps\n", (unsigned)steps.size());
          {
            Guard g;
            if (!shared.probeLogFrozen) shared.probeLog = "";
          }
          if (ch) withResp = !ch->canWriteNoResponse();
          antCh = ch;
          jbdCh = nullptr;
          if (auto svcs = client->getServices(false)) // cached, no rediscovery
            for (auto sv : *svcs)
              if (sv->getUUID() == NimBLEUUID(uint16_t(0xff00)))
                if (auto cs = sv->getCharacteristics(false))
                  for (auto cc : *cs)
                    if (cc->getUUID() == NimBLEUUID(uint16_t(0xff02))) jbdCh = cc;
          dalyCh = nullptr;
          if (auto svcs = client->getServices(false))
            for (auto sv : *svcs)
              if (sv->getUUID() == NimBLEUUID(uint16_t(0xfff0)))
                if (auto cs = sv->getCharacteristics(false))
                  for (auto cc : *cs)
                    if (cc->getUUID() == NimBLEUUID(uint16_t(0xfff2))) dalyCh = cc;
          dD2.reset(); dA5.reset(); dA5st.reset(); dalySub = 0; dalyIdx = 0;
          fam = forcedFamily(c.protocol);
          if (!fam && jbdCh) fam = 3; // service FF00 is specific to JBD
          antF.reset();
          jbdF.reset();
          jbdHaveBasic = false;
          jbdSample = jk::Sample();
          identAt = millis(); // let the module settle ~1 s before the first request
          identIdx = 0;
          identStartedAt = millis();
          fullProbeDone = false;
          lastOtherPoll = 0;
          Serial.printf("[BLE] channels: jk/ant=%s jbd=%s family=%u\n",
                        ch ? "FFE1" : "-", jbdCh ? "FF02" : "-", fam);
          peerIsAnt = peer.startsWith("ANT");
          {
            Guard g;
            shared.gatt = gatt;
            shared.subLog = subLog;
            shared.peerName = peer;
          }
          if (subscribed && (ch || jbdCh || dalyCh || !steps.empty())) {
            linkedLocal = true;
            probeStep = -1; // run quick identification first
            probeAt = 0;
            notifyBytes = 0;
            notifyChunks = 0;
            lastRxLen = 0;
            lastRxFrom = nullptr;
            winLen = 0;
            winChunks = 0;
            jkStep = -1;
            goodBefore = parser.good;
            gotInfo = gotCellFrame = false;
            linkedAt = millis();
            lastCellFrame = lastCellRequest = 0;
            cellRequests = 0;
            Guard g;
            shared.linked = true;
            shared.state = "WAITING_DEVICE_INFO";
            applyBleTxPower(); // per-connection power on the new link
            shared.connectionAuth = "WAITING_INFO";
            shared.parameterAuth = "LOCKED";
            shared.bleError = 0;
            shared.writeHandle = ch ? ch->getHandle() : 0;
            shared.probe = "identifying";
            shared.notifyHandle = notifyCh ? notifyCh->getHandle() : 0;
            lastPoll = 0;
            lastInfo = 0;
          } else {
            Serial.println("[BLE] FFE0/FFE1 not usable or subscribe failed");
            client->disconnect();
            ch = nullptr;
            Guard g;
            shared.state = "GATT_UNSUPPORTED";
            shared.bleError = client->getLastError();
          }
        } else {
          int err = client->getLastError();
          Serial.printf("[BLE] connect to %s (type %u) failed, err=%d\n",
                        c.mac.c_str(), (unsigned)c.type, err);
          Guard g;
          shared.state = "CONNECT_FAILED";
          shared.bleError = err;
        }
      }
    } else if (linkedLocal) {
      auto publishOther = [&](const jk::Sample &smp, const char *name) {
        gotCellFrame = true;
        lastCellFrame = millis();
        Guard g;
        if (generation != seen) return;
        shared.sample = smp;
        shared.have = true;
        shared.stamp = millis();
        shared.state = "LIVE";
        shared.detected = name;
        shared.connectionAuth = "NOT_SET";
        shared.good = antF.good + jbdF.good + parser.good;
      };
      auto storeSetting = [&](uint8_t f, int i, float v) {
        int n;
        const bset::Def *tb = bset::table(f, n);
        if (!tb || i < 0 || i >= n) return;
        Guard g;
        shared.pv[i] = v;
        shared.pvHave |= uint64_t(1) << i;
        if (vIdx == i) {
          const bset::Def &d = tb[i];
          bool same = std::fabs(v - vVal) <= d.step / 2 + 1e-4f;
          shared.lastWrite = (same ? "Verified: " : "Not changed: ") + String(d.name) + " = " +
                             String(v, int(d.dec)) + " " + d.unit + (same ? "" : " (BMS kept the old value)");
          vIdx = -1;
        }
      };
      Chunk chunk;
      while (xQueueReceive(queue, &chunk, 0) == pdTRUE) {
        parser.feed(chunk.data, chunk.n, receive);
        antF.feed(chunk.data, chunk.n, [&](const uint8_t *f, size_t n) {
          if (n >= 10 && f[2] == 0x12) { // settings register read back
            uint16_t addr = f[3] | uint16_t(f[4]) << 8;
            uint8_t len = f[5];
            if (len == 2 || len == 4) {
              int32_t raw = len == 4 ? int32_t(f[6] | f[7] << 8 | f[8] << 16 | uint32_t(f[9]) << 24) : int32_t(f[6] | f[7] << 8);
              for (int i = 0; i < bset::ANT_N; i++)
                if (bset::ANT[i].reg == addr) storeSetting(2, i, bset::decode(bset::ANT[i], raw));
            }
            return;
          }
          jk::Sample smp;
          if (bms::decodeAnt(f, n, smp)) {
            if (!fam) fam = 2;
            if (fam == 2) publishOther(smp, "ANT");
          } else {
            Guard g;
            shared.rejected++;
          }
        });
        dD2.feed(chunk.data, chunk.n, [&](uint8_t st, uint8_t fn, const uint8_t *d, size_t n) {
          if (st != 0xD2 || fn != 3) return;
          jk::Sample smp;
          uint8_t bal;
          if (daly::decodeBle(d, n, smp, bal)) {
            if (!fam) fam = 4;
            if (fam == 4) { dalySub = 1; publishOther(smp, "DALY"); }
          }
        });
        dA5.feed(chunk.data, chunk.n, [&](uint8_t id, const uint8_t *d) {
          if (dA5st.add(id, d)) {
            if (!fam) fam = 4;
            if (fam == 4) { dalySub = 2; publishOther(dA5st.s, "DALY (A5)"); }
          } else if (!fam && id == 0x90) dalySub = 2;
        });
        jbdF.feed(chunk.data, chunk.n, [&](uint8_t cmd, const uint8_t *d, size_t n) {
          if (fam == 3 && cmd >= 0x10 && cmd <= 0x2F && n == 2) { // settings register read back
            for (int i = 0; i < bset::JBD_N; i++)
              if (bset::JBD[i].reg == cmd) {
                const bset::Def &df = bset::JBD[i];
                int32_t raw = df.sgn ? int32_t(int16_t(d[0] << 8 | d[1])) : int32_t(d[0] << 8 | d[1]);
                storeSetting(3, i, bset::decode(df, raw));
              }
            return;
          }
          bool ok = cmd == 3 ? bms::decodeJbdBasic(d, n, jbdSample)
                  : cmd == 4 ? bms::decodeJbdCells(d, n, jbdSample) : false;
          if (!ok) return;
          if (!fam) fam = 3;
          if (cmd == 3) jbdHaveBasic = true;
          if (fam == 3 && jbdHaveBasic && jbdSample.count) {
            float total = 0;
            for (unsigned i = 0; i < jbdSample.count; i++) total += jbdSample.cells[i];
            if (std::fabs(jbdSample.voltage - total) <= std::fmax(2.f, total * .05f))
              publishOther(jbdSample, "JBD");
          }
        });
      }
      {
        uint32_t t = millis();
        auto writeRaw = [&](NimBLERemoteCharacteristic *w, const uint8_t *d,
                            size_t n, const char *what) {
          if (!w) return false;
          bool resp = !w->canWriteNoResponse();
          bool ok = w->writeValue(d, n, resp);
          Serial.printf("[BLE] -> %s %s\n", what, ok ? "ok" : "FAILED");
          Guard g;
          if (ok) shared.txOk++; else shared.txFail++;
          return ok;
        };
        {
          Guard g;
          shared.family = fam;
        }
        BmsCmd cmd;
        bool haveCmd = false;
        {
          Guard g;
          if (cmdN) {
            cmd = cmdQ[0];
            memmove(cmdQ, cmdQ + 1, sizeof(BmsCmd) * (cmdN - 1));
            cmdN--;
            haveCmd = true;
          }
        }
        if (haveCmd) {
          String result;
          bool ok = false;
          bool is32;
          {
            Guard g;
            is32 = shared.detected == "JK02_32S" || c.protocol == "JK02_32S";
          }
          if (fam == 1 && ch) {
            std::array<uint8_t, 20> f;
            if (cmd.isSwitch) {
              f = bmsw::jkWriteFrame(bmsw::JK_SW_REG[cmd.idx], cmd.on ? 1 : 0);
              result = String(cmd.idx == 0 ? "Charge" : cmd.idx == 1 ? "Discharge" : "Balancer") +
                       (cmd.on ? " ON" : " OFF");
            } else {
              const auto &p = bmsw::JK_PARAMS[cmd.idx];
              f = bmsw::jkWriteFrame(is32 ? p.reg32 : p.reg24, bmsw::jkRaw(cmd.idx, cmd.value));
              result = String(p.name) + " = " + String(cmd.value, int(p.decimals)) + " " + p.unit;
            }
            ok = ch->writeValue(f.data(), f.size(), withResp);
            delay(250);
            Guard g;
            readWanted = true; // 0x96 makes the BMS resend its settings
          } else if (fam == 2 && antCh && cmd.isSwitch) {
            auto a = bmsw::antAuthFrame();
            antCh->writeValue(a.data(), a.size(), !antCh->canWriteNoResponse());
            delay(150);
            auto f = bmsw::antWriteFrame(cmd.on ? bmsw::ANT_SW_ON[cmd.idx] : bmsw::ANT_SW_OFF[cmd.idx], 0);
            ok = antCh->writeValue(f.data(), f.size(), !antCh->canWriteNoResponse());
            result = String(cmd.idx == 0 ? "Charge" : cmd.idx == 1 ? "Discharge" : "Balancer") +
                     (cmd.on ? " ON" : " OFF");
          } else if (fam == 4 && cmd.isSwitch) {
            uint8_t fr[16];
            int k = 0;
            NimBLERemoteCharacteristic *w = dalyCh ? dalyCh : ch;
            if (dalySub == 2) {
              if (cmd.idx < 2) k = daly::a5Frame(fr, 0x80, cmd.idx == 0 ? 0xDA : 0xD9, cmd.on ? 1 : 0);
            } else k = daly::d2Frame(fr, 0xD2, 6, daly::BLE_MOS_REG[cmd.idx], cmd.on ? 1 : 0);
            if (k && w) ok = w->writeValue(fr, k, !w->canWriteNoResponse());
            result = String(cmd.idx == 0 ? "Charge" : cmd.idx == 1 ? "Discharge" : "Balancer") + (cmd.on ? " ON" : " OFF") +
                     (k ? "" : " (not on this DALY)");
          } else if (fam == 3 && cmd.isSwitch) {
            bool chg = jbdSample.charge, dsg = jbdSample.discharge;
            if (cmd.idx == 0) chg = cmd.on;
            if (cmd.idx == 1) dsg = cmd.on;
            auto f = bmsw::jbdMosFrame(chg, dsg);
            NimBLERemoteCharacteristic *w = jbdCh ? jbdCh : ch;
            if (w) ok = w->writeValue(f.data(), f.size(), !w->canWriteNoResponse());
            result = String(cmd.idx == 0 ? "Charge" : "Discharge") + (cmd.on ? " ON" : " OFF");
          } else if ((fam == 2 || fam == 3) && !cmd.isSwitch) {
            int n;
            const bset::Def *tb = bset::table(fam, n);
            if (cmd.idx < n) {
              const bset::Def &p = tb[cmd.idx];
              uint8_t fr[20];
              int k;
              int32_t raw = bset::encode(p, cmd.value);
              if (fam == 3) { // JBD: factory mode -> write -> save + exit, then read everything back
                NimBLERemoteCharacteristic *w = jbdCh ? jbdCh : ch;
                if (w) {
                  bool r = !w->canWriteNoResponse();
                  k = bset::jbdFactory(fr); w->writeValue(fr, k, r); delay(300);
                  k = bset::jbdWrite(fr, p.reg, raw); ok = w->writeValue(fr, k, r); delay(300);
                  k = bset::jbdExit(fr, true); w->writeValue(fr, k, r); delay(500);
                }
                prStep = 0;
              } else if (antCh) { // ANT (experimental): auth -> write -> read the register back
                bool r = !antCh->canWriteNoResponse();
                auto a = bmsw::antAuthFrame();
                antCh->writeValue(a.data(), a.size(), r); delay(150);
                k = bset::antWrite(fr, p, raw); ok = antCh->writeValue(fr, k, r); delay(400);
                k = bset::antRead(fr, p); antCh->writeValue(fr, k, r);
              }
              vIdx = cmd.idx;
              vVal = cmd.value;
              vAt = millis();
              result = String(p.name) + " = " + String(cmd.value, int(p.dec)) + " " + p.unit + " - checking...";
            }
          } else {
            result = "not supported for this BMS";
          }
          Serial.printf("[BMS] write %s -> %s\n", result.c_str(), ok ? "sent" : "FAILED");
          Guard g;
          shared.lastWrite = (ok ? "Sent: " : "Failed: ") + result;
        }
        if (wantRead && (fam == 2 || fam == 3)) prStep = 0;
        if (!prDone && prStep < 0 && (fam == 2 || fam == 3) && t - linkedAt > 4000) prStep = 0;
        if (vIdx >= 0 && millis() - vAt > 10000) {
          Guard g;
          shared.lastWrite = "No answer to the check read - press READ to see the value";
          vIdx = -1;
        }
        if (prStep >= 0 && (fam == 2 || fam == 3) && t - prAt > 150) {
          prAt = t;
          uint8_t fr[20];
          int k = 0, n;
          const bset::Def *tb = bset::table(fam, n);
          if (fam == 3) {
            NimBLERemoteCharacteristic *w = jbdCh ? jbdCh : ch;
            if (prStep == 0) k = bset::jbdFactory(fr);
            else if (prStep <= n) k = bset::jbdRead(fr, tb[prStep - 1].reg);
            else k = bset::jbdExit(fr, false);
            if (w) w->writeValue(fr, k, !w->canWriteNoResponse());
            if (++prStep > n + 1) { prStep = -1; prDone = true; }
          } else {
            k = bset::antRead(fr, tb[prStep]);
            if (antCh) antCh->writeValue(fr, k, !antCh->canWriteNoResponse());
            if (++prStep >= n) { prStep = -1; prDone = true; }
          }
          lastOtherPoll = t; // no live-data poll in between
        } else if (fam == 2 && t - lastOtherPoll > 2000) {
          writeRaw(antCh, bms::ANT_STATUS_REQUEST, sizeof(bms::ANT_STATUS_REQUEST), "ANT status");
          lastOtherPoll = t;
        } else if (fam == 3 && t - lastOtherPoll > 1000) {
          jbdToggle ^= 1;
          if (jbdToggle || !jbdHaveBasic)
            writeRaw(jbdCh ? jbdCh : ch, bms::JBD_BASIC_REQUEST, 7, "JBD basic");
          else
            writeRaw(jbdCh ? jbdCh : ch, bms::JBD_CELLS_REQUEST, 7, "JBD cells");
          lastOtherPoll = t;
        } else if (fam == 4 && t - lastOtherPoll > (dalySub == 2 ? 250u : 1000u)) {
          uint8_t fr[16];
          NimBLERemoteCharacteristic *w = dalyCh ? dalyCh : ch;
          if (dalySub == 2) {
            int k = daly::a5Frame(fr, 0x80, daly::A5_POLL[dalyIdx++ % 5]);
            writeRaw(w, fr, k, "DALY A5");
          } else {
            int k = daly::d2Frame(fr, 0xD2, 3, 0, daly::BLE_STATUS_REGS);
            writeRaw(w, fr, k, "DALY status");
          }
          lastOtherPoll = t;
        } else if (fam == 0 && probeStep == -1) {
          // Quick identification: rotate JK / ANT / JBD / DALY requests.
          uint32_t gap = fullProbeDone ? 3000 : 1200;
          if (!identAt || t - identAt > gap) {
            identAt = t;
            auto j7 = jk::request(0x97), j6 = jk::request(0x96);
            uint8_t fr[16];
            uint8_t idx = identIdx++;
            if (ch && !jbdCh && !dalyCh && !peerIsAnt && t - identStartedAt < 60000) {
              // JK first (FFE1 only). New JK modules (ZX-D30, MAC A4:C1:38) may only
              // answer one write type, so alternate it every two requests.
              bool resp = ((idx / 2) & 1) ? ch->canWrite() : !ch->canWriteNoResponse();
              auto j = (idx & 1) ? j6 : j7;
              bool ok = ch->writeValue(j.data(), j.size(), resp);
              Serial.printf("[BLE] -> ident JK 0x%02X %s %s\n", (idx & 1) ? 0x96 : 0x97, resp ? "resp" : "noresp", ok ? "ok" : "FAILED");
              Guard g;
              if (ok) shared.txOk++; else shared.txFail++;
              shared.writeWithResponse = resp;
              if (!ok) shared.bleError = client->getLastError();
              withResp = resp;
              shared.probe = "identifying JK";
            } else
            switch (idx % (dalyCh ? 6 : 4)) {
              case 0: writeRaw(ch, j7.data(), j7.size(), "ident JK 0x97"); break;
              case 1: writeRaw(antCh, bms::ANT_STATUS_REQUEST, 10, "ident ANT"); break;
              case 2: writeRaw(jbdCh ? jbdCh : ch, bms::JBD_BASIC_REQUEST, 7, "ident JBD"); break;
              case 3: writeRaw(ch, j6.data(), j6.size(), "ident JK 0x96"); break;
              case 4: { int k = daly::d2Frame(fr, 0xD2, 3, 0, daly::BLE_STATUS_REGS); writeRaw(dalyCh, fr, k, "ident DALY"); break; }
              default: { int k = daly::a5Frame(fr, 0x80, 0x90); writeRaw(dalyCh, fr, k, "ident DALY A5"); break; }
            }
            if (dalySub == 2 && !fam) { // A5 answered 0x90: finish reading as DALY
              fam = 4;
            }
            {
              Guard g;
              shared.probe = fullProbeDone ? "identifying (slow)" : "identifying";
            }
            if (!fullProbeDone && t - identStartedAt > 20000 && !steps.empty() && !ch) {
              // (FFE0 devices never get the brute-force probe: junk frames can wedge
              //  the command parser of newer JK BMS until the BMS restarts)
              // Nothing recognised: run the full diagnostic probe once.
              Serial.println("[BLE] no known BMS answered, starting full probe");
              fullProbeDone = true;
              probeStep = 0;
              probeAt = 0;
              winLen = 0;
              winChunks = 0;
              jkStep = -1;
              goodBefore = parser.good;
            }
          }
        }
      }
      {
        Guard g;
        shared.bad = parser.bad;
        if (shared.have && uint32_t(millis() - shared.stamp) > 15000)
          shared.state = "STALE";
      }
      if (probeStep >= 0) {
        uint32_t t = millis();
        static const char hx[] = "0123456789ABCDEF";
        if (!probeAt || t - probeAt > 1800) {
          // Close out the previous step: record what came back during it.
          if (probeAt && probeStep > 0) {
            String line;
            size_t got = winLen, ck = winChunks;
            line += String(" <- ") + got + " B / " + ck + " chunks";
            if (got)
              line += String(" from ") +
                      (lastRxFrom ? lastRxFrom->getUUID().toString().c_str() : "?") + ": ";
            for (size_t i = 0; i < got && i < 64; i++) {
              line += hx[winBuf[i] >> 4];
              line += hx[winBuf[i] & 15];
              line += ' ';
            }
            if (got > 64) line += "...";
            if (parser.good > goodBefore) {
              line += " [JK FRAME OK]";
              if (jkStep < 0) jkStep = probeStep - 1;
            }
            line += "\n";
            Serial.print(line);
            Guard g;
            if (!shared.probeLogFrozen && shared.probeLog.length() < 7000) shared.probeLog += line;
          }
          winLen = 0;
          winChunks = 0;
          goodBefore = parser.good;
          if (probeStep >= (int)steps.size()) {
            String r = jkStep >= 0
                ? String("JK_FOUND step ") + (jkStep + 1) + " on " +
                      steps[jkStep].target->getUUID().toString().c_str() +
                      (steps[jkStep].resp ? " resp" : " noresp")
                : String("DONE_NO_JK_FRAME");
            Serial.printf("[BLE] probe finished: %s\n", r.c_str());
            if (jkStep >= 0) {
              fam = 1;
              ch = steps[jkStep].target;
              withResp = steps[jkStep].resp;
              lastCellRequest = 0;
              lastInfo = 0;
            }
            Guard g;
            shared.probe = r;
            if (!shared.probeLogFrozen) {
              shared.probeLog += String("FINISHED: ") + r + "\n";
              shared.probeLogFrozen = true; // keep the first full run
            }
            probeStep = -1; // back to (slow) identification
          } else {
            const Step &st = steps[probeStep];
            bool ok = st.target->writeValue(probes[st.probe].data,
                                            probes[st.probe].len, st.resp);
            String line = String("#") + (probeStep + 1) + " " +
                          probes[st.probe].name + " -> " +
                          st.target->getUUID().toString().c_str() +
                          (st.resp ? " resp " : " noresp ") +
                          (ok ? "ok" : "WRITE_FAILED");
            Serial.print(line);
            {
              Guard g;
              if (ok) shared.txOk++; else shared.txFail++;
              shared.probe = String("probing ") + (probeStep + 1) + "/" + steps.size();
              if (!shared.probeLogFrozen && shared.probeLog.length() < 7000) shared.probeLog += line;
            }
            probeStep++;
            probeAt = t;
          }
        }
      }
      auto send = [&](uint8_t cmd) {
        if (!ch) return false;
        auto b = jk::request(cmd);
        // JK BMS expects write-without-response on FFE1 when it is offered.
        bool ok = ch->writeValue(b.data(), b.size(), withResp);
        Serial.printf("[BLE] -> 0x%02X (%s) %s\n", cmd,
                      withResp ? "with response" : "no response",
                      ok ? "ok" : "FAILED");
        {
          Guard g;
          if (ok) shared.txOk++; else shared.txFail++;
          shared.writeWithResponse = withResp;
        }
        if (!ok) {
          Guard g;
          shared.bleError = client->getLastError();
          shared.state = "BLE_WRITE_FAILED";
        }
        return ok;
      };
      uint32_t now = millis();
      bool probing = fam != 1 || probeStep >= 0 || !ch;
      if (probing) lastInfo = lastInfo ? lastInfo : now;
      // 1) Device info (0x97) right after linking, retried every 5 s until a
      //    0x03 frame arrives (never re-sent while streaming).
      if (!probing && (!lastInfo || (!gotInfo && now - lastInfo > 5000 && cellRequests < 3))) {
        send(0x97);
        lastInfo = now;
      }
      // 2) Cell info (0x96) starts the continuous 0x02 stream. Send it once
      //    device info arrived or 1.5 s after linking (some firmwares never
      //    answer 0x97), and again whenever the stream stops for 10 s.
      bool firstRequest = !probing && !lastCellRequest &&
                          (gotInfo || now - linkedAt > 1500);
      bool streamStalled = !probing && lastCellRequest &&
                           now - (gotCellFrame ? lastCellFrame : lastCellRequest) > 10000 &&
                           now - lastCellRequest > 10000;
      if (streamStalled && !notifyBytes) {
        // Nothing received at all: try the other write type, and re-ask info.
        if (ch->canWrite() && ch->canWriteNoResponse()) withResp = !withResp;
        send(0x97);
        delay(300);
      }
      // Some JK modules (new ZX-D30) send one cell frame per request instead of a
      // stream: ask again every 2 s while no new cell frame came in.
      bool pollDue = !probing && lastCellRequest && !streamStalled &&
                     now - lastCellRequest > 2000 &&
                     (!gotCellFrame || now - lastCellFrame > 2000);
      if (!probing && (firstRequest || streamStalled || wantRead || pollDue)) {
        delay(gotInfo ? 0 : 100);
        send(0x96);
        lastCellRequest = millis();
        cellRequests++;
      }
      {
        Guard g;
        shared.rxBytes = notifyBytes;
        shared.rxChunks = notifyChunks;
        if (lastRxLen) {
          static const char hx[] = "0123456789ABCDEF";
          String h;
          for (size_t i = 0; i < lastRxLen; i++) {
            h += hx[lastRx[i] >> 4];
            h += hx[lastRx[i] & 15];
            h += ' ';
          }
          shared.lastRxHex = h;
          if (lastRxFrom)
            shared.lastRxChar = lastRxFrom->getUUID().toString().c_str();
        }
      }
      static uint32_t lastRssi = 0, lastLog = 0;
      if (now - lastRssi > 5000) {
        lastRssi = now;
        int rssi = client->getRssi();
        Guard g;
        shared.rssi = rssi;
      }
      if (now - lastLog > 10000) {
        lastLog = now;
        Serial.printf("[BLE] rx=%lu B good=%lu crc_err=%lu q_drop=%lu info=%d cells=%d req96=%u\n",
                      (unsigned long)notifyBytes, (unsigned long)parser.good,
                      (unsigned long)parser.bad, (unsigned long)dropped,
                      gotInfo, gotCellFrame, (unsigned)cellRequests);
      }
      // 3) Give up only after 45 s without any cell frame.
      bool reset = false;
      {
        Guard g;
        uint32_t since = gotCellFrame ? lastCellFrame : linkedAt;
        reset = now - since > (notifyBytes || probeStep != -1 || fam == 0 ? 180000u : 45000u);
        if (!gotCellFrame && reset)
          shared.state = gotInfo ? "NO_CELL_DATA" : "NO_RESPONSE";
        if (generation != seen)
          reset = true;
      }
      if (reset) {
        Serial.println("[BLE] no data for 45 s or config changed -> reconnect");
        client->disconnect();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
// Onboard RGB LED. S3 DevKitC-1: WS2812 on GPIO48 (v1.1: GPIO38).
// ESP32-3248S035: discrete RGB on GPIO4/16/17, active low.
#ifndef BATTLAB_LED_PIN
#define BATTLAB_LED_PIN 48
#endif
const uint8_t LED_PALETTE[LED_COLORS][3] = {
    {255, 0, 0},   {255, 80, 0}, {255, 190, 0}, {0, 255, 40},
    {0, 220, 255}, {0, 50, 255}, {170, 0, 255}, {255, 255, 255}};
const char *const LED_COLOR_NAMES[LED_COLORS] = {
    "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "White"};
const char *const LED_MODE_NAMES[LED_MODES] = {"Rainbow", "Solid", "Breathe",
                                               "Status"};
static LedCfg ledCfg;
static bool ledPersistWanted = false;
LedCfg ledGet() {
  Guard g;
  return ledCfg;
}
void ledSet(const LedCfg &n) {
  LedCfg v = n;
  if (v.mode >= LED_MODES) v.mode = 0;
  if (v.color >= LED_COLORS) v.color = 0;
  if (v.level < 1) v.level = 1;
  if (v.level > 4) v.level = 4;
  Guard g;
  ledCfg = v;
  ledPersistWanted = true;
}
static void ledLoad() {
  Preferences p;
  p.begin("battlab-led", true);
  ledCfg.on = p.getBool("on", true);
  ledCfg.mode = p.getUChar("mode", 0);
  ledCfg.color = p.getUChar("color", 3);
  ledCfg.level = p.getUChar("level", 2);
  p.end();
  ledSet(ledCfg);
  ledPersistWanted = false;
}
static void ledPersist() {
  LedCfg v;
  {
    Guard g;
    if (!ledPersistWanted) return;
    ledPersistWanted = false;
    v = ledCfg;
  }
  Preferences p;
  p.begin("battlab-led", false);
  p.putBool("on", v.on);
  p.putUChar("mode", v.mode);
  p.putUChar("color", v.color);
  p.putUChar("level", v.level);
  p.end();
}
static void ledOut(uint8_t r, uint8_t g, uint8_t b) {
#ifdef BATTLAB_CYD35
  ledcWrite(0, 255 - r);
  ledcWrite(1, 255 - g);
  ledcWrite(2, 255 - b);
#else
  neopixelWrite(BATTLAB_LED_PIN, r, g, b);
#endif
}
static void hueToRgb(float hue, float &r, float &g, float &b) {
  float h = hue / 60.f, x = 1.f - fabsf(fmodf(h, 2.f) - 1.f);
  r = g = b = 0;
  switch (int(h) % 6) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
  }
}
void ledTask(void *) {
#ifdef BATTLAB_CYD35
  const uint8_t pins[3] = {4, 16, 17};
  for (int i = 0; i < 3; i++) {
    ledcSetup(i, 5000, 8);
    ledcAttachPin(pins[i], i);
    ledcWrite(i, 255);
  }
  const uint8_t levels[5] = {0, 30, 80, 150, 255};
#else
  const uint8_t levels[5] = {0, 10, 25, 50, 90}; // WS2812 is very bright
#endif
  float hue = 0, phase = 0;
  for (;;) {
    LedCfg c = ledGet();
    float r = 0, g = 0, b = 0, k = 1;
    if (c.on) {
      const uint8_t *pc = LED_PALETTE[c.color];
      switch (c.mode) {
        case 0:
          hueToRgb(hue, r, g, b);
          break;
        case 1:
          r = pc[0] / 255.f; g = pc[1] / 255.f; b = pc[2] / 255.f;
          break;
        case 2:
          r = pc[0] / 255.f; g = pc[1] / 255.f; b = pc[2] / 255.f;
          k = .08f + .92f * (.5f - .5f * cosf(phase));
          break;
        default: {
          // Status: green = live data, amber pulse = linked/waiting,
          // blue pulse = scanning/connecting, red slow pulse = no BMS.
          bool live, linked, scanning;
          {
            Guard gd;
            live = shared.linked && shared.have &&
                   uint32_t(millis() - shared.stamp) < 15000;
            linked = shared.linked;
            scanning = shared.scanning || shared.state == "CONNECTING";
          }
          float pulse = .15f + .85f * (.5f - .5f * cosf(phase));
          if (live) { g = 1; }
          else if (linked) { r = 1; g = .55f; k = pulse; }
          else if (scanning) { b = 1; g = .3f; k = pulse; }
          else { r = 1; k = .15f + .85f * (.5f - .5f * cosf(phase * .5f)); }
        }
      }
    }
    float m = levels[c.on ? c.level : 0] * k;
    ledOut(uint8_t(r * m), uint8_t(g * m), uint8_t(b * m));
    hue += 0.6f; // full colour circle in about 12 s
    if (hue >= 360.f) hue -= 360.f;
    phase += 0.045f; // breathing period about 2.8 s
    if (phase > 12.566f) phase -= 12.566f;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
static void ledJson(JsonDocument &d) {
  LedCfg c = ledGet();
  d["on"] = c.on;
  d["mode"] = c.mode;
  d["color"] = c.color;
  d["level"] = c.level;
  auto modes = d.createNestedArray("modes");
  for (auto n : LED_MODE_NAMES) modes.add(n);
  auto pal = d.createNestedArray("palette");
  for (int i = 0; i < LED_COLORS; i++) {
    char hx[8];
    snprintf(hx, sizeof hx, "#%02X%02X%02X", LED_PALETTE[i][0],
             LED_PALETTE[i][1], LED_PALETTE[i][2]);
    auto o = pal.createNestedObject();
    o["name"] = LED_COLOR_NAMES[i];
    o["hex"] = hx;
  }
}
// ---- BMS / Wi-Fi actions shared by the web API and the on-screen UI ----
// Each uses its own Preferences handle so it is safe from any task.
static bool wifiOn = true; // the BATTLAB hotspot (setup / local web page)

// ---- Home Wi-Fi + cloud upload ------------------------------------------
// The board joins the home network as a station (credentials in NVS) while
// optionally keeping the BATTLAB hotspot. A cloud task posts telemetry to the
// Supabase ingest() function over HTTPS every 10 s.
static String staSsid, staPass, devToken, pairCode;
static String cloudStatus = "off";
static uint32_t cloudOkAt = 0, cloudSent = 0;
static bool cloudPaired = false;

static void netLoad() {
  Preferences p;
  p.begin("battlab-net", false);
  staSsid = p.getString("ssid", "");
  staPass = p.getString("pass", "");
  devToken = p.getString("token", "");
  pairCode = p.getString("pair", "");
  if (devToken.length() != 64) { // first boot: create this device's identity
    char hx[65];
    for (int i = 0; i < 32; i++) snprintf(hx + i * 2, 3, "%02x", uint8_t(esp_random()));
    devToken = hx;
    p.putString("token", devToken);
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
static void wifiApply() {
  wifi_mode_t m = WIFI_OFF;
  if (wifiOn && staSsid.length()) m = WIFI_AP_STA;
  else if (wifiOn) m = WIFI_AP;
  else if (staSsid.length()) m = WIFI_STA;
  WiFi.persistent(false);
  WiFi.mode(m);
  if (m == WIFI_AP || m == WIFI_AP_STA) {
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(devName.c_str());
  }
  if (m == WIFI_STA || m == WIFI_AP_STA) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  applyWifiTxPower();
}
static void wifiStartAp() { wifiApply(); }
bool wifiEnabled() { return wifiOn; }
void wifiSetEnabled(bool on) {
  if (on == wifiOn) return;
  wifiOn = on;
  Preferences p;
  p.begin("battlab-jk", false);
  p.putBool("wifi", on);
  p.end();
  wifiApply();
}
void netSetHomeWifi(const String &ssid, const String &pass) {
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
void netForgetHomeWifi() { netSetHomeWifi("", ""); }
// A scan fails while the station keeps retrying a network that is not there
// (ESP_ERR_WIFI_STATE -> "no networks"). Pause the retries, scan, resume.
static bool scanResume = false;
static uint8_t scanTries = 0;
void netScanStart() {
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
// JSON array of nearby networks once a scan finished, "" while scanning.
String netScanJson() {
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
NetInfo netInfo() {
  NetInfo n;
  n.ssid = staSsid;
  n.connected = WiFi.status() == WL_CONNECTED;
  if (n.connected) {
    n.ip = WiFi.localIP().toString();
    n.rssi = WiFi.RSSI();
  }
  n.pair = pairCode;
  n.name = devName;
  n.hw = hwMac;
  n.site = CLOUD_SITE;
  n.configured = strlen(CLOUD_URL) > 0;
  Guard g;
  n.cloud = cloudStatus;
  n.paired = cloudPaired;
  n.lastOkAgo = cloudOkAt ? (millis() - cloudOkAt) / 1000 : -1;
  n.sent = cloudSent;
  return n;
}

// ---- Remote control from the BattLab Cloud website ---------------------
// The website queues commands in the database; ingest2() hands them to the
// board in the reply to its regular upload, and the board reports results
// (acks) with the next upload. While someone has the control page open the
// server sets "fast" and the board uploads every few seconds.
struct CloudAck { long id; bool ok; String msg; };
static std::vector<CloudAck> cloudAcks;
static bool wscanWanted = false, restartAfterSend = false;
static bool apChangeWanted = false, apChangeTo = true;
static String wifiNewSsid, wifiNewPass, wifiOldSsid, wifiOldPass, cloudNote;
static bool wifiChangeWanted = false;
static String otaUrl;
static bool otaWanted = false;
static uint32_t wifiRevertAt = 0;

// JK passcodes for the owner (local page / cloud command). pack < 0 = the BLE BMS.
static String jkCodesJson(int pack) {
  StaticJsonDocument<256> d;
  Guard g;
  String bt = shared.codeBt, st = shared.codeSet;
#ifndef BATTLAB_DISPLAY
  if (pack >= 0 && pack < 16) { bt = shared.packs[pack].codeBt; st = shared.packs[pack].codeSet; }
#endif
  d["ok"] = bt.length() > 0 || st.length() > 0;
  d["bt"] = bt;
  d["set"] = st;
  if (pack >= 0) d["a"] = pack;
  String out;
  serializeJson(d, out);
  return out;
}
static String runCloudCmd(const String &c, JsonObjectConst a, bool &ok) {
  ok = true;
  if (c == "ping") return "pong";
  if (c == "jk_codes") { String r = jkCodesJson(a["a"] | -1); ok = r.indexOf("\"ok\":true") >= 0; return r; }
  if (c == "led") {
    LedCfg l = ledGet();
    if (a.containsKey("on")) l.on = a["on"].as<bool>();
    if (a.containsKey("mode")) { int m = a["mode"]; if (m < 0 || m >= LED_MODES) { ok = false; return "bad mode"; } l.mode = m; }
    if (a.containsKey("color")) { int k = a["color"]; if (k < 0 || k >= LED_COLORS) { ok = false; return "bad color"; } l.color = k; }
    if (a.containsKey("level")) { int v = a["level"]; if (v < 1 || v > 4) { ok = false; return "bad level"; } l.level = v; }
    ledSet(l);
    return "ok";
  }
  if (c == "bms_scan") { bmsScan(); return "scanning"; }
  if (c == "bms_auto") { bmsAuto(); return "auto connect"; }
  if (c == "bms_disconnect") { bmsDisconnect(); return "disconnected"; }
  if (c == "bms_connect") {
    String mac = a["mac"] | "", p = a["protocol"] | "AUTO";
    int t = a["type"] | 0;
    if (t < 0 || t > 1 || !bmsConnect(mac, uint8_t(t), p)) { ok = false; return "invalid MAC or protocol"; }
    return "connecting";
  }
  if (c == "bms_param") {
    if (!a["index"].is<int>() || !(a["value"].is<float>() || a["value"].is<int>())) { ok = false; return "index and value required"; }
    if (const char *e = bmsWriteParam(a["index"].as<int>(), a["value"].as<float>(), a["a"] | -1)) { ok = false; return e; }
    return "sent to BMS";
  }
  if (c == "bms_switch") {
    if (!a["which"].is<int>() || !a["on"].is<bool>()) { ok = false; return "which and on required"; }
    if (const char *e = bmsSetSwitch(a["which"].as<int>(), a["on"].as<bool>(), a["a"] | -1)) { ok = false; return e; }
    return "sent to BMS";
  }
  if (c == "bms_read") { Guard g; readWanted = true; return "reading"; }
  if (c == "radio_max") { radioSetMaxPower(a["on"] | true); return radioMax ? "TX max" : "TX low"; }
  if (c == "wifi_scan") { netScanStart(); wscanWanted = true; return "scanning Wi-Fi"; }
  if (c == "wifi_ap") { apChangeWanted = true; apChangeTo = a["on"] | true; return apChangeTo ? "hotspot on" : "hotspot off"; }
  if (c == "home_wifi") {
    String ss = a["ssid"] | "", pw = a["pass"] | "";
    if (!ss.length() || ss.length() > 32 || pw.length() > 64) { ok = false; return "SSID 1-32, password up to 64"; }
    wifiNewSsid = ss; wifiNewPass = pw; wifiChangeWanted = true;
    return "switching Wi-Fi (goes back to the old one if it fails)";
  }
  if (c == "display") {
    if (const char *e = displayCfgSet(a)) { ok = false; return e; }
    return "ok";
  }
  if (c == "restart") { restartAfterSend = true; return "restarting"; }
  if (c == "link") {
#ifdef BATTLAB_DISPLAY
    ok = false;
    return "RS485 is available on the ESP32-S3 board";
#else
    String mode = a["mode"] | "ble";
    int rx = a["rx"] | int(linkCfg.rx), tx = a["tx"] | int(linkCfg.tx), addr = a["addr"] | int(linkCfg.addr);
    uint32_t baud = a["baud"] | linkCfg.baud;
    auto badPin = [](int p) { return p < 0 || p > 48 || p == 19 || p == 20 || (p >= 26 && p <= 37); };
    if (mode != "ble" && mode != "rs485") { ok = false; return "mode ble or rs485"; }
    if (badPin(rx) || badPin(tx) || rx == tx) { ok = false; return "pick two free GPIO pins (not 19/20, not 26-37)"; }
    if (baud != 9600 && baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200) { ok = false; return "baud 9600-115200"; }
    if (addr < 0 || addr > 15) { ok = false; return "address 0-15"; }
    String bus = a["bus"] | (linkCfg.bus == 1 ? "listen" : linkCfg.bus == 2 ? "poll" : "auto");
    int count = a["count"] | int(linkCfg.count);
    if (bus != "auto" && bus != "listen" && bus != "poll") { ok = false; return "bus auto, listen or poll"; }
    if (count < 1 || count > 15 || addr + count > 16) { ok = false; return "1-15 packs, last address 15"; }
    String brand = a["brand"] | (linkCfg.brand == 1 ? "jk" : linkCfg.brand == 2 ? "daly" : "auto");
    if (brand != "auto" && brand != "jk" && brand != "daly") { ok = false; return "brand auto, jk or daly"; }
    linkCfg.brand = brand == "jk" ? 1 : brand == "daly" ? 2 : 0;
    linkCfg.rs485 = mode == "rs485";
    linkCfg.rx = rx; linkCfg.tx = tx; linkCfg.baud = baud; linkCfg.addr = addr;
    linkCfg.bus = bus == "listen" ? 1 : bus == "poll" ? 2 : 0;
    linkCfg.count = count;
    Preferences p;
    p.begin("battlab-jk", false);
    p.putBool("l485", linkCfg.rs485);
    p.putChar("lrx", linkCfg.rx);
    p.putChar("ltx", linkCfg.tx);
    p.putUInt("lbaud", linkCfg.baud);
    p.putUChar("laddr", linkCfg.addr);
    p.putUChar("lbus", linkCfg.bus);
    p.putUChar("lcnt", linkCfg.count);
    p.putUChar("lbrand", linkCfg.brand);
    p.end();
    {
      Guard g;
      shared.linked = shared.have = false;
      shared.family = 0;
      shared.state = linkCfg.rs485 ? "RS485_STARTING" : "RECONFIGURING";
      generation++;
      configChanged = true;
    }
    linkRs485 = linkCfg.rs485;
    return linkCfg.rs485 ? String("RS485: RX ") + rx + " TX " + tx + " " + baud + " addr " + addr + " x" + count + " " + bus : String("Bluetooth");
#endif
  }
  if (c == "profile") {
    String t = a["type"] | "solar";
    int k = -1;
    for (int i = 0; i < 5; i++) if (t == PROF_NAME[i]) k = i;
    int wh = a["whkm"] | int(profCfg.whkm), ra = a["range_alarm"] | int(profCfg.rangeAlarm);
    if (k < 0 || wh < 0 || wh > 2000 || ra < 0 || ra > 200) { ok = false; return "type solar|ev|car|ups|camp, whkm 0-2000"; }
    profCfg.type = k; profCfg.whkm = wh; profCfg.rangeAlarm = ra;
    Preferences p;
    p.begin("battlab-jk", false);
    p.putUChar("ptype", profCfg.type);
    p.putUShort("pwhkm", profCfg.whkm);
    p.putUChar("prange", profCfg.rangeAlarm);
    p.end();
    return String("profile ") + t;
  }
  if (c == "bt_match" || c == "bt_clear") {
#ifdef BATTLAB_DISPLAY
    ok = false;
    return "needs the ESP32-S3 RS485 board";
#else
    if (!linkRs485) { ok = false; return "RS485 bank only"; }
    if (c == "bt_match") { btMatchWanted = true; Guard g; btmStatus = "queued"; return "matching packs over Bluetooth (~1 min)"; }
    btClearWanted = true;
    return "cleared";
#endif
  }
  if (c == "ota") {
    String url = a["url"] | "";
    if (!url.startsWith(String("https://") + CLOUD_SITE + "/fw/") || !url.endsWith(".bin") || url.length() > 200) {
      ok = false;
      return "update URL not allowed";
    }
    otaUrl = url;
    otaWanted = true;
    return "update starting - board restarts when done";
  }
  ok = false;
  return "unknown command";
}

// Download a new app image from the BattLab Cloud site into the other OTA
// slot, then restart into it. Settings (NVS) are kept.
static void runOta(WiFiClientSecure &tls) {
  otaWanted = false;
  otaBusy = true;
  otaPct = 0;
  { Guard g; cloudStatus = "updating firmware..."; }
  Serial.printf("[OTA] %s\n", otaUrl.c_str());
  vTaskDelay(pdMS_TO_TICKS(1500)); // Bluetooth link closes, RAM comes back
  tls.stop();
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    if (total > 0) otaPct = int(int64_t(cur) * 100 / total);
  });
  gfxHold(5000); // the screen draws only the progress bar meanwhile
  t_httpUpdate_return r = httpUpdate.update(tls, otaUrl);
  gfxRelease();
  if (r == HTTP_UPDATE_OK) {
    otaPct = 100;
    Serial.println("[OTA] done, restarting");
    delay(800);
    ESP.restart();
  }
  cloudNote = "update failed: " + httpUpdate.getLastErrorString();
  Serial.printf("[OTA] %s\n", cloudNote.c_str());
  { Guard g; cloudStatus = cloudNote; }
  otaPct = -1;
  otaBusy = false;
}

static void cloudTask(void *) {
  WiFiClientSecure tls;
  // Full Mozilla root store, looked up by issuer: works whichever CA the
  // server uses and only the matching root is parsed (little RAM).
  tls.setCACertBundle(CLOUD_CA_BUNDLE);
  tls.setTimeout(10);
  HTTPClient http;
#ifdef BATTLAB_DISPLAY
  // CYD: a new TLS session per upload, so its ~40 KB go back to the heap
  // between uploads (the screen needs them).
  http.setReuse(false);
  const uint32_t FAST_MS = 5000;
  const size_t DOC = 6144;
#else
  http.setReuse(true); // S3 has RAM to keep the connection open
  const uint32_t FAST_MS = 2500;
  size_t DOC = 10240; // grows with the RS485 bank (see below)
#endif
  uint32_t nextAt = 0;
  bool fast = false;
  auto setStatus = [](const String &s) { Guard g; cloudStatus = s; };
  auto ram = []() {
    return String((unsigned)(ESP.getFreeHeap() / 1024)) + "/" +
           (unsigned)(ESP.getMaxAllocHeap() / 1024) + "k";
  };
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    // a home Wi-Fi change made from the website: keep it only if it connects
    if (wifiRevertAt) {
      if (WiFi.status() == WL_CONNECTED) {
        wifiRevertAt = 0;
        cloudNote = String("Wi-Fi changed to ") + staSsid;
      } else if (int32_t(millis() - wifiRevertAt) > 0) {
        wifiRevertAt = 0;
        cloudNote = String("could not join ") + staSsid + ", back to " + wifiOldSsid;
        netSetHomeWifi(wifiOldSsid, wifiOldPass);
      }
    }
    if (!strlen(CLOUD_URL)) { setStatus("not set up"); continue; }
    if (!staSsid.length()) { setStatus("no home Wi-Fi"); continue; }
    if (WiFi.status() != WL_CONNECTED) { setStatus("Wi-Fi connecting"); continue; }
    if (otaWanted && cloudAcks.empty()) { runOta(tls); nextAt = 0; continue; }
    if (nextAt && int32_t(millis() - nextAt) < 0) continue;
    bool live, paired;
    {
      Guard g;
      live = shared.linked && shared.have && uint32_t(millis() - shared.stamp) < 15000;
      paired = cloudPaired;
    }
    uint32_t every = fast && paired ? FAST_MS : live ? 10000 : (paired ? 60000 : 20000);
    String json;
    bool sentWscan = false;
    {
#ifndef BATTLAB_DISPLAY
      { Guard g; DOC = 10240 + size_t(shared.bankN) * 1700; }
#endif
      DynamicJsonDocument body(DOC);
      body["p_token"] = devToken;
      body["p_pair"] = pairCode;
      auto acks = body.createNestedArray("p_acks");
      for (auto &k : cloudAcks) {
        auto o = acks.createNestedObject();
        o["id"] = k.id;
        o["ok"] = k.ok;
        o["msg"] = k.msg;
      }
      auto d = body.createNestedObject("p_data");
      LedCfg led = ledGet();
      String devs, wscan;
      if (wscanWanted) {
        wscan = netScanJson();
        if (wscan.length()) { d["wscan"] = serialized(wscan); sentWscan = true; }
      }
      auto c = d.createNestedObject("cfg");
#ifdef BATTLAB_DISPLAY
      c["board"] = "cyd35";
      displayCfgGet(c.createNestedObject("disp"));
#else
      c["board"] = "s3";
#endif
      c["remote"] = 1;
      c["ota"] = 1;
#ifndef BATTLAB_DISPLAY
      c["link"] = linkCfg.rs485 ? "rs485" : "ble";
      c["lrx"] = linkCfg.rx;
      c["ltx"] = linkCfg.tx;
      c["lbaud"] = linkCfg.baud;
      c["laddr"] = linkCfg.addr;
      c["lbus"] = linkCfg.bus == 1 ? "listen" : linkCfg.bus == 2 ? "poll" : "auto";
      c["lcnt"] = linkCfg.count;
      c["lbrand"] = linkCfg.brand == 1 ? "jk" : linkCfg.brand == 2 ? "daly" : "auto";
      if (linkRs485) {
        Guard g;
        auto bt = c.createNestedObject("bt");
        for (int a = 0; a < 16; a++)
          if (btMap[a].mac[0]) { auto o = bt.createNestedObject(String(a)); o["mac"] = btMap[a].mac; o["name"] = btMap[a].name; }
        if (btmStatus.length()) c["btm"] = btmStatus;
        auto pw = c.createNestedArray("pw");
        uint32_t now = millis();
        for (auto &e : pwLog)
          if (e.id && now - e.at < 600000) {
            auto o = pw.createNestedObject();
            o["a"] = e.a; o["what"] = e.what;
            static const char *const ST[5] = {"bt", "sent", "ok", "nochg", "nobt"};
            o["st"] = ST[e.st > 4 ? 4 : e.st];
            o["age"] = (now - e.at) / 1000;
          }
      }
#endif
      c["prof"] = PROF_NAME[profCfg.type];
      c["name"] = devName;
      c["hwid"] = hwMac;
      c["whkm"] = profCfg.whkm;
      c["bleota"] = 1;
      auto lo = c.createNestedObject("led");
      lo["on"] = led.on;
      lo["mode"] = led.mode;
      lo["color"] = led.color;
      lo["level"] = led.level;
      c["ap"] = wifiOn;
      c["tx"] = radioMax;
      c["ssid"] = staSsid;
      c["ip"] = WiFi.localIP().toString();
      c["wrssi"] = WiFi.RSSI();
      if (cloudNote.length()) c["note"] = cloudNote;
      Guard g;
      d["fw"] = BATTLAB_FW;
      d["state"] = shared.state;
      d["bms"] = shared.detected;
      d["model"] = shared.model;
      d["rssi"] = shared.rssi;
      d["up"] = millis() / 1000;
      d["heap"] = ESP.getFreeHeap();
      d["blk"] = ESP.getMaxAllocHeap();
      if (shared.state != "LIVE") {
        // BLE diagnostics so a stuck connection can be read from the cloud
        auto bd = d.createNestedObject("bd");
        bd["gatt"] = shared.gatt.substring(0, 400);
        bd["probe"] = shared.probe;
        bd["rx"] = shared.rxBytes;
        bd["rxc"] = shared.rxChunks;
        bd["hex"] = shared.lastRxHex.substring(0, 200);
        bd["from"] = shared.lastRxChar;
        bd["err"] = shared.bleError;
        bd["tok"] = shared.txOk;
        bd["tfail"] = shared.txFail;
        bd["good"] = shared.good;
        bd["bad"] = shared.bad;
        bd["rej"] = shared.rejected;
        bd["wr"] = shared.writeWithResponse;
        bd["try"] = shared.retries;
        if (shared.probeLog.length()) bd["log"] = shared.probeLog.substring(0, 1500);
        auto ft = bd.createNestedArray("ft");
        for (int i = 0; i < 4; i++) ft.add(shared.ftype[i]);
        if (shared.f2hex.length()) bd["f2"] = shared.f2hex;
        bd["links"] = shared.links;
        bd["linkms"] = shared.lastLinkMs;
        bd["mtu"] = bleMtu;
        bd["sub"] = shared.subLog;
        bd["peer"] = shared.peerName;
      }
      c["mac"] = cfg.mac;
      c["proto"] = cfg.protocol;
      c["auto"] = cfg.automatic;
      c["atype"] = cfg.type;
      c["fam"] = shared.family;
      c["scanning"] = shared.scanning;
      c["lw"] = shared.lastWrite;
      if (shared.devStamp && uint32_t(millis() - shared.devStamp) < 120000) {
        devs = shared.devices;
        d["devs"] = serialized(devs);
      }
      if (shared.linked && shared.pvHave && (shared.family == 2 || shared.family == 3)) {
        int n;
        bset::table(shared.family, n);
        auto po = d.createNestedObject("pa");
        po["f"] = shared.family;
        auto v = po.createNestedArray("v");
        for (int i = 0; i < n; i++)
          if (shared.pvHave >> i & 1) v.add(serialized(String(shared.pv[i], 3)));
          else v.add(nullptr);
      }
      if (shared.linked && shared.jk.valid && shared.family == 1) {
        auto jo = d.createNestedObject("jk");
        auto v = jo.createNestedArray("v");
        for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) v.add(shared.jk.v[i]);
        auto sw = jo.createNestedArray("sw");
        for (int i = 0; i < bmsw::SW_COUNT; i++) sw.add(shared.jk.sw[i]);
      }
      {
        float ei, eo;
        energyGet(ei, eo);
        d["ein"] = serialized(String(ei, 3));
        d["eout"] = serialized(String(eo, 3));
      }
      if (live) {
        const auto &s = shared.sample;
        d["v"] = s.voltage;
        d["i"] = s.current;
        d["p"] = s.voltage * s.current;
        d["soc"] = s.soc;
        d["cap"] = s.capacity;
        if (s.full > 0) d["full"] = s.full;
        d["cyc"] = s.cycles;
        d["t1"] = s.temp1;
        d["t2"] = s.temp2;
        d["mos"] = s.mos;
        d["chg"] = s.charge;
        d["dsg"] = s.discharge;
        float mn = 9, mx = 0;
#ifndef BATTLAB_DISPLAY
        bool bank = linkRs485 && (shared.bankN > 1 || shared.busMode == 1);
#else
        bool bank = false;
#endif
        if (!bank) {
          auto cl = d.createNestedArray("cells");
          for (int k = 0; k < 32; k++)
            if (s.cells[k] > 0) {
              cl.add(roundf(s.cells[k] * 1000) / 1000);
              mn = min(mn, s.cells[k]);
              mx = max(mx, s.cells[k]);
            }
        }
#ifndef BATTLAB_DISPLAY
        else {
          // RS485 bank: totals on top, every pack in "packs"
          d["bank"] = shared.bankN;
          d["busm"] = shared.busMode;
          auto pa = d.createNestedArray("packs");
          uint32_t now = millis();
          for (int a = 0; a < 16; a++) {
            const auto &pk = shared.packs[a];
            if (!pk.stamp || now - pk.stamp > 600000) continue;
            const auto &x = pk.s;
            auto o = pa.createNestedObject();
            o["a"] = a;
            o["age"] = (now - pk.stamp) / 1000;
            o["v"] = serialized(String(x.voltage, 2));
            o["i"] = serialized(String(x.current, 2));
            o["soc"] = x.soc;
            o["cap"] = serialized(String(x.capacity, 1));
            if (x.full > 0) o["full"] = serialized(String(x.full, 1));
            o["cyc"] = x.cycles;
            o["t1"] = serialized(String(x.temp1, 1));
            o["t2"] = serialized(String(x.temp2, 1));
            o["mos"] = serialized(String(x.mos, 1));
            o["chg"] = x.charge;
            o["dsg"] = x.discharge;
            if (pk.model.length()) o["model"] = pk.model;
            if (pk.serial[0]) o["sn"] = pk.serial;
            if (btMap[a].mac[0]) o["bt"] = 1;
            float pmn = 9, pmx = 0;
            auto cl = o.createNestedArray("cells");
            for (int k = 0; k < 32; k++)
              if (x.cells[k] > 0) {
                cl.add(serialized(String(x.cells[k], 3)));
                pmn = min(pmn, x.cells[k]); pmx = max(pmx, x.cells[k]);
              }
            if (pmx > 0) {
              o["cmin"] = serialized(String(pmn, 3)); o["cmax"] = serialized(String(pmx, 3));
              mn = min(mn, pmn); mx = max(mx, pmx);
            }
            if (pk.js.valid && now - pk.setStamp < 600000) {
              auto jo = o.createNestedObject("jk");
              auto v = jo.createNestedArray("v");
              for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) v.add(serialized(String(pk.js.v[i], 3)));
              auto sw = jo.createNestedArray("sw");
              for (int i = 0; i < bmsw::SW_COUNT; i++) sw.add(pk.js.sw[i]);
            }
          }
        }
#endif
        if (mx > 0) { d["cmin"] = mn; d["cmax"] = mx; }
      }
      serializeJson(body, json);
    }
    // Screen pauses its sprite drawing while the TLS session is open.
    if (!gfxHold(3000)) { nextAt = millis() + 2000; continue; }
    if (ESP.getMaxAllocHeap() < 20000) {
      gfxRelease();
      setStatus(String("low RAM ") + ram() + " - retry");
      Serial.printf("[CLOUD] low RAM free %u block %u\n", (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getMaxAllocHeap());
      nextAt = millis() + 15000;
      continue;
    }
    if (!fast) setStatus(String("sending (RAM ") + ram() + ")");
    int code = -1;
    String resp;
    if (http.begin(tls, String(CLOUD_URL) + "/rest/v1/rpc/ingest2")) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", CLOUD_ANON_KEY);
      http.addHeader("Authorization", String("Bearer ") + CLOUD_ANON_KEY);
      code = http.POST(json);
      if (code > 0) resp = http.getString();
      http.end();
    }
    json = String();
    char why[96] = {0};
    int e = code < 0 ? tls.lastError(why, sizeof why) : 0;
#ifdef BATTLAB_DISPLAY
    tls.stop(); // free the TLS buffers before the screen draws again
#else
    if (code < 0) tls.stop();
#endif
    gfxRelease();
    if (code != 200) {
      Guard g;
      if (code < 0) {
        cloudStatus = e ? String("TLS: ") + why : String("no connection (") + code + ")";
        cloudStatus += String("  RAM ") + ram();
        Serial.printf("[CLOUD] POST failed %d, tls %d %s, heap %u block %u\n", code, e, why,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      } else {
        cloudStatus = String("server error ") + code;
        Serial.printf("[CLOUD] %d %s\n", code, resp.c_str());
      }
      nextAt = millis() + (fast ? 5000 : 15000);
      continue;
    }
    // success: the acks we sent are delivered
    cloudAcks.clear();
    cloudNote = "";
    if (sentWscan) wscanWanted = false;
    DynamicJsonDocument r(4096);
    bool gotCmds = false;
    if (!deserializeJson(r, resp)) {
      String st = r["status"] | "ok";
      fast = r["fast"] | false;
      {
        Guard g;
        cloudSent++;
        cloudOkAt = millis();
        cloudPaired = st != "unpaired";
        cloudStatus = !cloudPaired ? "online - add on website" : fast ? "online - remote control" : "online";
      }
      for (JsonObjectConst cmd : r["cmds"].as<JsonArrayConst>()) {
        bool ok;
        String m = runCloudCmd(String((const char *)(cmd["cmd"] | "")), cmd["args"].as<JsonObjectConst>(), ok);
        Serial.printf("[CLOUD] cmd %ld %s -> %s\n", (long)(cmd["id"] | 0L), (const char *)(cmd["cmd"] | ""), m.c_str());
        cloudAcks.push_back({(long)(cmd["id"] | 0L), ok, m});
        gotCmds = true;
      }
    }
    nextAt = millis() + (gotCmds ? 700 : every);
    // actions that cut the connection run after their ack is queued; the
    // ack goes out with the next upload once the network is back
    if (gotCmds) continue;
    if (restartAfterSend) {
      delay(300);
      ESP.restart();
    }
    if (apChangeWanted) {
      apChangeWanted = false;
      wifiSetEnabled(apChangeTo);
    }
    if (wifiChangeWanted) {
      wifiChangeWanted = false;
      wifiOldSsid = staSsid;
      wifiOldPass = staPass;
      netSetHomeWifi(wifiNewSsid, wifiNewPass);
      wifiRevertAt = millis() + 90000;
    }
  }
}

// ---- Bluetooth setup service -------------------------------------------
// A GATT service so the BattLab Cloud website (Web Bluetooth, Chrome on
// Android / PC) can set up and control this board without joining its
// hotspot. Values are JSON (max 512 bytes each), refreshed by loop().
#define BSV_UUID "7a1e0000-5b3c-4f6a-9d2e-b47710ba7700"
static const char *const B_CHR_UUID[9] = {
    "7a1e0001-5b3c-4f6a-9d2e-b47710ba7700",  // 0 live data (read)
    "7a1e0002-5b3c-4f6a-9d2e-b47710ba7700",  // 1 settings (read)
    "7a1e0003-5b3c-4f6a-9d2e-b47710ba7700",  // 2 JK parameters (read)
    "7a1e0004-5b3c-4f6a-9d2e-b47710ba7700",  // 3 BMS scan list (read)
    "7a1e0005-5b3c-4f6a-9d2e-b47710ba7700",  // 4 Wi-Fi scan list (read)
    "7a1e0006-5b3c-4f6a-9d2e-b47710ba7700",  // 5 command (write)
    "7a1e0007-5b3c-4f6a-9d2e-b47710ba7700",  // 6 result (read, notify)
    "7a1e0008-5b3c-4f6a-9d2e-b47710ba7700",  // 7 firmware update control (write, notify)
    "7a1e0009-5b3c-4f6a-9d2e-b47710ba7700"}; // 8 firmware update data (write)
static NimBLECharacteristic *bChr[9] = {};
// ---- firmware update over Bluetooth ----
// Website: {"begin":<size>,"sha":"<sha256 hex>"} on 7, then the .bin in chunks on 8
// (write with response = flow control), then {"end":1}. The image is checked
// against the SHA-256 before the board switches to it.
static mbedtls_sha256_context bOtaSha;
static size_t bOtaSize = 0, bOtaGot = 0;
static uint8_t bOtaWant[32];
static uint32_t bOtaLast = 0, bOtaRestartAt = 0, bOtaNext = 0;
static void bOtaNotify(const String &m) {
  if (!bChr[7]) return;
  bChr[7]->setValue((const uint8_t *)m.c_str(), m.length());
  bChr[7]->notify();
}
static void bOtaFail(const String &why) {
  if (bleOtaBusy) Update.abort();
  bleOtaBusy = false;
  otaPct = -1;
  bOtaNotify(String("{\"st\":\"error\",\"msg\":\"") + why + "\"}");
}
class BleOtaCtl : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    NimBLEAttValue v = c->getValue();
    StaticJsonDocument<256> in;
    if (deserializeJson(in, (const char *)v.data(), v.length())) return bOtaFail("bad JSON");
    if (in.containsKey("begin")) {
      size_t sz = in["begin"] | 0;
      String sha = in["sha"] | "";
      const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
      if (bleOtaBusy) Update.abort();
      if (!next || !sz || sz > next->size) return bOtaFail("image too big for this board");
      if (sha.length() != 64) return bOtaFail("sha256 missing");
      for (int i = 0; i < 32; i++) bOtaWant[i] = strtoul(sha.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
      if (!Update.begin(sz)) return bOtaFail("cannot start update");
      mbedtls_sha256_init(&bOtaSha);
      mbedtls_sha256_starts(&bOtaSha, 0);
      bOtaSize = sz; bOtaGot = 0; bOtaNext = 32768;
      bOtaLast = millis();
      bleOtaBusy = true;
      otaPct = 0;
      bOtaNotify("{\"st\":\"ready\"}");
    } else if (in.containsKey("end")) {
      if (!bleOtaBusy) return bOtaFail("no update running");
      if (bOtaGot != bOtaSize) return bOtaFail("incomplete image");
      uint8_t got[32];
      mbedtls_sha256_finish(&bOtaSha, got);
      mbedtls_sha256_free(&bOtaSha);
      if (memcmp(got, bOtaWant, 32)) return bOtaFail("sha256 mismatch");
      if (!Update.end(true)) return bOtaFail(String("verify failed ") + Update.getError());
      bleOtaBusy = false;
      otaPct = 100;
      bOtaNotify("{\"st\":\"done\"}");
      bOtaRestartAt = millis() + 1500;
    } else if (in.containsKey("abort")) bOtaFail("aborted");
  }
};
class BleOtaData : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    if (!bleOtaBusy) return;
    NimBLEAttValue v = c->getValue();
    size_t n = v.length();
    if (bOtaGot + n > bOtaSize) return bOtaFail("too much data");
    if (Update.write((uint8_t *)v.data(), n) != n) return bOtaFail(String("flash write ") + Update.getError());
    mbedtls_sha256_update(&bOtaSha, v.data(), n);
    bOtaGot += n;
    bOtaLast = millis();
    otaPct = int(uint64_t(bOtaGot) * 100 / bOtaSize);
    if (bOtaGot >= bOtaNext) { bOtaNext += 32768; bOtaNotify(String("{\"st\":\"rx\",\"got\":") + bOtaGot + "}"); }
  }
};
static SemaphoreHandle_t bleQmx = nullptr;
static std::vector<std::string> bleQ;
// BMS / Wi-Fi scan lists over the Bluetooth / USB setup link: one value holds at
// most 512 bytes, so the list is split into pages; every read returns the next page
// {"p":page,"pp":pages,"t":total,"d":[[...],...]} and the website reads until it has all.
static SemaphoreHandle_t listMx = nullptr;
static std::vector<String> listPages[2];
static uint8_t listCur[2] = {0, 0};
static String listNext(int k) {
  String v = "{\"p\":0,\"pp\":1,\"t\":0,\"d\":[]}";
  if (!listMx) return v;
  xSemaphoreTake(listMx, portMAX_DELAY);
  if (!listPages[k].empty()) {
    if (listCur[k] >= listPages[k].size()) listCur[k] = 0;
    v = listPages[k][listCur[k]];
    listCur[k] = (listCur[k] + 1) % listPages[k].size();
  }
  xSemaphoreGive(listMx);
  return v;
}
static void listSet(int k, const std::vector<String> &items) { // items = compact JSON arrays
  std::vector<String> pages;
  String cur;
  std::vector<String> chunks;
  for (auto &it : items) {
    if (cur.length() && cur.length() + it.length() + 1 > 440) { chunks.push_back(cur); cur = ""; }
    cur += (cur.length() ? "," : "") + it;
  }
  if (cur.length() || chunks.empty()) chunks.push_back(cur);
  for (size_t i = 0; i < chunks.size(); i++)
    pages.push_back(String("{\"p\":") + i + ",\"pp\":" + chunks.size() + ",\"t\":" + items.size() + ",\"d\":[" + chunks[i] + "]}");
  if (!listMx) return;
  xSemaphoreTake(listMx, portMAX_DELAY);
  listPages[k] = pages;
  if (listCur[k] >= pages.size()) listCur[k] = 0;
  xSemaphoreGive(listMx);
}
class BleListCb : public NimBLECharacteristicCallbacks {
  int k;
public:
  explicit BleListCb(int kk) : k(kk) {}
  void onRead(NimBLECharacteristic *c) override { String v = listNext(k); c->setValue((const uint8_t *)v.c_str(), v.length()); }
};
static volatile bool bleClient = false;
class BleSrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override { bleClient = true; }
  void onDisconnect(NimBLEServer *s) override { bleClient = s->getConnectedCount() > 0; }
};
class BleCmdCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    NimBLEAttValue v = c->getValue();
    std::string m((const char *)v.data(), v.length());
    if (!bleQmx || m.size() > 512) return;
    xSemaphoreTake(bleQmx, portMAX_DELAY);
    if (bleQ.size() < 8) bleQ.push_back(m);
    xSemaphoreGive(bleQmx);
  }
};
void bleSetupStart() {
  bleQmx = xSemaphoreCreateMutex();
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new BleSrvCb());
  NimBLEService *svc = srv->createService(BSV_UUID);
  for (int i = 0; i < 9; i++) {
    uint32_t props = i == 5 || i == 8 ? (NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
                   : i == 7 ? (NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ)
                   : i == 6 ? (NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY)
                            : NIMBLE_PROPERTY::READ;
    bChr[i] = svc->createCharacteristic(B_CHR_UUID[i], props, 512);
    bChr[i]->setValue("{}");
  }
  bChr[5]->setCallbacks(new BleCmdCb());
  listMx = xSemaphoreCreateMutex();
  bChr[3]->setCallbacks(new BleListCb(0));
  bChr[4]->setCallbacks(new BleListCb(1));
  bChr[7]->setCallbacks(new BleOtaCtl());
  bChr[8]->setCallbacks(new BleOtaData());
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BSV_UUID);
  adv->setScanResponse(true);
  adv->setMinInterval(640); // 400-500 ms: cheap on radio time
  adv->setMaxInterval(800);
  adv->start();
}
static void bleSet(int i, const String &s) {
  if (bChr[i] && s.length() <= 512) bChr[i]->setValue((const uint8_t *)s.c_str(), s.length());
}
static void bleRefresh(bool slow) {
  LedCfg led = ledGet();
  NetInfo n = netInfo();
  String live, conf, jk, devs;
  {
    DynamicJsonDocument d(1536), c(1024);
    c["name"] = devName;
    c["hwid"] = hwMac;
    c["pair"] = n.pair;
    c["paired"] = n.paired;
    c["cloud"] = n.cloud;
    c["ssid"] = n.ssid;
    c["conn"] = n.connected;
    c["ip"] = n.ip;
    c["wrssi"] = n.rssi;
    c["ap"] = wifiOn;
    c["tx"] = radioMax;
#ifdef BATTLAB_DISPLAY
    c["board"] = "cyd35";
    displayCfgGet(c.createNestedObject("disp"));
#else
    c["board"] = "s3";
#endif
    c["remote"] = 1;
    c["ota"] = 1;
    c["bleota"] = 1;
    c["fw"] = BATTLAB_FW;
    auto lo = c.createNestedObject("led");
    lo["on"] = led.on;
    lo["mode"] = led.mode;
    lo["color"] = led.color;
    lo["level"] = led.level;
    Guard g;
    c["mac"] = cfg.mac;
    c["proto"] = cfg.protocol;
    c["auto"] = cfg.automatic;
    c["atype"] = cfg.type;
    c["fam"] = shared.family;
    c["scanning"] = shared.scanning;
    c["lw"] = shared.lastWrite.substring(0, 60);
    d["fw"] = BATTLAB_FW;
    d["state"] = shared.state;
    d["bms"] = shared.detected;
    d["model"] = shared.model;
    d["rssi"] = shared.rssi;
    d["up"] = millis() / 1000;
    bool fresh = shared.linked && shared.have && uint32_t(millis() - shared.stamp) < 15000;
    if (fresh) {
      const auto &s = shared.sample;
      d["v"] = roundf(s.voltage * 100) / 100;
      d["i"] = roundf(s.current * 10) / 10;
      d["p"] = roundf(s.voltage * s.current);
      d["soc"] = s.soc;
      d["cap"] = roundf(s.capacity * 10) / 10;
      if (s.full > 0) d["full"] = roundf(s.full * 10) / 10;
      d["cyc"] = s.cycles;
      d["t1"] = s.temp1;
      d["t2"] = s.temp2;
      d["mos"] = s.mos;
      d["chg"] = s.charge;
      d["dsg"] = s.discharge;
      float mn = 9, mx = 0;
      auto cl = d.createNestedArray("cells");
      for (int k = 0; k < 32; k++)
        if (s.cells[k] > 0) {
          cl.add(serialized(String(s.cells[k], 3)));
          mn = min(mn, s.cells[k]);
          mx = max(mx, s.cells[k]);
        }
      if (mx > 0) { d["cmin"] = serialized(String(mn, 3)); d["cmax"] = serialized(String(mx, 3)); }
    }
    serializeJson(d, live);
    if (live.length() > 512) { d.remove("cells"); live = ""; serializeJson(d, live); }
    serializeJson(c, conf);
    if (slow) {
      if (shared.linked && shared.jk.valid && shared.family == 1) {
        DynamicJsonDocument j(512);
        auto v = j.createNestedArray("v");
        for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) v.add(serialized(String(shared.jk.v[i], 3)));
        auto sw = j.createNestedArray("sw");
        for (int i = 0; i < bmsw::SW_COUNT; i++) sw.add(shared.jk.sw[i]);
        serializeJson(j, jk);
      } else if (shared.linked && shared.pvHave && (shared.family == 2 || shared.family == 3)) {
        int n;
        bset::table(shared.family, n);
        DynamicJsonDocument j(1536);
        j["f"] = shared.family;
        auto v = j.createNestedArray("v");
        for (int i = 0; i < n; i++)
          if (shared.pvHave >> i & 1) v.add(serialized(String(shared.pv[i], shared.pv[i] == int(shared.pv[i]) ? 0 : 3)));
          else v.add(nullptr);
        serializeJson(j, jk);
      } else jk = "{}";
      devs = shared.devices;
    }
  }
  bleSet(0, live);
  bleSet(1, conf);
  if (!slow) return;
  bleSet(2, jk);
  { // BMS scan: batteries first, then strongest signal; up to 30, paged
    DynamicJsonDocument in(8192);
    std::vector<std::pair<int, String>> L;
    if (!deserializeJson(in, devs))
      for (JsonObject o : in.as<JsonArray>()) {
        StaticJsonDocument<192> x;
        auto a = x.to<JsonArray>();
        a.add(String((const char *)(o["name"] | "")).substring(0, 16));
        a.add(o["mac"]);
        a.add(o["rssi"]);
        a.add(o["type"]);
        a.add(o["candidate"] ? 1 : 0);
        String t;
        serializeJson(a, t);
        L.push_back({(o["candidate"] ? 1000 : 0) + int(o["rssi"] | -100) + 200, t});
      }
    std::stable_sort(L.begin(), L.end(), [](const std::pair<int, String> &a, const std::pair<int, String> &b) { return a.first > b.first; });
    std::vector<String> items;
    for (auto &e : L) { if (items.size() >= 30) break; items.push_back(e.second); }
    listSet(0, items);
  }
  String ws = netScanJson();
  if (ws.length()) { // Wi-Fi scan: strongest first, up to 30, paged
    DynamicJsonDocument in(6144);
    std::vector<std::pair<int, String>> L;
    if (!deserializeJson(in, ws))
      for (JsonObject o : in.as<JsonArray>()) {
        StaticJsonDocument<128> x;
        auto a = x.to<JsonArray>();
        a.add(String((const char *)(o["ssid"] | "")).substring(0, 32));
        a.add(o["rssi"]);
        a.add(o["lock"] ? 1 : 0);
        String t;
        serializeJson(a, t);
        L.push_back({int(o["rssi"] | -100), t});
      }
    std::stable_sort(L.begin(), L.end(), [](const std::pair<int, String> &a, const std::pair<int, String> &b) { return a.first > b.first; });
    std::vector<String> items;
    for (auto &e : L) { if (items.size() >= 30) break; items.push_back(e.second); }
    listSet(1, items);
  }
}
static void bleSetupTick() {
  if (!bleQmx) return;
  if (bOtaRestartAt && int32_t(millis() - bOtaRestartAt) > 0) ESP.restart();
  if (bleOtaBusy && millis() - bOtaLast > 30000) bOtaFail("no data for 30 s");
  std::string m;
  xSemaphoreTake(bleQmx, portMAX_DELAY);
  if (!bleQ.empty()) { m = bleQ.front(); bleQ.erase(bleQ.begin()); }
  xSemaphoreGive(bleQmx);
  static uint32_t lastFast = 0, lastSlow = 0;
  if (m.size()) {
    StaticJsonDocument<768> in;
    bool ok = false;
    String msg = "bad JSON";
    long id = 0;
    bool reboot = false;
    if (!deserializeJson(in, m)) {
      id = in["id"] | 0L;
      String cmd = in["cmd"] | "";
      JsonObjectConst a = in["args"].as<JsonObjectConst>();
      if (cmd == "home_wifi") { // local link: apply at once, no auto-revert needed
        String ss = a["ssid"] | "", pw = a["pass"] | "";
        ok = ss.length() && ss.length() <= 32 && pw.length() <= 64;
        msg = ok ? "joining " + ss : String("SSID 1-32, password up to 64");
        if (ok) netSetHomeWifi(ss, pw);
      } else if (cmd == "forget_wifi") { netForgetHomeWifi(); ok = true; msg = "home Wi-Fi removed"; }
      else if (cmd == "wifi_ap") { wifiSetEnabled(a["on"] | true); ok = true; msg = wifiOn ? "hotspot on" : "hotspot off"; }
      else if (cmd == "restart") { ok = true; msg = "restarting"; reboot = true; }
      else msg = runCloudCmd(cmd, a, ok);
    }
    StaticJsonDocument<256> r;
    r["id"] = id;
    r["ok"] = ok;
    r["msg"] = msg;
    String rs;
    serializeJson(r, rs);
    if (bChr[6]) { bleSet(6, rs); bChr[6]->notify(); }
    if (millis() - serialSetupAt < 120000) Serial.printf("\n@BL R %s\n", rs.c_str());
    Serial.printf("[BLE-SETUP] %s -> %s\n", m.c_str(), msg.c_str());
    if (reboot) { delay(600); ESP.restart(); }
    lastFast = 0; // push the new state right away
  }
  if (!bleClient && millis() - serialSetupAt > 15000) return;
  uint32_t now = millis();
  if (now - lastFast < 1000) return;
  lastFast = now;
  bool slow = now - lastSlow > 2500;
  if (slow) lastSlow = now;
  bleRefresh(slow);
}

#ifndef BATTLAB_DISPLAY
// ---- JK BMS over the RS485 cable (RS485-to-TTL board on two GPIOs) -------
// Tries the JK-PB Modbus request and the older 4E 57 request in turn; if
// nothing answers it also tries with RX/TX swapped (common wiring mistake).
static void rs485Publish(const jk::Sample &s, const char *what) {
  Guard g;
  shared.sample = s;
  shared.have = shared.linked = true;
  shared.stamp = millis();
  shared.state = "LIVE";
  shared.family = 1;
  shared.detected = what;
  shared.good++;
}
static uint8_t rsFam = 1; // family on the cable: 1 JK, 4 DALY
// Rebuild the main sample from the packs (call with the Guard held).
static void rs485Recompute() {
  uint32_t now = millis(), win = shared.busMode == 1 ? 90000 : 15000;
  jk::Sample ss[16];
  bool fresh[16];
  int n = 0, first = -1;
  for (int k = 0; k < 16; k++) {
    fresh[k] = shared.packs[k].stamp && now - shared.packs[k].stamp < win;
    if (fresh[k]) { ss[k] = shared.packs[k].s; n++; if (first < 0) first = k; }
  }
  shared.bankN = n;
  if (!n) return;
  bool bank = n > 1 || shared.busMode == 1;
  if (!bank) shared.sample = ss[first];
  else {
    auto t = jk485::bankTotals(ss, fresh, 16);
    jk::Sample m = ss[first]; // cells of the first pack for the local page
    m.voltage = t.v; m.current = t.i; m.soc = t.soc; m.capacity = t.cap; m.full = t.full;
    m.temp1 = t.tmax; m.temp2 = t.tmax; m.mos = t.mos; m.cycles = t.cycles;
    m.charge = t.chg; m.discharge = t.dsg;
    shared.sample = m;
  }
  if (rsFam == 1) {
    int sel = linkCfg.addr;
    if (!shared.packs[sel].js.valid)
      for (int k = 0; k < 16; k++) if (shared.packs[k].js.valid) { sel = k; break; }
    if (shared.packs[sel].js.valid) { shared.jk = shared.packs[sel].js; shared.jkStamp = shared.packs[sel].setStamp; shared.haveParams = true; }
  }
  shared.have = shared.linked = true;
  shared.stamp = now;
  shared.state = "LIVE";
  shared.family = rsFam;
  const char *b = rsFam == 4 ? "DALY" : "JK";
  shared.detected = shared.busMode == 1 ? String("JK RS485 bank (master DIP 0)") : String(b) + (bank ? " RS485 bank" : " RS485");
  shared.good++;
}
static void rs485Task(void *) {
  HardwareSerial &U = Serial1;
  bool started = false, swapped = false;
  int8_t rx = -1, tx = -1;
  uint32_t baud = 0, nextTx = 0, lastOk = 0, startAt = 0, bytes = 0, ourTxEnd = 0, lastReqAt = 0, lastByteAt = 0;
  uint32_t lastSet[16] = {0}, lastInfo[16] = {0}, lastCells = 0;
  uint8_t proto = 0, tries = 0; // 1 JK-PB Modbus, 2 older JK 4E 57, 3 DALY (Modbus-like), 4 DALY A5
  uint8_t mode = 0;             // 0 deciding (listening), 1 listen (master present), 2 poll
  uint8_t lastReqAddr = 0xFF, pollIdx = 0, cfgBus = 0xFF, cfgCount = 0, cfgAddr = 0xFF, cfgBrand = 0xFF;
  uint8_t dBlock = 0, a5Idx = 0;
  bool everOk = false;
  jk::Parser parser;
  jk485::OldFramer old;
  jk485::BusWatcher watch;
  daly::D2Framer dF;
  daly::A5Framer aF;
  daly::A5State aS;
  static daly::WireRegs dRegs[16];
  uint8_t f[32];
  auto setMode = [&](uint8_t m) {
    mode = m;
    Guard g;
    shared.busMode = m == 1 ? 1 : (m == 2 && linkCfg.count > 1) ? 2 : 0;
  };
  auto storePack = [&](uint8_t a, const jk::Sample &s, uint8_t fam) {
    rsFam = fam;
    proto = fam == 4 ? proto : 1;
    lastOk = millis();
    everOk = true;
    Guard g;
    shared.packs[a].s = s;
    shared.packs[a].stamp = millis();
    rs485Recompute();
  };
  auto handlePb = [&](const uint8_t *b) {
    proto = 1;
    rsFam = 1;
    lastOk = millis();
    everOk = true;
    uint8_t a = jk485::frameOwner(lastReqAddr, lastReqAt, millis(), 600);
    if (mode == 2 && linkCfg.count == 1) a = linkCfg.addr;
    Guard g;
    auto &pk = shared.packs[a];
    if (b[4] == 2) {
      jk::Sample s;
      if (jk::decode(b, true, s) || jk::decode(b, false, s)) { pk.s = s; pk.stamp = millis(); rs485Recompute(); }
    } else if (b[4] == 1) {
      bmsw::JkSettings js;
      if (bmsw::parseJkSettings(b, js)) { pk.js = js; pk.setStamp = millis(); rs485Recompute(); }
    } else if (b[4] == 3) {
      pk.model = asciiField(b + 6, 16);
      serialOf(b, pk.serial);
      {
        std::string cb, cs;
        pk.codeBt = jk::readCode(b, 300, 62, cb) ? String(cb.c_str()) : "";
        pk.codeSet = jk::readCode(b, 300, 118, cs) ? String(cs.c_str()) : "";
      }
      if (a == linkCfg.addr || shared.model.length() == 0) {
        shared.model = pk.model;
        shared.hardware = asciiField(b + 22, 8);
        shared.software = asciiField(b + 30, 8);
      }
    }
  };
  auto send = [&](const uint8_t *p, int k) {
    U.write(p, k);
    U.flush();
    ourTxEnd = millis() + (uint32_t(k) * 10000UL) / baud + 40;
  };
  bool tryJk = true, tryDaly = true;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!linkRs485 || otaBusy || bleOtaBusy) {
      if (started) { U.end(); started = false; }
      proto = 0;
      vTaskDelay(pdMS_TO_TICKS(400));
      continue;
    }
    int8_t wr = swapped ? linkCfg.tx : linkCfg.rx, wt = swapped ? linkCfg.rx : linkCfg.tx;
    bool cfgChanged = cfgBus != linkCfg.bus || cfgCount != linkCfg.count || cfgAddr != linkCfg.addr || cfgBrand != linkCfg.brand;
    if (!started || rx != wr || tx != wt || baud != linkCfg.baud || cfgChanged) {
      if (started) U.end();
      U.setRxBufferSize(2048);
      U.begin(linkCfg.baud, SERIAL_8N1, wr, wt);
      started = true;
      rx = wr; tx = wt; baud = linkCfg.baud;
      proto = 0; tries = 0; bytes = 0;
      startAt = lastByteAt = millis();
      if (cfgChanged) {
        cfgBus = linkCfg.bus; cfgCount = linkCfg.count; cfgAddr = linkCfg.addr; cfgBrand = linkCfg.brand;
        everOk = false;
        Guard g;
        for (auto &pk : shared.packs) { pk.stamp = 0; pk.setStamp = 0; pk.js.valid = false; pk.model = ""; pk.serial[0] = 0; }
        shared.bankN = 0;
        for (auto &w : dRegs) w.have = 0;
      }
      tryJk = linkCfg.brand != 2;
      tryDaly = linkCfg.brand != 1;
      // DALY has no "master" on the bus: auto goes straight to polling for a DALY-only setup
      setMode(linkCfg.bus == 1 ? 1 : (linkCfg.bus == 2 || linkCfg.brand == 2) ? 2 : 0);
      if (mode == 0) { Guard g; shared.state = "RS485_LISTENING"; }
    }
    uint32_t t = millis();
    bool heard = false;
    while (U.available()) {
      uint8_t c = U.read();
      bytes++;
      lastByteAt = t;
      if (int32_t(millis() - ourTxEnd) > 0) heard = true;
      jk485::BusReq r;
      if (watch.feed(c, r) && !r.write) {
        lastReqAddr = r.addr;
        lastReqAt = millis();
        // a request we did not send = another master (a JK pack with DIP 0) runs the bus
        if (int32_t(millis() - ourTxEnd) > 0 && mode != 1 && linkCfg.bus != 2 && tryJk) setMode(1);
      }
      if (tryJk) parser.feed(&c, 1, handlePb);
      if (mode == 2 && linkCfg.count == 1 && tryJk && old.feed(c)) {
        jk::Sample s;
        if (jk485::decodeOld(old.data, old.dataLen, s)) { proto = 2; rsFam = 1; lastOk = millis(); everOk = true; rs485Publish(s, "JK RS485 (older)"); }
      }
      if (mode == 2 && tryDaly) {
        dF.feed(&c, 1, [&](uint8_t st, uint8_t fn, const uint8_t *d, size_t n) {
          if (fn != 3 || st < 0x50 || st > 0x5F) return;
          uint8_t a = st - 0x50;
          if (a > 15 || dBlock > 2) return;
          proto = 3;
          dRegs[a].store(dBlock, d, n);
          jk::Sample smp;
          if (dBlock == 2 && daly::decodeWire(dRegs[a], smp)) storePack(a, smp, 4);
          lastOk = millis();
        });
        aF.feed(&c, 1, [&](uint8_t id, const uint8_t *d) {
          proto = 4;
          lastOk = millis();
          if (aS.add(id, d)) storePack(linkCfg.addr, aS.s, 4);
        });
      }
    }
    // keep the bank fresh-count current even when a pack goes quiet
    static uint32_t lastRecalc = 0;
    if (t - lastRecalc > 2000) { lastRecalc = t; Guard g; if (shared.bankN || mode == 1) rs485Recompute(); }

    if (mode == 0) { // auto: listen first; any traffic we did not cause means a master runs the bus
      if (heard && tryJk) setMode(1);
      else if (t - startAt > 40000) setMode(2);
      continue;
    }
    if (mode == 1) { // listen only: never transmit while a master owns the bus
      {
        Guard g;
        if (cmdN) { // single-pack writes cannot go out on a bus we do not own
          memmove(cmdQ, cmdQ + 1, sizeof(BmsCmd) * (cmdN - 1)); memmove(cmdAddrQ, cmdAddrQ + 1, cmdN - 1); cmdN--;
          shared.lastWrite = "Choose a pack: settings go to it over Bluetooth while a master (DIP 0) runs the bus";
        }
        readWanted = false;
      }
      if (!everOk && t - lastByteAt > 45000) { // never heard anything: maybe RX is on the other pin
        lastByteAt = t;
        swapped = !swapped;
        Guard g;
        shared.state = "RS485_NO_ANSWER";
        shared.linked = shared.have = false;
      } else if (bytes > 200 && t - lastOk > 60000) {
        lastOk = t;
        Guard g;
        shared.state = "RS485_NO_FRAMES"; // traffic but nothing readable: baud rate?
        if (!shared.bankN) shared.linked = shared.have = false;
      }
      continue;
    }
    // ---- mode 2: the board asks the packs ----
    if (int32_t(t - nextTx) < 0) continue;
    BmsCmd cmd;
    uint8_t cmdAddr = 0xFF;
    bool haveCmd = false;
    {
      Guard g;
      if (cmdN) { cmd = cmdQ[0]; cmdAddr = cmdAddrQ[0]; memmove(cmdQ, cmdQ + 1, sizeof(BmsCmd) * (cmdN - 1)); memmove(cmdAddrQ, cmdAddrQ + 1, cmdN - 1); cmdN--; haveCmd = true; }
      if (readWanted) { readWanted = false; memset(lastSet, 0, sizeof lastSet); memset(lastInfo, 0, sizeof lastInfo); }
    }
    if (haveCmd) {
      String result = cmd.isSwitch ? String(cmd.idx == 0 ? "Charge" : cmd.idx == 1 ? "Discharge" : "Balancer") + (cmd.on ? " ON" : " OFF") : String("");
      uint8_t to = cmdAddr <= 15 ? cmdAddr : linkCfg.addr;
      int k = 0;
      if (proto == 1) {
        uint16_t reg;
        uint32_t raw;
        if (cmd.isSwitch) {
          reg = 0x1000 + (bmsw::JK_SW_OFFSET[cmd.idx] - 6);
          raw = cmd.on ? 1 : 0;
        } else {
          const auto &p = bmsw::JK_PARAMS[cmd.idx];
          reg = 0x1000 + (p.offset - 6);
          raw = bmsw::jkRaw(cmd.idx, cmd.value);
          result = String(p.name) + " = " + String(cmd.value, int(p.decimals)) + " " + p.unit;
        }
        k = jk485::pbWrite(f, to, reg, raw);
        lastSet[to] = 0; // read the settings back
      } else if (proto == 3 && cmd.isSwitch && cmd.idx < 2) {
        k = daly::d2Frame(f, 0x80 + to, 6, daly::WIRE_MOS_REG[cmd.idx], cmd.on ? 1 : 0);
      } else if (proto == 4 && cmd.isSwitch && cmd.idx < 2) {
        k = daly::a5Frame(f, 0x40, cmd.idx == 0 ? 0xDA : 0xD9, cmd.on ? 1 : 0);
      }
      if (k) { send(f, k); result = String("Sent (RS485 pack ") + to + "): " + result; }
      else result = proto == 3 || proto == 4 ? "DALY: only charge / discharge MOS can be switched" : "Writing needs a JK-PB / inverter BMS on RS485";
      Guard g;
      shared.lastWrite = result;
      nextTx = t + 400;
      continue;
    }
    int k = 0;
    uint8_t first = linkCfg.addr, cnt = linkCfg.count;
    if (proto == 0) {
      // try every protocol the brand setting allows, one per slot
      uint8_t a = first + (tries / 4) % cnt;
      switch (tries % 4) {
        case 0: if (tryJk) { k = jk485::pbRequest(f, a, 2); lastReqAddr = a; lastReqAt = t; } break;
        case 1: if (tryDaly) { dBlock = 0; k = daly::d2Frame(f, 0x80 + a, 3, daly::WIRE_BLOCKS[0].reg, daly::WIRE_BLOCKS[0].count); } break;
        case 2: if (tryJk && cnt == 1) k = jk485::oldReadAll(f); break;
        default: if (tryDaly && cnt == 1) k = daly::a5Frame(f, 0x40, 0x90); break;
      }
      if (++tries >= 8 + 4 * cnt) { // nothing answered: try the other wiring
        tries = 0;
        swapped = !swapped;
        Guard g;
        shared.state = "RS485_NO_ANSWER";
        shared.linked = false;
      }
      nextTx = t + 500;
    } else if (proto == 1) {
      // one pack per slot: cells round-robin (~1 s per pack), settings 30 s, info 120 s
      uint8_t a = first + pollIdx % cnt;
      pollIdx++;
      uint8_t type = 2;
      if (t - lastSet[a] > 30000 || !lastSet[a]) { type = 1; lastSet[a] = t; }
      else if (t - lastInfo[a] > 120000 || !lastInfo[a]) { type = 3; lastInfo[a] = t; }
      else if (cnt == 1 && t - lastCells < 900) { nextTx = t + 100; continue; }
      if (type == 2) lastCells = t;
      k = jk485::pbRequest(f, a, type);
      lastReqAddr = a; lastReqAt = t;
      nextTx = t + (cnt > 1 ? max(150, 1000 / cnt) : 250);
    } else if (proto == 3) { // DALY: three register blocks per pack
      uint8_t a = first + (pollIdx / 3) % cnt;
      dBlock = pollIdx % 3;
      pollIdx++;
      uint16_t regs = dBlock == 0 ? std::max<uint16_t>(16, std::min<uint16_t>(32, dRegs[a].r[0x3C] ? dRegs[a].r[0x3C] : 32)) : daly::WIRE_BLOCKS[dBlock].count;
      k = daly::d2Frame(f, 0x80 + a, 3, daly::WIRE_BLOCKS[dBlock].reg, regs);
      nextTx = t + (cnt > 1 ? 120 : 300);
    } else if (proto == 4) {
      k = daly::a5Frame(f, 0x40, daly::A5_POLL[a5Idx++ % 5]);
      nextTx = t + 200;
    } else {
      k = jk485::oldReadAll(f);
      nextTx = t + 1500;
    }
    if (k) send(f, k);
    if (proto && t - lastOk > 10000 + 1000 * cnt) { // lost
      proto = 0;
      Guard g;
      shared.state = "RS485_NO_ANSWER";
      if (!shared.bankN) shared.linked = shared.have = false;
    }
  }
}
#endif

void bmsScan() {
  Guard g;
  scanWanted = true;
}
bool bmsConnect(const String &mac, uint8_t type, const String &p) {
  if (!validMac(mac) ||
      (p != "AUTO" && p != "JK02_24S" && p != "JK02_32S" && p != "ANT" && p != "JBD" && p != "DALY") ||
      type > 1)
    return false;
  {
    Guard g;
    persistWanted = false;
    cfg.automatic = p == "AUTO";
    cfg.mac = mac;
    cfg.protocol = p;
    cfg.type = type;
    configChanged = true;
    generation++;
    shared.have = false;
    shared.haveParams = false;
    shared.detected = "";
    shared.model = "";
    shared.hardware = "";
    shared.software = "";
    shared.linked = false;
    shared.state = "RECONFIGURING";
  }
  Preferences pr;
  pr.begin("battlab-jk", false);
  pr.putBool("auto", p == "AUTO");
  pr.putString("mac", mac);
  pr.putString("protocol", p);
  pr.putUChar("type", type);
  pr.end();
  return true;
}
void bmsAuto() {
  {
    Guard g;
    persistWanted = false;
    cfg.automatic = true;
    cfg.mac = "";
    cfg.protocol = "AUTO";
    configChanged = true;
    generation++;
    shared.have = false;
    shared.haveParams = false;
    shared.detected = "";
    shared.linked = false;
    shared.state = "AUTO_DISCOVERY";
  }
  Preferences pr;
  pr.begin("battlab-jk", false);
  pr.putBool("auto", true);
  pr.remove("mac");
  pr.putString("protocol", "AUTO");
  pr.end();
}
void bmsDisconnect() {
  {
    Guard g;
    persistWanted = false;
    cfg.automatic = false;
    cfg.mac = "";
    configChanged = true;
    generation++;
    shared.have = false;
    shared.haveParams = false;
    shared.linked = false;
    shared.state = "UNCONFIGURED";
  }
  Preferences pr;
  pr.begin("battlab-jk", false);
  pr.putBool("auto", false);
  pr.remove("mac");
  pr.end();
}
void takeDisplaySnapshot(DisplaySnap &o) {
  energyGet(o.eIn, o.eOut);
  {
    Guard g;
    o.state = shared.state;
    o.detected = shared.detected;
    o.model = shared.model;
    o.mac = cfg.mac;
    o.probe = shared.probe;
    o.linked = shared.linked;
    o.scanning = shared.scanning;
    o.live = shared.linked && shared.have &&
             uint32_t(millis() - shared.stamp) < 15000;
    o.sample = shared.sample;
    o.prof = profCfg.type;
    o.whkm = profCfg.whkm;
    o.rangeAlarm = profCfg.rangeAlarm;
    o.rssi = shared.rssi;
    o.rxBytes = shared.rxBytes;
    o.good = shared.good;
    o.ageMs = shared.have ? uint32_t(millis() - shared.stamp) : 0;
    o.devices = shared.devices;
    o.protocol = cfg.protocol;
    o.automatic = cfg.automatic;
    o.connAuth = shared.connectionAuth;
  }
  o.wifiOn = wifiOn;
  o.radioMax = radioMax;
  o.bleTx = bleTxText();
  {
    Guard g;
    o.jk = shared.jk;
    o.jkFresh = shared.jk.valid && uint32_t(millis() - shared.jkStamp) < 600000;
    o.family = shared.family;
    o.pvHave = shared.pvHave;
    memcpy(o.pv, shared.pv, sizeof o.pv);
    o.lastWrite = shared.lastWrite;
  }
  o.resetReason = resetReasonText;
  o.resetAbnormal = resetAbnormal;
  o.freeHeap = ESP.getFreeHeap();
  o.minHeap = ESP.getMinFreeHeap();
  o.maxBlock = ESP.getMaxAllocHeap();
  o.firmware = BATTLAB_FW;
  o.uptimeMs = millis();
  o.stations = WiFi.softAPgetStationNum();
}
// ---- energy today (kWh in / out), counted on the board, reset at midnight ----
struct EnergyDay { float in = 0, out = 0; int day = -1; uint32_t lastMs = 0, savedAt = 0; bool dirty = false; };
static EnergyDay energy;
static SemaphoreHandle_t energyMx = nullptr;
static void energySave() {
  Preferences p;
  p.begin("battlab-en", false);
  p.putInt("day", energy.day);
  p.putFloat("in", energy.in);
  p.putFloat("out", energy.out);
  p.end();
  energy.dirty = false;
  energy.savedAt = millis();
}
static void energyLoad() {
  Preferences p;
  p.begin("battlab-en", true);
  energy.day = p.getInt("day", -1);
  energy.in = p.getFloat("in", 0);
  energy.out = p.getFloat("out", 0);
  p.end();
}
void energyGet(float &in, float &out) {
  if (energyMx) xSemaphoreTake(energyMx, portMAX_DELAY);
  in = energy.in; out = energy.out;
  if (energyMx) xSemaphoreGive(energyMx);
}
static void energyTick() {
  uint32_t now = millis();
  if (now - energy.lastMs < 1000) return;
  float hours = energy.lastMs ? (now - energy.lastMs) / 3600000.f : 0;
  energy.lastMs = now;
  time_t t = time(nullptr);
  bool timeOk = t > 1700000000;
  bool live;
  float pw = 0;
  {
    Guard g;
    live = shared.linked && shared.have && uint32_t(now - shared.stamp) < 15000;
    if (live) pw = shared.sample.voltage * shared.sample.current;
  }
  if (energyMx) xSemaphoreTake(energyMx, portMAX_DELAY);
  if (timeOk) {
    struct tm tm;
    localtime_r(&t, &tm);
    int day = (tm.tm_year + 1900) * 400 + tm.tm_yday;
    if (day != energy.day) { // a new day (or first clock after boot on another day)
      energy.in = energy.out = 0;
      energy.day = day;
      energy.dirty = true;
    }
  }
  if (live && hours > 0 && hours < 0.01f) {
    if (pw > 0) energy.in += pw * hours / 1000;
    else energy.out += -pw * hours / 1000;
    energy.dirty = true;
  }
  bool save = timeOk && energy.dirty && now - energy.savedAt > 600000;
  if (energyMx) xSemaphoreGive(energyMx);
  if (save) energySave();
}

// ---- setup from a computer over USB (website "Set up over USB") ----------
//  "@BL get N"      -> "@BL N <json>"  (N = the Bluetooth setup values 0..4)
//  "@BL cmd {json}" -> same commands as Bluetooth; reply "@BL R <json>"
//  Improv Wi-Fi packets (ESP Web Tools) -> join the home Wi-Fi right after flashing
static void improvSend(uint8_t type, const std::vector<uint8_t> &d) {
  auto p = improv::packet(type, d);
  Serial.write(p.data(), p.size());
  Serial.write('\n');
  Serial.flush();
}
static std::string claimUrl() {
  return std::string("https://") + CLOUD_SITE + "/#claim/" + pairCode.c_str() + "/" + devName.c_str();
}
static void serialSetupTask(void *) {
  improv::Parser ip;
  String line;
  for (;;) {
    while (Serial.available()) {
      uint8_t c = Serial.read();
      uint8_t type;
      std::vector<uint8_t> d;
      if (ip.feed(c, type, d)) {
        serialSetupAt = millis();
        if (type != improv::T_RPC || d.empty()) continue;
        uint8_t cmd = d[0];
        bool conn = WiFi.status() == WL_CONNECTED;
        if (cmd == improv::C_STATE) {
          improvSend(improv::T_STATE, {uint8_t(conn ? improv::S_PROVISIONED : improv::S_AUTHORIZED)});
          if (conn) improvSend(improv::T_RESULT, improv::result(improv::C_STATE, {claimUrl()}));
        } else if (cmd == improv::C_INFO) {
#ifdef BATTLAB_DISPLAY
          const char *chip = "ESP32";
#else
          const char *chip = "ESP32-S3";
#endif
          improvSend(improv::T_RESULT, improv::result(improv::C_INFO, {"BATTLAB", BATTLAB_FW, chip, devName.c_str()}));
        } else if (cmd == improv::C_SCAN) {
          int n = -1;
          for (int k = 0; k < 3 && n < 0; k++) { n = WiFi.scanNetworks(false, false); if (n < 0) vTaskDelay(pdMS_TO_TICKS(800)); }
          for (int i = 0; i < n && i < 20; i++)
            improvSend(improv::T_RESULT, improv::result(improv::C_SCAN, {WiFi.SSID(i).c_str(), std::to_string(WiFi.RSSI(i)),
                                                                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"}));
          if (n >= 0) WiFi.scanDelete();
          improvSend(improv::T_RESULT, improv::result(improv::C_SCAN, {}));
        } else if (cmd == improv::C_WIFI) {
          std::string ss, pw;
          if (!improv::wifiSettings(d, ss, pw) || ss.size() > 32 || pw.size() > 64) { improvSend(improv::T_ERROR, {improv::E_INVALID}); continue; }
          improvSend(improv::T_STATE, {improv::S_PROVISIONING});
          netSetHomeWifi(String(ss.c_str()), String(pw.c_str()));
          uint32_t t0 = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(250));
          if (WiFi.status() == WL_CONNECTED) {
            improvSend(improv::T_STATE, {improv::S_PROVISIONED});
            improvSend(improv::T_RESULT, improv::result(improv::C_WIFI, {claimUrl()}));
          } else {
            improvSend(improv::T_ERROR, {improv::E_CONNECT});
            improvSend(improv::T_STATE, {improv::S_AUTHORIZED});
          }
        } else improvSend(improv::T_ERROR, {improv::E_UNKNOWN_RPC});
        continue;
      }
      if (c == '\n' || c == '\r') {
        if (line.startsWith("@BL ")) {
          serialSetupAt = millis();
          if (line.startsWith("@BL get ")) {
            int i = line.substring(8).toInt();
            String v = "{}";
            if (i == 3 || i == 4) v = listNext(i - 3);
            else if (i >= 0 && i < 9 && bChr[i]) { NimBLEAttValue a = bChr[i]->getValue(); v = String((const char *)a.data(), a.length()); }
            Serial.printf("\n@BL %d %s\n", i, v.c_str());
          } else if (line.startsWith("@BL cmd ") && bleQmx) {
            std::string m = line.substring(8).c_str();
            xSemaphoreTake(bleQmx, portMAX_DELAY);
            if (bleQ.size() < 8) bleQ.push_back(m);
            xSemaphoreGive(bleQmx);
          } else if (line == "@BL hello") Serial.printf("\n@BL hello %s %s %s\n", devName.c_str(), hwMac.c_str(), BATTLAB_FW);
        }
        line = "";
      } else if (line.length() < 700) line += char(c);
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
void setup() {
  Serial.begin(115200);
  readResetReason();
  {
    // Safety net for cloud updates: three crash restarts in a row right
    // after boot -> start the previous firmware from the other OTA slot.
    Preferences b;
    b.begin("battlab-boot", false);
    uint8_t fails = resetAbnormal ? b.getUChar("fails", 0) + 1 : 0;
    b.putUChar("fails", fails);
    b.end();
    if (fails >= 3) {
      const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
      esp_app_desc_t desc;
      if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK &&
          esp_ota_set_boot_partition(other) == ESP_OK) {
        Preferences c;
        c.begin("battlab-boot", false);
        c.putUChar("fails", 0);
        c.end();
        Serial.println("Repeated crashes - starting the previous firmware");
        delay(200);
        ESP.restart();
      }
    }
  }
  {
    uint64_t mac = ESP.getEfuseMac();
    char nm[16];
    uint8_t m6[6];
    for (int i = 0; i < 6; i++) m6[i] = (mac >> (8 * i)) & 0xff;
    snprintf(nm, sizeof nm, "BATTLAB-%02X%02X%02X", m6[3], m6[4], m6[5]);
    devName = nm;
    char hm[18];
    snprintf(hm, sizeof hm, "%02X:%02X:%02X:%02X:%02X:%02X", m6[0], m6[1], m6[2], m6[3], m6[4], m6[5]);
    hwMac = hm;
  }
  lock = xSemaphoreCreateMutex();
  prefs.begin("battlab-jk", false);
  cfg.mac = prefs.getString("mac", "");
  cfg.protocol = prefs.getString("protocol", "AUTO");
  cfg.automatic = prefs.getBool("auto", true);
  cfg.type = prefs.getUChar("type", 0);
#ifdef BATTLAB_DISPLAY
  wifiOn = prefs.getBool("wifi", true); // can be switched off on the screen
#endif
  radioMax = prefs.getBool("txmax", true);
#ifndef BATTLAB_DISPLAY
  linkCfg.rs485 = prefs.getBool("l485", false);
  linkCfg.rx = prefs.getChar("lrx", 18);
  linkCfg.tx = prefs.getChar("ltx", 17);
  linkCfg.baud = prefs.getUInt("lbaud", 115200);
  linkCfg.addr = prefs.getUChar("laddr", 1);
  linkCfg.bus = prefs.getUChar("lbus", 0);
  linkCfg.count = prefs.getUChar("lcnt", 1);
  linkCfg.brand = prefs.getUChar("lbrand", 0);
  if (linkCfg.brand > 2) linkCfg.brand = 0;
  if (linkCfg.bus > 2) linkCfg.bus = 0;
  if (linkCfg.count < 1 || linkCfg.addr + linkCfg.count > 16) linkCfg.count = 1;
  linkRs485 = linkCfg.rs485;
  btLoad();
#endif
  profCfg.type = prefs.getUChar("ptype", 0);
  if (profCfg.type > 4) profCfg.type = 0;
  profCfg.whkm = prefs.getUShort("pwhkm", 0);
  profCfg.rangeAlarm = prefs.getUChar("prange", 10);
  netLoad();
  energyMx = xSemaphoreCreateMutex();
  energyLoad();
  xTaskCreatePinnedToCore(serialSetupTask, "usbsetup", 6144, nullptr, 1, nullptr, 1);
  wifiApply();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com"); // Thailand time for the screen clock
  bool apStarted = wifiOn;
  Serial.printf("\nLast reset: %s\n", resetReasonText);
  Serial.printf("\nBATTLAB " BATTLAB_FW "\nWi-Fi + Bluetooth name: %s (open)\nAP started: %s\nhttp://192.168.4.1\n", devName.c_str(), apStarted ? "YES" : "NO");
  const char *headers[] = {"Origin"};
  web.collectHeaders(headers, 1);
  web.on("/", HTTP_GET,
         []() { web.send_P(200, "text/html; charset=utf-8", UI_HTML); });
  web.on("/api/state", HTTP_GET, stateApi);
  web.on("/api/bms/params", HTTP_GET, []() {
    DynamicJsonDocument d(12288);
    {
      Guard g;
      d["family"] = shared.family;
      d["valid"] = shared.jk.valid;
      d["last_write"] = shared.lastWrite;
      auto a = d.createNestedArray("params");
      if (shared.family == 2 || shared.family == 3) { // ANT / JBD table
        int n;
        const bset::Def *tb = bset::table(shared.family, n);
        d["valid"] = shared.pvHave != 0;
        d["experimental"] = shared.family == 2;
        for (int i = 0; i < n; i++) {
          auto o = a.createNestedObject();
          o["index"] = i;
          o["name"] = tb[i].name;
          o["unit"] = tb[i].unit;
          o["min"] = tb[i].min;
          o["max"] = tb[i].max;
          o["step"] = tb[i].step;
          if (shared.pvHave >> i & 1) o["value"] = shared.pv[i];
        }
      }
      for (int i = 0; shared.family == 1 && i < bmsw::JK_PARAM_COUNT; i++) {
        const auto &p = bmsw::JK_PARAMS[i];
        auto o = a.createNestedObject();
        o["index"] = i;
        o["name"] = p.name;
        o["unit"] = p.unit;
        o["min"] = p.min;
        o["max"] = p.max;
        o["step"] = p.step;
        if (shared.jk.valid) o["value"] = shared.jk.v[i];
      }
      auto sw = d.createNestedArray("switches");
      for (int i = 0; i < bmsw::SW_COUNT; i++) sw.add(shared.jk.sw[i]);
    }
    json(200, d);
  });
  web.on("/api/bms/param", HTTP_POST, []() {
    if (!localRequest()) return;
    StaticJsonDocument<128> in;
    if (web.arg("plain").length() > 128 || deserializeJson(in, web.arg("plain"))) { error(400, "Invalid JSON"); return; }
    if (!in["index"].is<int>() || !in["value"].is<float>()) { error(400, "index and value required"); return; }
    if (const char *e = bmsWriteParam(in["index"].as<int>(), in["value"].as<float>())) { error(409, e); return; }
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.on("/api/bms/switch", HTTP_POST, []() {
    if (!localRequest()) return;
    StaticJsonDocument<128> in;
    if (web.arg("plain").length() > 128 || deserializeJson(in, web.arg("plain"))) { error(400, "Invalid JSON"); return; }
    if (!in["which"].is<int>() || !in["on"].is<bool>()) { error(400, "which and on required"); return; }
    if (const char *e = bmsSetSwitch(in["which"].as<int>(), in["on"].as<bool>())) { error(409, e); return; }
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.on("/api/net", HTTP_GET, []() {
    NetInfo n = netInfo();
    DynamicJsonDocument d(4096);
    d["ssid"] = n.ssid;
    d["connected"] = n.connected;
    d["ip"] = n.ip;
    d["rssi"] = n.rssi;
    d["cloud"] = n.cloud;
    d["cloud_configured"] = n.configured;
    d["paired"] = n.paired;
    d["pair_code"] = n.pair;
    d["site"] = n.site;
    d["sent"] = n.sent;
    String scan = netScanJson();
    if (scan.length()) d["scan"] = serialized(scan);
    else d["scanning"] = true;
    json(200, d);
  });
  web.on("/api/net/scan", HTTP_POST, []() {
    if (!localRequest()) return;
    netScanStart();
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.on("/api/net", HTTP_POST, []() {
    if (!localRequest()) return;
    StaticJsonDocument<256> in;
    if (web.arg("plain").length() > 200 || deserializeJson(in, web.arg("plain"))) { error(400, "Invalid JSON"); return; }
    String ssid = in["ssid"] | "", pass = in["pass"] | "";
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 64) { error(400, "SSID 1-32, password up to 64 characters"); return; }
    netSetHomeWifi(ssid, pass);
    web.send(202, "application/json", "{\"saved\":true}");
  });
  web.on("/api/net/forget", HTTP_POST, []() {
    if (!localRequest()) return;
    netForgetHomeWifi();
    web.send(202, "application/json", "{\"forgotten\":true}");
  });
  web.on("/api/led", HTTP_GET, []() {
    DynamicJsonDocument d(1024);
    ledJson(d);
    json(200, d);
  });
  web.on("/api/led", HTTP_POST, []() {
    if (!localRequest()) return;
    if (web.arg("plain").length() > 256) { error(413, "Too large"); return; }
    StaticJsonDocument<256> in;
    if (deserializeJson(in, web.arg("plain"))) { error(400, "Invalid JSON"); return; }
    LedCfg c = ledGet();
    if (in.containsKey("on")) c.on = in["on"].as<bool>();
    if (in.containsKey("mode")) c.mode = in["mode"].as<int>();
    if (in.containsKey("color")) c.color = in["color"].as<int>();
    if (in.containsKey("level")) c.level = in["level"].as<int>();
    ledSet(c);
    DynamicJsonDocument d(1024);
    ledJson(d);
    json(200, d);
  });
  web.on("/api/display", HTTP_GET, []() {
    // Screen orientation / colour fixes for the on-board display, e.g.
    // /api/display?rotation=3  or  ?invert=1  or  ?rgb=1 (reboots to apply)
    if (web.args() && !localRequest()) return;
    Preferences p;
    p.begin("battlab-disp", false);
    bool changed = false;
    if (web.hasArg("rotation")) { p.putUChar("rot", web.arg("rotation").toInt() & 7); p.remove("cal"); changed = true; }
    if (web.hasArg("calibrate")) { p.remove("cal"); changed = true; }
    if (web.hasArg("invert")) { p.putBool("inv", web.arg("invert").toInt() != 0); changed = true; }
    if (web.hasArg("rgb")) { p.putBool("rgb", web.arg("rgb").toInt() != 0); changed = true; }
    String out = String("{\"rotation\":") + p.getUChar("rot", 1) +
                 ",\"invert\":" + (p.getBool("inv", false) ? "true" : "false") +
                 ",\"rgb\":" + (p.getBool("rgb", false) ? "true" : "false") +
                 ",\"rebooting\":" + (changed ? "true" : "false") + "}";
    p.end();
    web.send(200, "application/json", out);
    if (changed) {
      delay(300);
      ESP.restart();
    }
  });
  web.on("/api/probe", HTTP_GET, []() {
    String s;
    {
      Guard g;
      s = String("BATTLAB " BATTLAB_FW " probe log\nstate: ") + shared.probe +
          "\ngatt: " + shared.gatt + "\n\n" + shared.probeLog;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/plain; charset=utf-8", s);
  });
  web.on("/api/scan", HTTP_POST, []() {
    if (!localRequest()) return;
    bmsScan();
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.on("/api/devices", HTTP_GET, []() {
    String s;
    {
      Guard g;
      s = shared.devices;
    }
    web.send(200, "application/json", s);
  });
  web.on("/api/jk_codes", HTTP_GET, []() {
    if (!localRequest()) return;
    int pack = web.hasArg("a") ? web.arg("a").toInt() : -1;
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", jkCodesJson(pack));
  });
  web.on("/api/auth", HTTP_POST, []() {
    if (!localRequest()) return;
    if (web.arg("plain").length() > 256) { error(413,"Too large"); return; }
    StaticJsonDocument<384> d;
    if (deserializeJson(d,web.arg("plain"))) {error(400,"Invalid JSON");return;}
    if (!d["bluetooth_code"].is<const char*>() || !d["parameter_code"].is<const char*>()) {error(400,"Codes must be strings");return;}
    String bluetooth = d["bluetooth_code"].as<String>();
    String parameter = d["parameter_code"].as<String>();
    if (!jk::validCode(bluetooth.c_str()) || (parameter.length() && !jk::validCode(parameter.c_str()))) {error(400,"Use 1-16 printable ASCII characters");return;}
    { Guard g;
      cfg.bluetoothCode = bluetooth;
      cfg.parameterCode = parameter;
      shared.have = shared.haveParams = false;
      shared.connectionAuth = "WAITING_INFO";
      shared.parameterAuth = "LOCKED";
      configChanged = true;
      generation++;
    }
    StaticJsonDocument<128> result;
    result["accepted"] = true;
    result["verified"] = false;
    result["parameter_write_supported"] = false;
    json(202,result);
  });
  web.on("/api/connect", HTTP_POST, []() {
    if (!localRequest()) return;
    if (web.arg("plain").length() > 256) { error(413, "Too large"); return; }
    StaticJsonDocument<256> d;
    if (deserializeJson(d, web.arg("plain"))) { error(400, "Invalid JSON"); return; }
    String mac = d["mac"] | "", p = d["protocol"] | "";
    int type = d["type"] | 0;
    if (type < 0 || type > 1 || !bmsConnect(mac, uint8_t(type), p)) {
      error(400, "Invalid MAC, protocol or address type");
      return;
    }
    web.send(202, "application/json", "{\"saved\":true}");
  });
  web.on("/api/auto", HTTP_POST, []() {
    if (!localRequest()) return;
    bmsAuto();
    web.send(202, "application/json", "{\"automatic\":true}");
  });
  web.on("/api/disconnect", HTTP_POST, []() {
    if (!localRequest()) return;
    bmsDisconnect();
    web.send(202, "application/json", "{\"disconnected\":true}");
  });
  web.on("/api/read", HTTP_POST, []() {
    if (!localRequest())
      return;
    {
      Guard g;
      readWanted = true;
    }
    web.send(202, "application/json", "{\"queued\":true}");
  });
  web.onNotFound(
      []() { error(404, "Not found / BMS writes are not implemented"); });
  web.begin();
  xTaskCreatePinnedToCore(bleWorker, "ble", 12288, nullptr, 1, nullptr, 0);
  ledLoad();
  xTaskCreatePinnedToCore(ledTask, "led", 3072, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(cloudTask, "cloud", 10240, nullptr, 1, nullptr, 1);
#ifndef BATTLAB_DISPLAY
  xTaskCreatePinnedToCore(rs485Task, "rs485", 6144, nullptr, 1, nullptr, 1);
#endif
  displayStart();
}
void loop() {
  web.handleClient();
  Config saved;
  bool persist = false;
  {
    Guard g;
    if (persistWanted) {
      saved = cfg;
      persistWanted = false;
      persist = true;
    }
  }
  ledPersist();
  bleSetupTick();
  energyTick();
  static bool bootOk = false;
  if (!bootOk && millis() > 60000) {
    bootOk = true;
    Preferences b;
    b.begin("battlab-boot", false);
    b.putUChar("fails", 0);
    b.end();
  }
  if (persist) {
    prefs.putString("mac", saved.mac);
    prefs.putString("protocol", saved.protocol);
    prefs.putUChar("type", saved.type);
    prefs.putBool("auto", saved.automatic);
  }
  delay(2);
}
