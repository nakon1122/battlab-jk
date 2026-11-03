// LuxPower Wi-Fi dongle, read directly over the home network (see include/lux_local.h).
#include "lux_dongle.h"
#include "lux_local.h"
#include <WiFi.h>
#include <Preferences.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace luxd {

static const int MAXD = 2, MAXI = 4;
static const uint16_t PORT = 8000;

struct Dongle {
  String ip;
  WiFiClient cli;
  luxl::Splitter sp;
  bool conn = false;
  uint32_t nextTry = 0, lastRx = 0, lastAsk = 0, frames = 0, connAt = 0;
  uint8_t askStep = 0;
  char dl[11] = {0};
  String st = "off", err;
};
struct Slot {
  luxl::Inv x;
  int8_t dongle = -1;
};

static SemaphoreHandle_t mtx;
struct G { G() { xSemaphoreTake(mtx, portMAX_DELAY); } ~G() { xSemaphoreGive(mtx); } };
static Dongle dg[MAXD];
static Slot inv[MAXI];
static bool on = false;
static volatile bool scanWanted = false, cfgChanged = false;
static bool scanBusy = false;
static String scanFound = "[]", scanNote;
static uint32_t scanAt = 0;

static void load() {
  Preferences p;
  p.begin("battlab-lux", true);
  String ips = p.getString("ips", "");
  on = p.getBool("on", false);
  p.end();
  int k = 0, from = 0;
  for (int i = 0; i < MAXD; i++) dg[i].ip = "";
  while (k < MAXD && from <= (int)ips.length()) {
    int c = ips.indexOf(',', from);
    if (c < 0) c = ips.length();
    String s = ips.substring(from, c);
    s.trim();
    IPAddress a;
    if (s.length() && a.fromString(s)) dg[k++].ip = s;
    from = c + 1;
  }
}

bool enabled() { G g; return on; }

void configure(const String &ips, bool en) {
  Preferences p;
  p.begin("battlab-lux", false);
  p.putString("ips", ips.substring(0, 64));
  p.putBool("on", en);
  p.end();
  cfgChanged = true;
}

void scan() { scanWanted = true; }

static Slot *slotFor(const char *sn, int d) {
  for (auto &s : inv) if (s.x.sn[0] && !strcmp(s.x.sn, sn)) return &s;
  for (auto &s : inv) if (!s.x.sn[0]) { s = Slot(); s.dongle = d; return &s; }
  // all used: take the oldest
  Slot *o = &inv[0];
  for (auto &s : inv) if (s.x.at < o->x.at) o = &s;
  *o = Slot(); o->dongle = d;
  return o;
}

static void drop(Dongle &d, const char *why) {
  d.cli.stop();
  d.conn = false;
  d.sp.n = 0;
  d.err = why;
  d.st = "retry";
  d.nextTry = millis() + 15000;
}

static void ask(Dongle &d, int di) {
  char sn[11] = {0};
  {
    G g;
    for (auto &s : inv) if (s.dongle == di && s.x.sn[0]) { memcpy(sn, s.x.sn, 11); break; }
  }
  uint8_t f[38];
  uint16_t reg = d.askStep == 0 ? 0 : 40;
  luxl::buildReadInput(f, d.dl[0] ? d.dl : nullptr, sn[0] ? sn : nullptr, reg, 40);
  d.cli.write(f, sizeof f);
}

