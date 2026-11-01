#include "bambu.h"
#include <string>

static StaticJsonDocument<2048> gFilter;
static DynamicJsonDocument *gDoc = nullptr;

void bambuInitFilter() {
  JsonObject p = gFilter.createNestedObject("print");
  for (const char *k : {"command", "gcode_state", "mc_percent", "mc_remaining_time", "layer_num", "total_layer_num",
                        "subtask_name", "print_error", "nozzle_temper", "nozzle_target_temper", "bed_temper",
                        "bed_target_temper", "chamber_temper", "cooling_fan_speed", "big_fan1_speed", "big_fan2_speed",
                        "spd_lvl", "wifi_signal"})
    p[k] = true;
  JsonObject h = p["hms"].createNestedObject();
  h["attr"] = true;
  h["code"] = true;
  JsonObject ams = p.createNestedObject("ams");
  ams["tray_now"] = true;
  JsonObject u = ams["ams"].createNestedObject();
  for (const char *k : {"id", "humidity", "humidity_raw", "temp", "dry_time"}) u[k] = true;
  JsonObject t = u["tray"].createNestedObject();
  for (const char *k : {"id", "tray_type", "tray_color", "remain", "tray_sub_brands"}) t[k] = true;
  // H2D / H2C / X2D style (dual nozzle, packed temperatures)
  JsonObject dev = p.createNestedObject("device");
  JsonObject ex = dev.createNestedObject("extruder");
  ex["state"] = true;
  JsonObject ei = ex["info"].createNestedObject();
  ei["id"] = true;
  ei["temp"] = true;
  dev["bed"]["info"]["temp"] = true;
  dev["ctc"]["info"]["temp"] = true;
  gDoc = new DynamicJsonDocument(16384);
}

// ---- reads at most `left` bytes from the TLS stream, waiting for data ----
class Bounded : public Stream {
 public:
  Bounded(Client &c, uint32_t n) : c_(c), left_(n) {}
  uint32_t left() const { return left_; }
  int available() override { return left_ ? (c_.available() > 0 ? 1 : 0) : 0; }
  int read() override {
    if (!left_) return -1;
    uint32_t t = millis();
    while (c_.available() <= 0) {
      if (!c_.connected() || millis() - t > 4000) return -1;
      delay(1);
    }
    int b = c_.read();
    if (b >= 0) left_--;
    return b;
  }
  size_t readBytes(char *buf, size_t len) override {
    size_t n = 0;
    while (n < len) {
      int b = read();
      if (b < 0) break;
      buf[n++] = (char)b;
    }
    return n;
  }
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 0; }

 private:
  Client &c_;
  uint32_t left_;
};

// ------------------------------------------------------------- helpers ----
static long toL(JsonVariantConst v, long def) {
  if (v.isNull()) return def;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    return (s && *s) ? strtol(s, nullptr, 10) : def;
  }
  if (v.is<float>()) return (long)v.as<float>();
  return v.as<long>();
}
static float toF(JsonVariantConst v, float def) {
  if (v.isNull()) return def;
  if (v.is<const char *>()) {
    const char *s = v.as<const char *>();
    return (s && *s) ? atof(s) : def;
  }
  return v.as<float>();
}
static int fanPct(JsonVariantConst v) {  // "0".."15" -> 0..100 in 10 % steps
  long f = toL(v, -1);
  if (f < 0) return -1;
  return (int)lroundf(f / 1.5f) * 10;
}
static void unpackTemp(long packed, float &cur, float &tgt) {
  // low 16 bits = current, high 16 bits = target
  float a = packed & 0xFFFF, b = (packed >> 16) & 0xFFFF;
  cur = a;
  tgt = b;
}

bool BambuPrinter::hasChamberSensor() const {
  String m = cfg.model;
  m.toUpperCase();
  return m.startsWith("X1") || m.startsWith("X2") || m.startsWith("H2") || m.startsWith("P2");
}

