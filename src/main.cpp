#include "jk_protocol.h"
#include "jk_auth.h"
#include "ble_channels.h"
#include "ui.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
WebServer web(80);
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
  String h = web.hostHeader();
  if (h != "192.168.4.1" && h != "192.168.4.1:80") {
    error(403, "Invalid Host");
    return false;
  }
  String o = web.header("Origin");
  if (o.length() && o != "http://192.168.4.1") {
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
    d["parameter_write_supported"] = false;
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
  d["uptime_ms"] = millis();
  d["firmware"] = "1.3.4-probe-rgb";
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
void bleWorker(void *) {
  NimBLEDevice::init("BATTLAB-JK");
  NimBLEDevice::setMTU(247);
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
  auto receive = [&](const uint8_t *b) {
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
    } else if (b[4] == 1 &&
               (c.protocol != "AUTO" || shared.detected.length())) {
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
            upper.startsWith("JK") ||
            (dev.haveServiceUUID() &&
             dev.isAdvertisingService(NimBLEUUID(uint16_t(0xffe0))));
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
          auto service = client->getService(NimBLEUUID(uint16_t(0xffe0)));
          ch = nullptr;
          NimBLERemoteCharacteristic *notifyCh = nullptr;
          if (service) {
            auto chars = service->getCharacteristics(true);
            if (chars) jk::selectChannels(*chars, ch, notifyCh,
              [](NimBLERemoteCharacteristic *candidate) {
                return candidate->getUUID() == NimBLEUUID(uint16_t(0xffe1));
              });
          }
          Serial.printf("[BLE] connected %s, service=%s tx=%s rx=%s mtu=%u\n",
                        c.mac.c_str(), service ? "FFE0" : "none",
                        ch ? "ok" : "none", notifyCh ? "ok" : "none",
                        (unsigned)client->getMTU());
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
          {
            Guard g;
            shared.gatt = gatt;
          }
          if (subscribed && !steps.empty()) {
            probeStep = 0;
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
            shared.connectionAuth = "WAITING_INFO";
            shared.parameterAuth = "LOCKED";
            shared.bleError = 0;
            shared.writeHandle = ch ? ch->getHandle() : 0;
            shared.probe = "probing";
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
    } else if (probeStep != -1 || ch) {
      Chunk chunk;
      while (xQueueReceive(queue, &chunk, 0) == pdTRUE)
        parser.feed(chunk.data, chunk.n, receive);
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
            probeStep = -2;
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
      bool probing = probeStep >= 0 || !ch;
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
        reset = now - since > (notifyBytes || probeStep != -1 ? 180000u : 45000u);
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
// Onboard RGB LED (WS2812). DevKitC-1 v1.0 uses GPIO48, v1.1 uses GPIO38.
#ifndef BATTLAB_LED_PIN
#define BATTLAB_LED_PIN 48
#endif
void ledTask(void *) {
  const uint8_t maxLevel = 40; // keep it soft, the LED is very bright
  float hue = 0;
  for (;;) {
    float h = hue / 60.f, x = 1.f - fabsf(fmodf(h, 2.f) - 1.f);
    float r = 0, g = 0, b = 0;
    switch (int(h) % 6) {
      case 0: r = 1; g = x; break;
      case 1: r = x; g = 1; break;
      case 2: g = 1; b = x; break;
      case 3: g = x; b = 1; break;
      case 4: r = x; b = 1; break;
      default: r = 1; b = x; break;
    }
    neopixelWrite(BATTLAB_LED_PIN, uint8_t(r * maxLevel),
                  uint8_t(g * maxLevel), uint8_t(b * maxLevel));
    hue += 0.6f; // full colour circle in about 12 s
    if (hue >= 360.f) hue -= 360.f;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
void setup() {
  Serial.begin(115200);
  lock = xSemaphoreCreateMutex();
  prefs.begin("battlab-jk", false);
  cfg.mac = prefs.getString("mac", "");
  cfg.protocol = prefs.getString("protocol", "AUTO");
  cfg.automatic = prefs.getBool("auto", true);
  cfg.type = prefs.getUChar("type", 0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  const bool apStarted = WiFi.softAP("BATTLAB-JK");
  Serial.printf("\nBATTLAB 1.3.4-probe-rgb\nWi-Fi: BATTLAB-JK (OPEN)\nAP started: %s\nhttp://192.168.4.1\n", apStarted ? "YES" : "NO");
  const char *headers[] = {"Origin"};
  web.collectHeaders(headers, 1);
  web.on("/", HTTP_GET,
         []() { web.send_P(200, "text/html; charset=utf-8", UI_HTML); });
  web.on("/api/state", HTTP_GET, stateApi);
  web.on("/api/probe", HTTP_GET, []() {
    String s;
    {
      Guard g;
      s = String("BATTLAB 1.3.4 probe log\nstate: ") + shared.probe +
          "\ngatt: " + shared.gatt + "\n\n" + shared.probeLog;
    }
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/plain; charset=utf-8", s);
  });
  web.on("/api/scan", HTTP_POST, []() {
    if (!localRequest())
      return;
    {
      Guard g;
      scanWanted = true;
    }
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
    if (!localRequest())
      return;
    if (web.arg("plain").length() > 256) {
      error(413, "Too large");
      return;
    }
    StaticJsonDocument<256> d;
    if (deserializeJson(d, web.arg("plain"))) {
      error(400, "Invalid JSON");
      return;
    }
    String mac = d["mac"] | "", p = d["protocol"] | "";
    int type = d["type"] | 0;
    if (!validMac(mac) || (p != "AUTO" && p != "JK02_24S" && p != "JK02_32S") ||
        (type != 0 && type != 1)) {
      error(400, "Invalid MAC, protocol or address type");
      return;
    }
    {
      Guard g;
      persistWanted = false;
      cfg.automatic = p == "AUTO";
      cfg.mac = mac;
      cfg.protocol = p;
      cfg.type = (uint8_t)type;
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
    prefs.putBool("auto", p == "AUTO");
    prefs.putString("mac", mac);
    prefs.putString("protocol", p);
    prefs.putUChar("type", type);
    web.send(202, "application/json", "{\"saved\":true}");
  });
  web.on("/api/auto", HTTP_POST, []() {
    if (!localRequest())
      return;
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
    prefs.putBool("auto", true);
    prefs.remove("mac");
    prefs.putString("protocol", "AUTO");
    web.send(202, "application/json", "{\"automatic\":true}");
  });
  web.on("/api/disconnect", HTTP_POST, []() {
    if (!localRequest())
      return;
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
    prefs.putBool("auto", false);
    prefs.remove("mac");
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
  xTaskCreatePinnedToCore(ledTask, "led", 3072, nullptr, 1, nullptr, 1);
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
  if (persist) {
    prefs.putString("mac", saved.mac);
    prefs.putString("protocol", saved.protocol);
    prefs.putUChar("type", saved.type);
    prefs.putBool("auto", saved.automatic);
  }
  delay(2);
}
