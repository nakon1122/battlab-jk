#include "jk_protocol.h"
#include "bms_other.h"
#include "bms_params.h"
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
#include "cloud_config.h"
#include "cloud_ca.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#ifdef BATTLAB_CYD35
#define BATTLAB_FW "2.0.0-cyd35"
#else
#define BATTLAB_FW "2.0.0-s3"
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
  uint8_t family = 0;       // 0 unknown, 1 JK, 2 ANT, 3 JBD
  String lastWrite;         // result of the last BMS write, for the UI
  bool probeLogFrozen = false;
  String state = "UNCONFIGURED", devices = "[]";
} shared;
bool scanWanted = false, configChanged = false, readWanted = false;
uint32_t generation = 0;
bool persistWanted = false;
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
static uint8_t cmdN = 0;
static const char *queueCmd(const BmsCmd &c) {
  Guard g;
  if (!shared.linked) return "BMS not connected";
  if (cmdN >= 4) return "busy, try again";
  cmdQ[cmdN++] = c;
  return nullptr;
}
// Returns nullptr when queued, otherwise the reason it was refused.
const char *bmsWriteParam(int idx, float value) {
  {
    Guard g;
    if (shared.family != 1) return "parameter editing is JK only";
    if (uint32_t(millis() - shared.jkStamp) > 600000 || !shared.jk.valid)
      return "settings not read yet";
    if (const char *e = bmsw::checkJkValue(shared.jk, idx, value)) return e;
  }
  return queueCmd({false, uint8_t(idx), value, false});
}
const char *bmsSetSwitch(int which, bool on) {
  if (which < 0 || which >= bmsw::SW_COUNT) return "unknown switch";
  {
    Guard g;
    if (!shared.family) return "BMS type not identified yet";
    if (shared.family == 3 && which == bmsw::SW_BALANCER) return "not available on JBD";
  }
  return queueCmd({true, uint8_t(which), 0, on});
}

