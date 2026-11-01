#pragma once
// One Bambu Lab printer on the LAN: MQTT over TLS (port 8883, user "bblp",
// password = LAN access code). Small hand-written MQTT 3.1.1 client that
// parses each report straight from the TLS stream through an ArduinoJson
// filter, so even a 30 KB "pushall" never needs a 30 KB buffer.
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

struct TrayS {
  String type, color;
  int remain = -1;
  bool present = false;
};
struct AmsS {
  int id = 0, hum = -1, lvl = -1, dry = -1;
  float temp = -1000;
  int ntray = 4;
  TrayS tray[4];
};
struct PState {
  bool have = false;  // got at least one report
  String gcode, task;
  int pct = 0, remain = 0, layer = 0, tlayer = 0;
  uint32_t err = 0;
  float noz = 0, nozT = 0, noz2 = 0, noz2T = 0, bed = 0, bedT = 0, cham = -1000;
  bool dual = false;
  int active = 0;
  int fanPart = -1, fanAux = -1, fanCham = -1, spd = 2, wifi = 0;
  int nhms = 0;
  String hms[3];
  int nams = 0;
  AmsS ams[4];
  int trayNow = 255;
};

struct PrinterCfg {
  String name, model, ip, serial, code;
};

class BambuPrinter {
 public:
  PrinterCfg cfg;
  PState st;
  bool connected = false;
  String lastErr;            // shown on the web page
  uint32_t reports = 0;      // messages parsed
  uint32_t lastRxMs = 0;
  volatile bool dirty = false;      // something changed since last cloud push
  volatile bool urgent = false;     // state / error / link changed
  bool hasChamberSensor() const;

  void begin(const PrinterCfg &c);
  void stop();
  void loop();               // call often
  void requestPushall();
  void toJson(JsonObject o) const;

 private:
  WiFiClientSecure tls_;
  uint32_t nextTryMs_ = 0, backoffMs_ = 3000, lastPingMs_ = 0, lastPushallMs_ = 0;
  uint16_t pid_ = 1;
  bool connect_();
  void drop_(const char *why);
  bool readByte_(uint8_t &b, uint32_t timeoutMs);
  bool readLen_(uint32_t &len);
  bool skip_(uint32_t n);
  void writePacket_(uint8_t hdr, const uint8_t *body, size_t len);
  void handlePublish_(uint32_t rem, uint8_t flags);
  void merge_(JsonObject print);
};

void bambuInitFilter();