// ------------------------------------------------------------ lifecycle ----
void BambuPrinter::begin(const PrinterCfg &c) {
  stop();
  cfg = c;
  st = PState();
  lastErr = "";
  nextTryMs_ = millis() + 500;
  backoffMs_ = 3000;
}

void BambuPrinter::stop() {
  if (tls_.connected()) tls_.stop();
  connected = false;
}

void BambuPrinter::drop_(const char *why) {
  if (why) lastErr = why;
  tls_.stop();
  if (connected) urgent = true;
  connected = false;
  nextTryMs_ = millis() + backoffMs_;
  backoffMs_ = min<uint32_t>(backoffMs_ * 2, 60000);
}

void BambuPrinter::writePacket_(uint8_t hdr, const uint8_t *body, size_t len) {
  uint8_t h[5];
  size_t n = 0;
  h[n++] = hdr;
  size_t l = len;
  do {
    uint8_t d = l % 128;
    l /= 128;
    if (l) d |= 0x80;
    h[n++] = d;
  } while (l);
  tls_.write(h, n);
  if (len) tls_.write(body, len);
}

static void putStr(std::string &b, const String &s) {
  b.push_back((char)(s.length() >> 8));
  b.push_back((char)(s.length() & 255));
  b.append(s.c_str(), s.length());
}

bool BambuPrinter::readByte_(uint8_t &b, uint32_t timeoutMs) {
  uint32_t t = millis();
  while (tls_.available() <= 0) {
    if (!tls_.connected() || millis() - t > timeoutMs) return false;
    delay(1);
  }
  int v = tls_.read();
  if (v < 0) return false;
  b = (uint8_t)v;
  return true;
}

bool BambuPrinter::readLen_(uint32_t &len) {
  len = 0;
  uint32_t mul = 1;
  for (int i = 0; i < 4; i++) {
    uint8_t d;
    if (!readByte_(d, 4000)) return false;
    len += (d & 127) * mul;
    if (!(d & 128)) return true;
    mul *= 128;
  }
  return false;
}

bool BambuPrinter::skip_(uint32_t n) {
  uint8_t buf[128];
  while (n) {
    uint32_t t = millis();
    while (tls_.available() <= 0) {
      if (!tls_.connected() || millis() - t > 4000) return false;
      delay(1);
    }
    int r = tls_.read(buf, min<uint32_t>(n, sizeof buf));
    if (r <= 0) return false;
    n -= r;
  }
  return true;
}

bool BambuPrinter::connect_() {
  if (!cfg.ip.length() || !cfg.serial.length() || !cfg.code.length()) {
    lastErr = "not configured";
    nextTryMs_ = millis() + 10000;
    return false;
  }
  tls_.setInsecure();  // printers use a self-signed certificate
  tls_.setHandshakeTimeout(12);
  tls_.setTimeout(5);
  if (!tls_.connect(cfg.ip.c_str(), 8883)) {
    char e[80];
    tls_.lastError(e, sizeof e);
    drop_(strlen(e) ? (String("cannot connect: ") + e).c_str() : "cannot connect (check IP, LAN mode)");
    return false;
  }
  std::string b;
  b.append("\x00\x04MQTT\x04\xC2\x00\x3C", 10);
  putStr(b, String("bbridge-") + String((uint32_t)ESP.getEfuseMac(), HEX) + "-" + cfg.serial.substring(cfg.serial.length() - 4));
  putStr(b, "bblp");
  putStr(b, cfg.code);
  writePacket_(0x10, (const uint8_t *)b.data(), b.size());
  uint8_t h, l, f, rc;
  if (!readByte_(h, 6000) || h != 0x20 || !readByte_(l, 2000) || !readByte_(f, 2000) || !readByte_(rc, 2000)) {
    drop_("no MQTT answer (turn on LAN / Developer mode)");
    return false;
  }
  if (rc != 0) {
    drop_(rc == 4 || rc == 5 ? "wrong access code" : "MQTT refused");
    backoffMs_ = 60000;
    return false;
  }
  // subscribe device/<serial>/report
  b.clear();
  uint16_t id = pid_++;
  b.push_back((char)(id >> 8));
  b.push_back((char)(id & 255));
  putStr(b, "device/" + cfg.serial + "/report");
  b.push_back(0);
  writePacket_(0x82, (const uint8_t *)b.data(), b.size());
  connected = true;
  urgent = true;
  lastErr = "";
  backoffMs_ = 3000;
  lastRxMs = millis();
  lastPingMs_ = millis();
  requestPushall();
  return true;
}