void bleWorker(void *) {
#ifdef BATTLAB_CYD35
  // Start the Bluetooth radio after Wi-Fi and the boot splash have settled,
  // so both radios never ramp up at the same moment (current spike).
  vTaskDelay(pdMS_TO_TICKS(5500));
#endif
  NimBLEDevice::init("BATTLAB");
  NimBLEDevice::setMTU(247);
  applyBleTxPower();
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
  uint8_t cellRequests = 0;
  // BMS family: 0 = not identified yet, 1 = JK, 2 = ANT, 3 = JBD/Xiaoxiang
  uint8_t fam = 0;
  bool linkedLocal = false;
  NimBLERemoteCharacteristic *antCh = nullptr, *jbdCh = nullptr;
  bms::AntFramer antF;
  bms::JbdFramer jbdF;
  jk::Sample jbdSample;
  bool jbdHaveBasic = false;
  uint32_t identAt = 0, identStartedAt = 0, lastOtherPoll = 0;
  uint8_t identIdx = 0, jbdToggle = 0;
  bool fullProbeDone = false;
  auto forcedFamily = [](const String &p) -> uint8_t {
    if (p == "ANT") return 2;
    if (p == "JBD") return 3;
    if (p == "JK02_24S" || p == "JK02_32S") return 1;
    return 0;
  };
  auto receive = [&](const uint8_t *b) {
    if (!fam && (b[4] == 2 || b[4] == 3)) fam = 1;
    Guard g;
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
  for (;;) {
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
        cmdN = 0; // never replay a queued write onto a new connection
      }
      steps.clear();
      probeStep = -1;
      if (ch)
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
          Serial.printf("[BLE] GATT: %s\n", gatt.c_str());
          bool subscribed = false;
          steps.clear();
          probeStep = -1;
          if (auto svcs = client->getServices(false))
            for (auto sv : *svcs) {
              if (sv->getUUID() == NimBLEUUID(uint16_t(0x1800)) ||
                  sv->getUUID() == NimBLEUUID(uint16_t(0x1801)))
                continue;
              auto cs = sv->getCharacteristics(false);
              if (!cs) continue;
              for (auto cc : *cs) {
                if (cc->canNotify() || cc->canIndicate()) {
                  bool ok = false;
                  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
                    ok = cc->canNotify() ? cc->subscribe(true, notify)
                                         : cc->subscribe(false, notify);
                    if (!ok) delay(200);
                  }
                  Serial.printf("[BLE] subscribe %s -> %s\n",
                                cc->getUUID().toString().c_str(), ok ? "ok" : "FAILED");
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
          fam = forcedFamily(c.protocol);
          if (!fam && jbdCh) fam = 3; // service FF00 is specific to JBD
          antF.reset();
          jbdF.reset();
          jbdHaveBasic = false;
          jbdSample = jk::Sample();
          identAt = 0;
          identIdx = 0;
          identStartedAt = millis();
          fullProbeDone = false;
          lastOtherPoll = 0;
          Serial.printf("[BLE] channels: jk/ant=%s jbd=%s family=%u\n",
                        ch ? "FFE1" : "-", jbdCh ? "FF02" : "-", fam);
          {
            Guard g;
            shared.gatt = gatt;
          }
          if (subscribed && (ch || jbdCh || !steps.empty())) {
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
      Chunk chunk;
      while (xQueueReceive(queue, &chunk, 0) == pdTRUE) {
        parser.feed(chunk.data, chunk.n, receive);
        antF.feed(chunk.data, chunk.n, [&](const uint8_t *f, size_t n) {
          jk::Sample smp;
          if (bms::decodeAnt(f, n, smp)) {
            if (!fam) fam = 2;
            if (fam == 2) publishOther(smp, "ANT");
          } else {
            Guard g;
            shared.rejected++;
          }
        });
        jbdF.feed(chunk.data, chunk.n, [&](uint8_t cmd, const uint8_t *d, size_t n) {
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
          } else if (fam == 3 && cmd.isSwitch) {
            bool chg = jbdSample.charge, dsg = jbdSample.discharge;
            if (cmd.idx == 0) chg = cmd.on;
            if (cmd.idx == 1) dsg = cmd.on;
            auto f = bmsw::jbdMosFrame(chg, dsg);
            NimBLERemoteCharacteristic *w = jbdCh ? jbdCh : ch;
            if (w) ok = w->writeValue(f.data(), f.size(), !w->canWriteNoResponse());
            result = String(cmd.idx == 0 ? "Charge" : "Discharge") + (cmd.on ? " ON" : " OFF");
          } else {
            result = "not supported for this BMS";
          }
          Serial.printf("[BMS] write %s -> %s\n", result.c_str(), ok ? "sent" : "FAILED");
          Guard g;
          shared.lastWrite = (ok ? "Sent: " : "Failed: ") + result;
        }
        if (fam == 2 && t - lastOtherPoll > 2000) {
          writeRaw(antCh, bms::ANT_STATUS_REQUEST, sizeof(bms::ANT_STATUS_REQUEST), "ANT status");
          lastOtherPoll = t;
        } else if (fam == 3 && t - lastOtherPoll > 1000) {
          jbdToggle ^= 1;
          if (jbdToggle || !jbdHaveBasic)
            writeRaw(jbdCh ? jbdCh : ch, bms::JBD_BASIC_REQUEST, 7, "JBD basic");
          else
            writeRaw(jbdCh ? jbdCh : ch, bms::JBD_CELLS_REQUEST, 7, "JBD cells");
          lastOtherPoll = t;
        } else if (fam == 0 && probeStep == -1) {
          // Quick identification: rotate JK / ANT / JBD requests.
          uint32_t gap = fullProbeDone ? 3000 : 1200;
          if (!identAt || t - identAt > gap) {
            identAt = t;
            auto j7 = jk::request(0x97), j6 = jk::request(0x96);
            switch (identIdx++ % 4) {
              case 0: writeRaw(ch, j7.data(), j7.size(), "ident JK 0x97"); break;
              case 1: writeRaw(antCh, bms::ANT_STATUS_REQUEST, 10, "ident ANT"); break;
              case 2: writeRaw(jbdCh ? jbdCh : ch, bms::JBD_BASIC_REQUEST, 7, "ident JBD"); break;
              default: writeRaw(ch, j6.data(), j6.size(), "ident JK 0x96"); break;
            }
            {
              Guard g;
              shared.probe = fullProbeDone ? "identifying (slow)" : "identifying";
            }
            if (!fullProbeDone && t - identStartedAt > 20000 && !steps.empty()) {
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
      if (!probing && (firstRequest || streamStalled || wantRead)) {
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
    WiFi.softAP("BATTLAB");
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
void netScanStart() {
  wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_OFF) WiFi.mode(WIFI_STA);
  else if (m == WIFI_AP) WiFi.mode(WIFI_AP_STA); // keep the hotspot running
  WiFi.scanNetworks(true); // async
}
// JSON array of nearby networks once a scan finished, "" while scanning.
String netScanJson() {
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
NetInfo netInfo() {
  NetInfo n;
  n.ssid = staSsid;
  n.connected = WiFi.status() == WL_CONNECTED;
  if (n.connected) {
    n.ip = WiFi.localIP().toString();
    n.rssi = WiFi.RSSI();
  }
  n.pair = pairCode;
  n.site = CLOUD_SITE;
  n.configured = strlen(CLOUD_URL) > 0;
  Guard g;
  n.cloud = cloudStatus;
  n.paired = cloudPaired;
  n.lastOkAgo = cloudOkAt ? (millis() - cloudOkAt) / 1000 : -1;
  n.sent = cloudSent;
  return n;
}

static void cloudTask(void *) {
  WiFiClientSecure tls;
  tls.setCACert(CLOUD_CA);
  tls.setTimeout(12);
  HTTPClient http;
  http.setReuse(true);
  uint32_t lastSend = 0;
  auto setStatus = [](const String &s) { Guard g; cloudStatus = s; };
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!strlen(CLOUD_URL)) { setStatus("not set up"); continue; }
    if (!staSsid.length()) { setStatus("no home Wi-Fi"); continue; }
    if (WiFi.status() != WL_CONNECTED) { setStatus("Wi-Fi connecting"); continue; }
    bool live;
    DynamicJsonDocument body(3072);
    {
      Guard g;
      live = shared.linked && shared.have && uint32_t(millis() - shared.stamp) < 15000;
    }
    uint32_t every = live ? 10000 : 60000; // heartbeat when no BMS data
    if (lastSend && millis() - lastSend < every) continue;
    lastSend = millis();
    body["p_token"] = devToken;
    body["p_pair"] = pairCode;
    auto d = body.createNestedObject("p_data");
    {
      Guard g;
      d["fw"] = BATTLAB_FW;
      d["state"] = shared.state;
      d["bms"] = shared.detected;
      d["model"] = shared.model;
      d["rssi"] = shared.rssi;
      d["up"] = millis() / 1000;
      if (live) {
        const auto &s = shared.sample;
        d["v"] = s.voltage;
        d["i"] = s.current;
        d["p"] = s.voltage * s.current;
        d["soc"] = s.soc;
        d["cap"] = s.capacity;
        d["cyc"] = s.cycles;
        d["t1"] = s.temp1;
        d["t2"] = s.temp2;
        d["mos"] = s.mos;
        d["chg"] = s.charge;
        d["dsg"] = s.discharge;
        float mn = 9, mx = 0;
        auto c = d.createNestedArray("cells");
        for (int k = 0; k < 32; k++)
          if (s.cells[k] > 0) {
            c.add(roundf(s.cells[k] * 1000) / 1000);
            mn = min(mn, s.cells[k]);
            mx = max(mx, s.cells[k]);
          }
        if (mx > 0) { d["cmin"] = mn; d["cmax"] = mx; }
      }
    }
    String json;
    serializeJson(body, json);
    setStatus("sending");
    if (!http.begin(tls, String(CLOUD_URL) + "/rest/v1/rpc/ingest")) {
      setStatus("connect failed");
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", CLOUD_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + CLOUD_ANON_KEY);
    int code = http.POST(json);
    String resp = code > 0 ? http.getString() : String();
    http.end();
    Guard g;
    if (code == 200) {
      cloudSent++;
      cloudOkAt = millis();
      cloudPaired = resp.indexOf("unpaired") < 0;
      cloudStatus = cloudPaired ? "online" : "online - add on website";
    } else if (code < 0) {
      cloudStatus = String("TLS/net error ") + code;
      tls.stop();
    } else {
      cloudStatus = String("server error ") + code;
      Serial.printf("[CLOUD] %d %s\n", code, resp.c_str());
    }
  }
}

void bmsScan() {
  Guard g;
  scanWanted = true;
}
bool bmsConnect(const String &mac, uint8_t type, const String &p) {
  if (!validMac(mac) ||
      (p != "AUTO" && p != "JK02_24S" && p != "JK02_32S" && p != "ANT" && p != "JBD") ||
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
void setup() {
  Serial.begin(115200);
  readResetReason();
  lock = xSemaphoreCreateMutex();
  prefs.begin("battlab-jk", false);
  cfg.mac = prefs.getString("mac", "");
  cfg.protocol = prefs.getString("protocol", "AUTO");
  cfg.automatic = prefs.getBool("auto", true);
  cfg.type = prefs.getUChar("type", 0);
#ifdef BATTLAB_DISPLAY
  wifiOn = prefs.getBool("wifi", true); // can be switched off on the screen
  radioMax = prefs.getBool("txmax", true);
#endif
  netLoad();
  wifiApply();
  bool apStarted = wifiOn;
  Serial.printf("\nLast reset: %s\n", resetReasonText);
  Serial.printf("\nBATTLAB " BATTLAB_FW "\nWi-Fi: BATTLAB (OPEN)\nAP started: %s\nhttp://192.168.4.1\n", apStarted ? "YES" : "NO");
  const char *headers[] = {"Origin"};
  web.collectHeaders(headers, 1);
  web.on("/", HTTP_GET,
         []() { web.send_P(200, "text/html; charset=utf-8", UI_HTML); });
  web.on("/api/state", HTTP_GET, stateApi);
  web.on("/api/bms/params", HTTP_GET, []() {
    DynamicJsonDocument d(4096);
    {
      Guard g;
      d["family"] = shared.family;
      d["valid"] = shared.jk.valid;
      d["last_write"] = shared.lastWrite;
      auto a = d.createNestedArray("params");
      for (int i = 0; i < bmsw::JK_PARAM_COUNT; i++) {
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
  if (persist) {
    prefs.putString("mac", saved.mac);
    prefs.putString("protocol", saved.protocol);
    prefs.putUChar("type", saved.type);
    prefs.putBool("auto", saved.automatic);
  }
  delay(2);
}
