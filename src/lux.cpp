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
// auto search: when no dongle answers for 5 min (its IP changed), look for it again (at most every 30 min)
static uint32_t lastLive = 0, lastAuto = 0, autoAt = 0;
static String autoNote;
static const uint32_t AUTO_AFTER = 5 * 60000UL, AUTO_EVERY = 30 * 60000UL;
static const int SCAN_BATCH = 4; // sockets open at the same time; the cloud upload and the web page need the rest

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
      if (kd != luxl::K_BAD) { d.lastRx = millis(); d.frames++; }
      if (kd != luxl::K_BAD && luxl::validSerial(dl)) memcpy(d.dl, dl, 11);
      if (kd == luxl::K_INPUT && luxl::validSerial(tmp.sn)) {
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

// hosts of the /24 with port 8000 open (last octet), at most `max`.
// Only SCAN_BATCH sockets at a time: opening 24 used up every lwIP socket and the
// cloud upload hung until the 10-min watchdog restarted the board (fixed in 2.10.1).
static int portScan(IPAddress me, int *hosts, int max) {
  int nf = 0;
  for (int start = 1; start < 255 && nf < max; start += SCAN_BATCH) {
    int fds[SCAN_BATCH], hs[SCAN_BATCH], n = 0;
    for (int h = start; h < start + SCAN_BATCH && h < 255; h++) {
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
      fds[n] = s; hs[n] = h; n++;
    }
    if (!n) { vTaskDelay(pdMS_TO_TICKS(200)); continue; } // no free socket now: let the others finish
    uint32_t t0 = millis();
    bool done[SCAN_BATCH] = {false};
    while (millis() - t0 < 600) {
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
          if (!e && nf < max) hosts[nf++] = hs[i];
        }
    }
    for (int i = 0; i < n; i++) lwip_close(fds[i]);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  return nf;
}

// does a LuxPower dongle answer at this address? (a valid frame with a datalog serial within 4 s)
static bool probe(const IPAddress &a) {
  WiFiClient c;
  if (!c.connect(a, PORT, 2000)) return false;
  uint8_t f[38];
  luxl::buildReadInput(f, nullptr, nullptr, 0, 40);
  c.write(f, sizeof f);
  luxl::Splitter sp;
  bool ok = false;
  uint32_t t0 = millis();
  while (!ok && millis() - t0 < 4000 && c.connected()) {
    int k = c.available();
    if (k <= 0) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
    uint8_t b[128];
    int r = c.read(b, k > (int)sizeof b ? sizeof b : k);
    if (r <= 0) break;
    sp.push(b, r);
    size_t n;
    while (!ok && (n = sp.frame())) {
      luxl::Inv tmp;
      char dl[11] = {0};
      if (luxl::decode(sp.buf, n, tmp, dl) != luxl::K_BAD && luxl::validSerial(dl)) ok = true;
      sp.drop(n);
    }
  }
  c.stop();
  return ok;
}

// manual (button) or automatic search. Found list: LuxPower dongles first,
// or every host with port 8000 when none answered like a dongle.
static void doScan(bool autoMode) {
  { G g; scanBusy = true; scanNote = autoMode ? "auto: searching" : "scanning"; }
  IPAddress me = WiFi.localIP();
  if (WiFi.status() != WL_CONNECTED || (uint32_t)me == 0) {
    G g; scanBusy = false; scanNote = "not on home Wi-Fi"; return;
  }
  int hosts[8];
  int nh = portScan(me, hosts, 8);
  String lux[MAXD];
  int nl = 0;
  bool isLux[8] = {false};
  for (int i = 0; i < nh; i++) {
    isLux[i] = probe(IPAddress(me[0], me[1], me[2], hosts[i]));
    if (isLux[i] && nl < MAXD) lux[nl++] = IPAddress(me[0], me[1], me[2], hosts[i]).toString();
  }
  String found = "[";
  int nf = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < nh; i++) {
      if (nl ? (pass == 0 ? !isLux[i] : true) : pass == 1) continue;
      if (nf++) found += ",";
      found += "\"";
      found += IPAddress(me[0], me[1], me[2], hosts[i]).toString();
      found += "\"";
    }
  found += "]";
  String note = nl ? "found" : nh ? "port 8000 open but no LuxPower answer" : "no device with port 8000";
  String moved;
  if (autoMode && nl) {
    // dongles that answered first, then the old addresses (kept in case one is only switched off)
    String cur, nw;
    int k = 0;
    for (int i = 0; i < nl; i++) { if (k++) nw += ","; nw += lux[i]; }
    {
      G g;
      for (auto &d : dg) {
        if (!d.ip.length()) continue;
        if (cur.length()) cur += ",";
        cur += d.ip;
        bool dup = false;
        for (int i = 0; i < nl; i++) if (lux[i] == d.ip) dup = true;
        if (!dup && k < MAXD) { if (k++) nw += ","; nw += d.ip; }
      }
    }
    if (nw != cur) { configure(nw, true); moved = nw; }
  }
  G g;
  scanFound = found;
  scanBusy = false;
  scanAt = millis();
  scanNote = note;
  if (autoMode) {
    autoAt = millis();
    if (moved.length()) { autoNote = "dongle moved to "; autoNote += moved; }
    else autoNote = nl ? "dongle found, same IP" : "no LuxPower dongle on the Wi-Fi";
  }
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
      lastLive = millis();
    }
    if (scanWanted) { scanWanted = false; doScan(false); }
    bool en;
    { G g; en = on; }
    if (!en || WiFi.status() != WL_CONNECTED) {
      for (auto &d : dg) if (d.conn) { G g; drop(d, en ? "Wi-Fi down" : "off"); d.st = en ? "retry" : "off"; }
      continue;
    }
    for (int i = 0; i < MAXD; i++) if (dg[i].ip.length()) serviceDongle(i);
    uint32_t now = millis();
    bool live = false, any = false;
    for (auto &d : dg) { if (d.ip.length()) any = true; if (d.st == "live") live = true; }
    if (live || !any) lastLive = now;
    else if (now - lastLive > AUTO_AFTER && (!lastAuto || now - lastAuto > AUTO_EVERY)) { lastAuto = now; doScan(true); }
  }
}

void start() {
  mtx = xSemaphoreCreateMutex();
  load();
  lastLive = millis();
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
  if (autoNote.length()) { o["auto"] = autoNote; o["autoAgo"] = (now - autoAt) / 1000; }
}

// one short line for the screen while no inverter data comes in
void hint(char *out, size_t n) {
  G g;
  out[0] = 0;
  if (!on) return;
  if (scanBusy || scanWanted) { snprintf(out, n, "Searching the home Wi-Fi for the dongle..."); return; }
  if (autoNote.length() && millis() - autoAt < 15 * 60000UL) { snprintf(out, n, "Auto search: %s", autoNote.c_str()); return; }
  for (auto &d : dg)
    if (d.ip.length() && d.st != "live" && d.err.length()) {
      uint32_t now = millis();
      if (lastAuto && now - lastAuto < AUTO_EVERY)
        snprintf(out, n, "%s: no answer - next search in %u min", d.ip.c_str(), unsigned((AUTO_EVERY - (now - lastAuto)) / 60000 + 1));
      else
        snprintf(out, n, "%s: no answer - auto search after 5 min", d.ip.c_str());
      return;
    }
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