void BambuPrinter::requestPushall() {
  if (!connected) return;
  std::string b;
  putStr(b, "device/" + cfg.serial + "/request");
  b += "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
  writePacket_(0x30, (const uint8_t *)b.data(), b.size());
  lastPushallMs_ = millis();
}

void BambuPrinter::loop() {
  if (!connected) {
    if ((int32_t)(millis() - nextTryMs_) >= 0) connect_();
    return;
  }
  if (!tls_.connected()) {
    drop_("connection lost");
    return;
  }
  int guard = 0;
  while (tls_.available() > 0 && guard++ < 8) {
    uint8_t h;
    uint32_t rem;
    if (!readByte_(h, 2000) || !readLen_(rem)) {
      drop_("read error");
      return;
    }
    lastRxMs = millis();
    if ((h & 0xF0) == 0x30) handlePublish_(rem, h & 0x0F);
    else if (!skip_(rem)) {
      drop_("read error");
      return;
    }
    if (!connected) return;
  }
  uint32_t now = millis();
  if (now - lastPingMs_ > 30000) {
    writePacket_(0xC0, nullptr, 0);
    lastPingMs_ = now;
  }
  if (now - lastRxMs > 95000) drop_("printer stopped answering");
  else if (now - lastPushallMs_ > 15UL * 60 * 1000) requestPushall();
}

void BambuPrinter::handlePublish_(uint32_t rem, uint8_t flags) {
  uint8_t a, b;
  if (!readByte_(a, 2000) || !readByte_(b, 2000)) return drop_("read error");
  uint16_t tlen = (a << 8) | b;
  if (tlen + 2 > rem || !skip_(tlen)) return drop_("read error");
  rem -= tlen + 2;
  if (flags & 0x06) {  // QoS 1/2 carries a packet id (printers send QoS 0)
    if (rem < 2 || !skip_(2)) return drop_("read error");
    rem -= 2;
  }
  Bounded in(tls_, rem);
  gDoc->clear();
  DeserializationError e = deserializeJson(*gDoc, in, DeserializationOption::Filter(gFilter),
                                           DeserializationOption::NestingLimit(12));
  uint32_t left = in.left();
  if (left && !skip_(left)) return drop_("read error");
  if (e && e != DeserializationError::NoMemory) {
    if (e == DeserializationError::IncompleteInput && !tls_.connected()) return drop_("connection lost");
    return;  // not JSON we care about
  }
  JsonObject p = (*gDoc)["print"];
  if (p.isNull()) return;
  merge_(p);
  reports++;
}