static void serviceDongle(int di) {
  Dongle &d = dg[di];
  uint32_t now = millis();
  if (!d.conn) {
    if (int32_t(now - d.nextTry) < 0) return;
    IPAddress a;
    if (!a.fromString(d.ip)) { d.st = "bad ip"; d.nextTry = now + 60000; return; }
    { G g; d.st = "connecting"; }
    if (!d.cli.connect(a, PORT, 3000)) {
      G g;
      d.err = "port 8000 closed / no answer";
      d.st = "retry";
      d.nextTry = millis() + 15000;
      return;
    }
    d.cli.setNoDelay(true);
    G g;
    d.conn = true;
    d.connAt = d.lastRx = millis();
    d.lastAsk = 0;
    d.askStep = 0;
    d.st = "connected";
    d.err = "";
    return;
  }
  if (!d.cli.connected()) { G g; drop(d, "dongle closed the connection"); return; }
  uint8_t b[128];
  int k;
  while ((k = d.cli.available()) > 0) {
    int r = d.cli.read(b, k > (int)sizeof b ? sizeof b : k);
    if (r <= 0) break;
    d.sp.push(b, r);
    size_t n;
    while ((n = d.sp.frame())) {
      luxl::Inv tmp;
      char dl[11] = {0};
      luxl::Kind kd = luxl::decode(d.sp.buf, n, tmp, dl);
      G g;
      if (kd != luxl::BAD) { d.lastRx = millis(); d.frames++; }
      if (kd != luxl::BAD && luxl::validSerial(dl)) memcpy(d.dl, dl, 11);
      if (kd == luxl::INPUT && luxl::validSerial(tmp.sn)) {
        Slot *s = slotFor(tmp.sn, di);
        s->dongle = di;
        memcpy(s->x.dl, tmp.dl, 11);
        memcpy(s->x.sn, tmp.sn, 11);
        for (int i = 0; i < 80; i++) {
          bool lo = i < 40;
          // only copy the block that came in (decode() fills only those registers)
          if ((lo && (tmp.have & 1)) || (!lo && (tmp.have & 2))) s->x.r[i] = tmp.r[i];
        }
        s->x.have |= tmp.have;
        s->x.at = millis();
        d.st = "live";
      }
      d.sp.drop(n);
    }
  }
  now = millis();
  // ask for registers 0-39 then 40-79 every 10 s
  if (d.askStep == 0 && (!d.lastAsk || now - d.lastAsk > 10000)) { ask(d, di); d.lastAsk = now; d.askStep = 1; }
  else if (d.askStep == 1 && now - d.lastAsk > 800) { ask(d, di); d.askStep = 0; }
  if (now - d.lastRx > 45000) { G g; drop(d, "no data from dongle for 45 s"); }
}

// non-blocking connect to every host of the /24 on port 8000, 24 at a time
static void doScan() {
  { G g; scanBusy = true; scanNote = "scanning"; }
  String found = "[";
  int nf = 0;
  IPAddress me = WiFi.localIP();
  if (WiFi.status() != WL_CONNECTED || (uint32_t)me == 0) {
    G g; scanBusy = false; scanNote = "not on home Wi-Fi"; return;
  }
  for (int start = 1; start < 255; start += 24) {
    int fds[24], hosts[24], n = 0;
    for (int h = start; h < start + 24 && h < 255; h++) {
      if (h == me[3]) continue;
      int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
      if (s < 0) break;
      int fl = lwip_fcntl(s, F_GETFL, 0);
      lwip_fcntl(s, F_SETFL, fl | O_NONBLOCK);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(PORT);
      a.sin_addr.s_addr = (uint32_t)IPAddress(me[0], me[1], me[2], h);
      lwip_connect(s, (sockaddr *)&a, sizeof a);
      fds[n] = s; hosts[n] = h; n++;
    }
    uint32_t t0 = millis();
    bool done[24] = {false};
    while (millis() - t0 < 700) {
      fd_set w; FD_ZERO(&w);
      int mx = -1, left = 0;
      for (int i = 0; i < n; i++) if (!done[i]) { FD_SET(fds[i], &w); if (fds[i] > mx) mx = fds[i]; left++; }
      if (!left) break;
      timeval tv{0, 100000};
      if (lwip_select(mx + 1, nullptr, &w, nullptr, &tv) <= 0) continue;
      for (int i = 0; i < n; i++)
        if (!done[i] && FD_ISSET(fds[i], &w)) {
          done[i] = true;
          int e = 0; socklen_t l = sizeof e;
          lwip_getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &e, &l);
          if (!e && nf < 8) {
            if (nf++) found += ",";
            found += "\"" + IPAddress(me[0], me[1], me[2], hosts[i]).toString() + "\"";
          }
        }
    }
    for (int i = 0; i < n; i++) lwip_close(fds[i]);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  found += "]";
  G g;
  scanFound = found;
  scanBusy = false;
  scanAt = millis();
  scanNote = nf ? "found" : "no device with port 8000";
}