// -------------------------------------------------------------- merge ----
void BambuPrinter::merge_(JsonObject p) {
  PState &s = st;
  String oldG = s.gcode;
  uint32_t oldE = s.err;
  int oldH = s.nhms;
  s.have = true;
  if (p.containsKey("gcode_state")) s.gcode = p["gcode_state"].as<const char *>();
  if (p.containsKey("subtask_name")) s.task = p["subtask_name"].as<const char *>();
  if (p.containsKey("mc_percent")) s.pct = toL(p["mc_percent"], s.pct);
  if (p.containsKey("mc_remaining_time")) s.remain = toL(p["mc_remaining_time"], s.remain);
  if (p.containsKey("layer_num")) s.layer = toL(p["layer_num"], s.layer);
  if (p.containsKey("total_layer_num")) s.tlayer = toL(p["total_layer_num"], s.tlayer);
  if (p.containsKey("print_error")) s.err = (uint32_t)toL(p["print_error"], 0);
  if (p.containsKey("nozzle_temper")) s.noz = toF(p["nozzle_temper"], s.noz);
  if (p.containsKey("nozzle_target_temper")) s.nozT = toF(p["nozzle_target_temper"], s.nozT);
  if (p.containsKey("bed_temper")) s.bed = toF(p["bed_temper"], s.bed);
  if (p.containsKey("bed_target_temper")) s.bedT = toF(p["bed_target_temper"], s.bedT);
  if (p.containsKey("chamber_temper") && hasChamberSensor()) {
    float c = toF(p["chamber_temper"], -1000);
    if (c > 0 && c < 120) s.cham = c;
  }
  if (p.containsKey("cooling_fan_speed")) s.fanPart = fanPct(p["cooling_fan_speed"]);
  if (p.containsKey("big_fan1_speed")) s.fanAux = fanPct(p["big_fan1_speed"]);
  if (p.containsKey("big_fan2_speed")) s.fanCham = fanPct(p["big_fan2_speed"]);
  if (p.containsKey("spd_lvl")) s.spd = toL(p["spd_lvl"], s.spd);
  if (p.containsKey("wifi_signal")) s.wifi = toL(p["wifi_signal"], s.wifi);
  if (p.containsKey("hms")) {
    s.nhms = 0;
    for (JsonObject h : p["hms"].as<JsonArray>()) {
      uint32_t at = (uint32_t)toL(h["attr"], 0), co = (uint32_t)toL(h["code"], 0);
      if (s.nhms < 3) {
        char b[24];
        snprintf(b, sizeof b, "%04X_%04X_%04X_%04X", (unsigned)(at >> 16), (unsigned)(at & 0xFFFF), (unsigned)(co >> 16),
                 (unsigned)(co & 0xFFFF));
        s.hms[s.nhms] = b;
      }
      s.nhms++;
    }
    if (s.nhms > 3) s.nhms = 3;
  }
  // dual nozzle machines: device.extruder.info[{id, temp(packed)}], state bits 4..7 = active id
  JsonObject dev = p["device"];
  if (!dev.isNull()) {
    JsonObject ex = dev["extruder"];
    if (!ex.isNull()) {
      JsonArray info = ex["info"];
      int n = 0;
      for (JsonObject i : info) {
        int id = toL(i["id"], n);
        if (!i.containsKey("temp")) { n++; continue; }
        float c, t;
        unpackTemp(toL(i["temp"], 0), c, t);
        // id 0 = right (main) nozzle, id 1 = left
        if (id == 1) { s.noz = c; s.nozT = t; }
        else { s.noz2 = c; s.noz2T = t; }
        n++;
      }
      if (info.size() >= 2) s.dual = true;
      if (ex.containsKey("state")) {
        long stt = toL(ex["state"], 0);
        s.active = ((stt >> 4) & 0xF) == 1 ? 0 : 1;  // UI: 0 = left, 1 = right
      }
    }
    if (dev["bed"]["info"].containsKey("temp")) unpackTemp(toL(dev["bed"]["info"]["temp"], 0), s.bed, s.bedT);
    if (dev["ctc"]["info"].containsKey("temp")) {
      float c, t;
      unpackTemp(toL(dev["ctc"]["info"]["temp"], 0), c, t);
      if (c > 0 && c < 120) s.cham = c;
    }
  }
  // AMS
  JsonObject ams = p["ams"];
  if (!ams.isNull()) {
    if (ams.containsKey("tray_now")) s.trayNow = toL(ams["tray_now"], s.trayNow);
    if (ams.containsKey("ams")) {
      JsonArray units = ams["ams"];
      if (units.size() == 0) s.nams = 0;
      for (JsonObject u : units) {
        int id = toL(u["id"], 0);
        int k = -1;
        for (int i = 0; i < s.nams; i++)
          if (s.ams[i].id == id) k = i;
        if (k < 0) {
          if (s.nams >= 4) continue;
          k = s.nams++;
          s.ams[k] = AmsS();
          s.ams[k].id = id;
        }
        AmsS &a = s.ams[k];
        if (u.containsKey("humidity_raw")) a.hum = toL(u["humidity_raw"], a.hum);
        if (u.containsKey("humidity")) a.lvl = toL(u["humidity"], a.lvl);
        if (u.containsKey("temp")) a.temp = toF(u["temp"], a.temp);
        if (u.containsKey("dry_time")) a.dry = toL(u["dry_time"], a.dry);
        if (u.containsKey("tray")) {
          JsonArray trays = u["tray"];
          if (trays.size() == 1) a.ntray = 1;  // AMS HT has one slot
          int n = 0;
          for (JsonObject t : trays) {
            int tid = toL(t["id"], n++);
            if (tid < 0 || tid > 3) continue;
            TrayS &x = a.tray[tid];
            if (t.size() <= 1) {  // only "id": slot is empty
              x = TrayS();
              continue;
            }
            if (t.containsKey("tray_type")) {
              x.type = t["tray_type"].as<const char *>();
              x.present = x.type.length() > 0;
            }
            if (t.containsKey("tray_color")) x.color = String(t["tray_color"].as<const char *>()).substring(0, 6);
            if (t.containsKey("remain")) x.remain = toL(t["remain"], x.remain);
          }
          if (trays.size() > 1) a.ntray = 4;
        }
      }
    }
  }
  dirty = true;
  if (s.gcode != oldG || s.err != oldE || s.nhms != oldH) urgent = true;
}

void BambuPrinter::toJson(JsonObject o) const {
  o["serial"] = cfg.serial;
  o["name"] = cfg.name.length() ? cfg.name : cfg.serial;
  o["model"] = cfg.model;
  o["connected"] = connected && st.have;
  JsonObject s = o.createNestedObject("state");
  if (!st.have) return;
  s["gcode_state"] = st.gcode;
  s["subtask_name"] = st.task;
  s["mc_percent"] = st.pct;
  s["remain"] = st.remain;
  s["layer"] = st.layer;
  s["tlayer"] = st.tlayer;
  s["print_error"] = st.err;
  JsonArray n = s.createNestedArray("noz");
  n.add(roundf(st.noz));
  n.add(roundf(st.nozT));
  if (st.dual) {
    JsonArray n2 = s.createNestedArray("noz2");
    n2.add(roundf(st.noz2));
    n2.add(roundf(st.noz2T));
    s["active"] = st.active;
  }
  JsonArray b = s.createNestedArray("bed");
  b.add(roundf(st.bed));
  b.add(roundf(st.bedT));
  if (st.cham > -999) s["cham"] = roundf(st.cham);
  if (st.fanPart >= 0) s["fan_part"] = st.fanPart;
  if (st.fanAux >= 0) s["fan_aux"] = st.fanAux;
  if (st.fanCham >= 0) s["fan_cham"] = st.fanCham;
  s["spd"] = st.spd;
  if (st.wifi) s["wifi"] = st.wifi;
  JsonArray h = s.createNestedArray("hms");
  for (int i = 0; i < st.nhms; i++) h.add(st.hms[i]);
  s["tray_now"] = st.trayNow;
  JsonArray am = s.createNestedArray("ams");
  for (int i = 0; i < st.nams; i++) {
    const AmsS &a = st.ams[i];
    JsonObject u = am.createNestedObject();
    u["id"] = a.id;
    if (a.hum >= 0) u["hum"] = a.hum;
    if (a.lvl >= 0) u["lvl"] = a.lvl;
    if (a.temp > -999) u["temp"] = roundf(a.temp * 10) / 10;
    if (a.dry > 0) u["dry"] = a.dry;
    JsonArray tr = u.createNestedArray("trays");
    for (int k = 0; k < a.ntray; k++) {
      const TrayS &t = a.tray[k];
      if (!t.present) { tr.add(nullptr); continue; }
      JsonObject x = tr.createNestedObject();
      x["t"] = t.type;
      x["c"] = t.color.length() ? t.color : String("808080");
      if (t.remain >= 0) x["r"] = t.remain;
    }
  }
}