static void task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (cfgChanged) {
      cfgChanged = false;
      G g;
      for (auto &d : dg) { d.cli.stop(); d.conn = false; d.sp.n = 0; d.nextTry = 0; d.st = "off"; d.err = ""; }
      for (auto &s : inv) s = Slot();
      load();
    }
    if (scanWanted) { scanWanted = false; doScan(); }
    bool en;
    { G g; en = on; }
    if (!en || WiFi.status() != WL_CONNECTED) {
      for (auto &d : dg) if (d.conn) { G g; drop(d, en ? "Wi-Fi down" : "off"); d.st = en ? "retry" : "off"; }
      continue;
    }
    for (int i = 0; i < MAXD; i++) if (dg[i].ip.length()) serviceDongle(i);
  }
}

void start() {
  mtx = xSemaphoreCreateMutex();
  load();
  xTaskCreatePinnedToCore(task, "lux", 6144, nullptr, 1, nullptr, 1);
}

bool fresh() {
  G g;
  if (!on) return false;
  for (auto &s : inv) if (s.x.sn[0] && (s.x.have & 1) && millis() - s.x.at < 60000) return true;
  return false;
}

static void putVals(JsonObject o, const luxl::Inv &x) {
  luxl::Values v = luxl::values(x);
  o["soc"] = v.soc;
  o["v"] = v.v;
  o["p"] = v.p;
  if (v.v > 1) o["i"] = roundf(v.p / v.v * 100) / 100;
  o["ppv"] = v.ppv; o["ppv1"] = v.ppv1; o["ppv2"] = v.ppv2;
  o["pload"] = v.pload; o["pgrid"] = v.pgrid; o["peps"] = v.peps;
  o["pinv"] = v.pinv; o["prec"] = v.prec;
  o["vac"] = v.vac; o["vpv1"] = v.vpv1;
  o["epv"] = serialized(String(v.epv, 1));
  o["ein"] = serialized(String(v.ein, 1));
  o["eout"] = serialized(String(v.eout, 1));
  o["eload"] = serialized(String(v.eload, 1));
  o["egrid"] = serialized(String(v.egrid, 1));
  if (v.t1 > -100) o["t1"] = v.t1;
  if (v.mos > -100) o["mos"] = v.mos;
  o["status"] = luxl::statusText(v.status);
  o["scode"] = v.status;
}

void cloudJson(JsonArray arr) {
  G g;
  if (!on) return;
  uint32_t now = millis();
  for (auto &s : inv) {
    if (!s.x.sn[0] || !(s.x.have & 1) || now - s.x.at > 60000) continue;
    auto o = arr.createNestedObject();
    o["sn"] = s.x.sn;
    o["dl"] = s.x.dl;
    o["age"] = (now - s.x.at) / 1000;
    putVals(o, s.x);
  }
}

void statusJson(JsonObject o) {
  G g;
  o["on"] = on;
  auto ips = o.createNestedArray("ips");
  auto ds = o.createNestedArray("dongles");
  uint32_t now = millis();
  for (auto &d : dg) {
    if (!d.ip.length()) continue;
    ips.add(d.ip);
    auto x = ds.createNestedObject();
    x["ip"] = d.ip;
    x["st"] = d.st;
    if (d.err.length()) x["err"] = d.err;
    x["dl"] = d.dl;
    x["frames"] = d.frames;
    if (d.lastRx) x["rxago"] = (now - d.lastRx) / 1000;
  }
  auto iv = o.createNestedArray("inv");
  for (auto &s : inv) {
    if (!s.x.sn[0]) continue;
    auto x = iv.createNestedObject();
    x["sn"] = s.x.sn;
    x["dl"] = s.x.dl;
    x["age"] = (now - s.x.at) / 1000;
    if (s.x.have & 1) putVals(x, s.x);
  }
  auto sc = o.createNestedObject("scan");
  sc["busy"] = scanBusy || scanWanted;
  sc["note"] = scanNote;
  sc["found"] = serialized(scanFound);
}

int views(View *out, int max) {
  G g;
  int n = 0;
  uint32_t now = millis();
  for (auto &s : inv) {
    if (n >= max || !s.x.sn[0] || !(s.x.have & 1)) continue;
    luxl::Values v = luxl::values(s.x);
    View &w = out[n++];
    memcpy(w.sn, s.x.sn, 11);
    w.fresh = now - s.x.at < 60000;
    w.soc = v.soc; w.v = v.v; w.p = v.p; w.ppv = v.ppv; w.pload = v.pload; w.pgrid = v.pgrid;
    w.epv = v.epv; w.eload = v.eload; w.status = v.status;
  }
  return n;
}

} // namespace luxd
