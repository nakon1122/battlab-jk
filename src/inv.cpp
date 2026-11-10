// Inverters of other brands, read directly over the home network (see include/inv_local.h).
#include "inv_dongle.h"
#include "inv_local.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

namespace invd {
using namespace invl;

static const int MAXS = 2;
static const uint16_t P_SOLARMAN = 8899, P_SOLARMAN_UDP = 48899, P_SOLIS = 502, P_EY_LISTEN = 8899, P_EY_UDP = 58899;
static const uint32_t POLL_MS = 10000, IO_MS = 3000;
enum Kind : uint8_t { K_NONE, K_DEYE, K_SOFAR, K_SOLIS, K_POWMR };
static const char *kindName(Kind k) { return k == K_DEYE ? "deye" : k == K_SOFAR ? "sofar" : k == K_SOLIS ? "solis" : k == K_POWMR ? "powmr" : ""; }
static Kind kindOf(const String &s) { return s == "deye" ? K_DEYE : s == "sofar" ? K_SOFAR : s == "solis" ? K_SOLIS : s == "powmr" ? K_POWMR : K_NONE; }

// what the rest of the firmware sees (copied under the mutex)
struct Pub {
  Kind kind = K_NONE;
  String ip, st = "off", err, model;
  uint32_t logger = 0;
  char sn[24] = {0};
  Val val;
  uint32_t at = 0;      // millis of the last good read, 0 = never
};
// task-owned connection state
struct Conn {
  WiFiClient cli;
  Model model = M_NONE;
  bool hv = false;
  uint8_t seq = 0, fails = 0;
  uint16_t tid = 0, eyCode = 0, eyRep = 0;
  uint8_t eyAddr = 1;
  char mode = 0;
  uint32_t nextTry = 0, nextPoll = 0, lastHb = 0, lastAnnounce = 0, logger = 0;
  char sn[24] = {0};
  char pn[24] = {0};
};

static SemaphoreHandle_t mtx;
struct G { G() { xSemaphoreTake(mtx, portMAX_DELAY); } ~G() { xSemaphoreGive(mtx); } };
static Pub pub[MAXS];
static Conn cn[MAXS];
static bool on = false;
static volatile bool scanWanted = false, cfgChanged = false;
static bool scanBusy = false;
static String scanFound = "[]", scanNote;
static WiFiServer eySrv(P_EY_LISTEN);
static bool eySrvOn = false;
static Regs regs;
static uint8_t tx[160], rx[600];

static void parseSpec(const String &spec) {
  for (int i = 0; i < MAXS; i++) pub[i] = Pub();
  int k = 0, from = 0;
  while (k < MAXS && from < (int)spec.length()) {
    int e = spec.indexOf(';', from);
    if (e < 0) e = spec.length();
    String part = spec.substring(from, e);
    from = e + 1;
    int c1 = part.indexOf(','), c2 = c1 < 0 ? -1 : part.indexOf(',', c1 + 1);
    if (c1 < 0) continue;
    String kd = part.substring(0, c1), ip = c2 < 0 ? part.substring(c1 + 1) : part.substring(c1 + 1, c2), sn = c2 < 0 ? "" : part.substring(c2 + 1);
    kd.trim(); ip.trim(); sn.trim();
    IPAddress a;
    Kind kk = kindOf(kd);
    if (kk == K_NONE || !a.fromString(ip)) continue;
    pub[k].kind = kk; pub[k].ip = ip; pub[k].logger = (uint32_t)strtoul(sn.c_str(), nullptr, 10);
    k++;
  }
}
static String specOf() {
  String s;
  for (int i = 0; i < MAXS; i++) {
    if (pub[i].kind == K_NONE) continue;
    if (s.length()) s += ";";
    s += String(kindName(pub[i].kind)) + "," + pub[i].ip;
    if (pub[i].logger) s += "," + String(pub[i].logger);
  }
  return s;
}
static void load() {
  Preferences p;
  p.begin("battlab-inv", true);
  String spec = p.getString("spec", "");
  on = p.getBool("on", false);
  p.end();
  parseSpec(spec);
}

bool enabled() { G g; return on && pub[0].kind != K_NONE; }
void configure(const String &spec, bool en) {
  G g;
  parseSpec(spec);
  on = en;
  Preferences p;
  p.begin("battlab-inv", false);
  p.putString("spec", specOf());
  p.putBool("on", on);
  p.end();
  cfgChanged = true;
}
void scan() { scanWanted = true; }

static void setSt(int i, const char *st, const String &err = "") { G g; pub[i].st = st; pub[i].err = err; }

static void drop(int i, const char *why) {
  cn[i].cli.stop();
  cn[i].model = M_NONE; cn[i].fails = 0; cn[i].eyCode = 0;
  cn[i].nextTry = millis() + 15000;
  setSt(i, "retry", why);
}

static bool readN(WiFiClient &c, uint8_t *b, size_t n, uint32_t ms) {
  uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    int a = c.available();
    if (a > 0) { got += c.read(b + got, std::min((size_t)a, n - got)); continue; }
    if (!c.connected() || millis() - t0 > ms) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}
static void flushIn(WiFiClient &c) { while (c.available() > 0) c.read(); }

// ------------------------------------------------------------------ one Modbus read through each transport
static int readSolarman(Conn &c, uint8_t fc, uint16_t start, uint16_t count, uint16_t *out) {
  uint8_t mb[8];
  mbReq(mb, 1, fc, start, count);
  c.seq++;
  size_t n = v5Req(tx, c.logger, c.seq, mb, 8);
  flushIn(c.cli);
  if (c.cli.write(tx, n) != n) return -100;
  uint32_t t0 = millis();
  while (millis() - t0 < IO_MS) {
    if (!readN(c.cli, rx, 3, IO_MS)) return -101;
    size_t tot = v5Total(rx, 3);
    if (!tot || tot > sizeof rx) return -102;
    if (!readN(c.cli, rx + 3, tot - 3, IO_MS)) return -101;
    const uint8_t *m; size_t ml;
    int r = v5Parse(rx, tot, c.logger, c.seq, &m, &ml);
    if (r < 0) return -110 + r;
    if (r == 1) return mbParse(m, ml, 1, fc, count, out);
  }
  return -101;
}
static int readSolis(Conn &c, uint8_t fc, uint16_t start, uint16_t count, uint16_t *out) {
  c.tid++;
  size_t n = mbtcpReq(tx, c.tid, 1, fc, start, count);
  flushIn(c.cli);
  if (c.cli.write(tx, n) != n) return -100;
  if (!readN(c.cli, rx, 6, IO_MS)) return -101;
  size_t tot = mbtcpTotal(rx, 6);
  if (tot < 9 || tot > sizeof rx) return -102;
  if (!readN(c.cli, rx + 6, tot - 6, IO_MS)) return -101;
  return mbtcpParse(rx, tot, c.tid, fc, count, out);
}
// Eybond: send one raw inverter request, get the raw answer. Returns the payload length or <0.
static int eyAsk(Conn &c, uint16_t code, uint8_t addr, const uint8_t *pl, size_t n, uint8_t *resp, size_t cap) {
  c.tid = c.tid % 0xFFFF + 1;
  size_t fl = eyFrame(tx, c.tid, code, addr, EY_FORWARD, pl, n);
  flushIn(c.cli);
  if (c.cli.write(tx, fl) != fl) return -100;
  uint32_t t0 = millis();
  while (millis() - t0 < IO_MS + 1500) {
    if (!readN(c.cli, rx, 8, IO_MS + 1500)) return -101;
    EyHdr h;
    if (!eyHeader(rx, 8, h) || h.total() > sizeof rx) return -102;
    size_t pln = h.total() - 8;
    if (pln && !readN(c.cli, rx + 8, pln, IO_MS)) return -101;
    if (h.fc == EY_FORWARD && h.tid == c.tid) {
      if (pln > cap) return -102;
      memcpy(resp, rx + 8, pln);
      return (int)pln;
    }
  }
  return -101;
}
static bool eyHeartbeatNow(Conn &c) {
  time_t now = time(nullptr);
  struct tm t; gmtime_r(&now, &t);
  c.tid = c.tid % 0xFFFF + 1;
  size_t fl = eyHeartbeat(tx, c.tid, (t.tm_year + 1900 - 2000) & 0xFF, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 60);
  if (c.cli.write(tx, fl) != fl) return false;
  uint32_t t0 = millis();
  while (millis() - t0 < IO_MS) {
    if (!readN(c.cli, rx, 8, IO_MS)) return false;
    EyHdr h;
    if (!eyHeader(rx, 8, h) || h.total() > sizeof rx) return false;
    size_t pln = h.total() - 8;
    if (pln && !readN(c.cli, rx + 8, pln, IO_MS)) return false;
    if (h.fc == EY_HEARTBEAT) {
      c.eyRep = h.devcode;
      size_t k = 0;
      for (size_t i = 0; i < pln && k < sizeof c.pn - 1; i++) if (rx[8 + i] > 0x20 && rx[8 + i] < 0x7F) c.pn[k++] = rx[8 + i];
      c.pn[k] = 0;
      c.lastHb = millis();
      return true;
    }
  }
  return false;
}
static int eyPi30(Conn &c, uint16_t code, uint8_t addr, const char *cmd, char *out, size_t cap) {
  uint8_t q[16], r[200];
  size_t n = pi30Cmd(q, cmd);
  if (!n) return -1;
  int rl = eyAsk(c, code, addr, q, n, r, sizeof r);
  if (rl <= 0) return rl ? rl : -1;
  return pi30Parse(r, rl, out, cap) ? 1 : -2;
}
static int readEySmg(Conn &c, uint16_t code, uint8_t addr, uint16_t start, uint16_t count, uint16_t *out) {
  uint8_t q[8], r[300];
  mbReq(q, 1, 3, start, count);
  int rl = eyAsk(c, code, addr, q, 8, r, sizeof r);
  if (rl <= 0) return rl ? rl : -1;
  return mbParse(r, rl, 0, 3, count, out);
}

static int readBlock(int i, uint8_t fc, uint16_t start, uint16_t count, uint16_t *out) {
  Conn &c = cn[i];
  Kind k;
  { G g; k = pub[i].kind; }
  if (k == K_SOLIS) return readSolis(c, fc, start, count, out);
  if (k == K_POWMR) return readEySmg(c, c.eyCode, c.eyAddr, start, count, out);
  return readSolarman(c, fc, start, count, out);
}

// ask a Solarman logger for its serial number (needed in every V5 frame)
static uint32_t solarmanSerial(const String &ip) {
  WiFiUDP u;
  if (!u.begin(0)) return 0;
  IPAddress a; a.fromString(ip);
  uint32_t found = 0;
  for (int tryN = 0; tryN < 2 && !found; tryN++) {
    u.beginPacket(a, P_SOLARMAN_UDP);
    u.print("WIFIKIT-214028-READ");
    u.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 1200 && !found) {
      int n = u.parsePacket();
      if (n > 0) {
        char b[96]; int l = u.read((uint8_t *)b, sizeof b - 1); b[l > 0 ? l : 0] = 0;
        char ipb[20]; uint32_t s;
        if (solarmanDiscovery(b, ipb, sizeof ipb, s)) found = s;
      } else vTaskDelay(pdMS_TO_TICKS(30));
    }
  }
  u.stop();
  return found;
}

static void snFallback(int i, const char *prefix) {
  Conn &c = cn[i];
  String ip; { G g; ip = pub[i].ip; }
  ip.replace('.', '-');
  snprintf(c.sn, sizeof c.sn, "%s-%s", prefix, ip.c_str());
}

// find out what is behind the connection: model, register map, serial number
static bool detect(int i) {
  Conn &c = cn[i];
  Kind k; { G g; k = pub[i].kind; }
  uint16_t r[16];
  setSt(i, "detect");
  if (k == K_DEYE) {
    if (readSolarman(c, 3, 0, 8, r) != 8) return false;
    c.model = deyeModel(r[0]); c.hv = c.model == M_DEYE_3P && deyeHv(r[0]);
    if (!regAscii(r + 3, 5, c.sn, sizeof c.sn)) snprintf(c.sn, sizeof c.sn, "DY%lu", (unsigned long)c.logger);
    return true;
  }
  if (k == K_SOFAR) {
    if (readSolarman(c, 3, 1093, 8, r) == 8) { c.model = M_SOFAR_G3; if (!regAscii(r, 8, c.sn, sizeof c.sn)) snprintf(c.sn, sizeof c.sn, "SF%lu", (unsigned long)c.logger); return true; }
    vTaskDelay(pdMS_TO_TICKS(300));
    if (readSolarman(c, 3, 8193, 7, r) == 7) { c.model = M_SOFAR_ES; if (!regAscii(r, 7, c.sn, sizeof c.sn)) snprintf(c.sn, sizeof c.sn, "SF%lu", (unsigned long)c.logger); return true; }
    return false;
  }
  if (k == K_SOLIS) {
    if (readSolis(c, 4, 33139, 1, r) != 1) return false;
    c.model = M_SOLIS;
    uint16_t s[16];
    if (!(readSolis(c, 4, 33004, 16, s) == 16 && regAscii(s, 16, c.sn, sizeof c.sn))) snFallback(i, "SOLIS");
    return true;
  }
  if (k == K_POWMR) {
    if (!eyHeartbeatNow(c)) return false;
    char txt[160];
    const uint16_t codes[] = {c.eyRep, 2452, 258, 1};
    const uint8_t addrs[] = {1, 255};
    for (uint8_t ad : addrs)
      for (int ci = 0; ci < 4; ci++) {
        uint16_t code = codes[ci];
        if (!code || (ci && code == c.eyRep)) continue;
        if (eyPi30(c, code, ad, "QPIGS", txt, sizeof txt) == 1) {
          Val v;
          if (!pi30Qpigs(txt, 0, v)) continue;
          c.model = M_PI30; c.eyCode = code; c.eyAddr = ad;
          char id[40];
          if (eyPi30(c, code, ad, "QID", id, sizeof id) == 1 && strlen(id) >= 6 && strlen(id) < sizeof c.sn && strspn(id, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == strlen(id) && strspn(id, "0") != strlen(id))
            strcpy(c.sn, id);
          else if (c.pn[0]) snprintf(c.sn, sizeof c.sn, "%s", c.pn);
          else snFallback(i, "EY");
          return true;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
      }
    const uint8_t sa[] = {0xFF, 1};
    for (uint8_t ad : sa) {
      uint16_t live[34];
      if (readEySmg(c, 1, ad, 201, 34, live) == 34) {
        c.model = M_SMG; c.eyCode = 1; c.eyAddr = ad;
        if (c.pn[0]) snprintf(c.sn, sizeof c.sn, "%s", c.pn); else snFallback(i, "EY");
        return true;
      }
      vTaskDelay(pdMS_TO_TICKS(400));
    }
    return false;
  }
  return false;
}

static bool pollOnce(int i) {
  Conn &c = cn[i];
  Val v;
  if (c.model == M_PI30) {
    char txt[160], md[8];
    if (eyPi30(c, c.eyCode, c.eyAddr, "QMOD", md, sizeof md) == 1) c.mode = md[0];
    if (eyPi30(c, c.eyCode, c.eyAddr, "QPIGS", txt, sizeof txt) != 1 || !pi30Qpigs(txt, c.mode, v)) return false;
  } else {
    const Block *b; int nb = blocks(c.model, &b);
    regs.clear();
    for (int k = 0; k < nb; k++) {
      uint16_t *dst = regs.add(b[k].start, b[k].count);
      if (!dst || readBlock(i, fcOf(c.model), b[k].start, b[k].count, dst) != b[k].count) return false;
      vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (!decode(c.model, regs, v, c.hv)) return false;
  }
  G g;
  pub[i].val = v; pub[i].at = millis() ? millis() : 1;
  strlcpy(pub[i].sn, c.sn, sizeof pub[i].sn);
  pub[i].model = modelName(c.model);
  pub[i].st = "live"; pub[i].err = "";
  return true;
}

static void service(int i) {
  Conn &c = cn[i];
  Kind k; String ip; uint32_t logger;
  { G g; k = pub[i].kind; ip = pub[i].ip; logger = pub[i].logger; }
  if (k == K_NONE) return;
  uint32_t now = millis();
  if (!c.cli.connected()) {
    if (c.model != M_NONE) { drop(i, "connection closed"); return; }
    if (k == K_POWMR) {
      // the collector connects to us: ask for it every 5 s, then wait for the call
      if (!eySrvOn) { eySrv.begin(); eySrv.setNoDelay(true); eySrvOn = true; }
      WiFiClient inc = eySrv.available();
      IPAddress want; want.fromString(ip);
      if (inc) {
        if (inc.remoteIP() == want) { c.cli = inc; c.cli.setNoDelay(true); c.fails = 0; c.nextPoll = 0; setSt(i, "connected"); }
        else inc.stop();
        return;
      }
      if (now - c.lastAnnounce > 5000 || !c.lastAnnounce) {
        c.lastAnnounce = now ? now : 1;
        WiFiUDP u;
        if (u.begin(0)) {
          String msg = "set>server=" + WiFi.localIP().toString() + ":" + String(P_EY_LISTEN) + ";";
          u.beginPacket(want, P_EY_UDP); u.print(msg); u.endPacket();
          u.stop();
        }
        setSt(i, "wait");
      }
      return;
    }
    if ((int32_t)(now - c.nextTry) < 0) return;
    setSt(i, "connecting");
    if ((k == K_DEYE || k == K_SOFAR) && !logger) {
      logger = solarmanSerial(ip);
      if (!logger) { c.nextTry = millis() + 20000; setSt(i, "retry", "logger serial not found: enter it"); return; }
      G g; pub[i].logger = logger;
    }
    c.logger = logger;
    IPAddress a; a.fromString(ip);
    if (!c.cli.connect(a, k == K_SOLIS ? P_SOLIS : P_SOLARMAN, 4000)) { c.nextTry = millis() + 15000; setSt(i, "retry", "no answer on the dongle port"); return; }
    c.cli.setNoDelay(true);
    c.fails = 0; c.nextPoll = 0;
    setSt(i, "connected");
    return;
  }
  if (c.model == M_NONE) {
    if (!detect(i)) { drop(i, k == K_POWMR ? "inverter protocol not recognised" : "no Modbus answer from the inverter"); return; }
    c.nextPoll = 0;
  }
  if ((int32_t)(now - c.nextPoll) < 0) return;
  c.nextPoll = now + POLL_MS;
  if (k == K_POWMR && millis() - c.lastHb > 60000) eyHeartbeatNow(c);
  if (pollOnce(i)) c.fails = 0;
  else if (++c.fails >= 4) drop(i, "no valid data");
}

static void closeAll() {
  for (int i = 0; i < MAXS; i++) { cn[i].cli.stop(); cn[i] = Conn(); }
  if (eySrvOn) { eySrv.end(); eySrvOn = false; }
  G g;
  for (int i = 0; i < MAXS; i++) { pub[i].st = "off"; pub[i].err = ""; pub[i].at = 0; }
}

// LAN scan: Solarman loggers answer a UDP broadcast with ip + serial; Solis loggers have TCP 502 open.
static void doScan() {
  { G g; scanBusy = true; scanNote = "scanning"; }
  String found = "[";
  int nf = 0;
  IPAddress me = WiFi.localIP();
  if (WiFi.status() != WL_CONNECTED || (uint32_t)me == 0) { G g; scanBusy = false; scanNote = "not on home Wi-Fi"; return; }
  WiFiUDP u;
  if (u.begin(0)) {
    for (int rnd = 0; rnd < 2; rnd++) {
      u.beginPacket(IPAddress(me[0], me[1], me[2], 255), P_SOLARMAN_UDP);
      u.print("WIFIKIT-214028-READ");
      u.endPacket();
      uint32_t t0 = millis();
      while (millis() - t0 < 1500) {
        int n = u.parsePacket();
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        char b[96]; int l = u.read((uint8_t *)b, sizeof b - 1); b[l > 0 ? l : 0] = 0;
        char ipb[20]; uint32_t s;
        if (!solarmanDiscovery(b, ipb, sizeof ipb, s) || nf >= 6 || found.indexOf(String("\"") + ipb + "\"") >= 0) continue;
        IPAddress chk;
        if (!chk.fromString(ipb)) continue;
        if (nf++) found += ",";
        found += String("{\"k\":\"solarman\",\"ip\":\"") + ipb + "\",\"sn\":\"" + String(s) + "\"}";
      }
    }
    u.stop();
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
      a.sin_port = htons(P_SOLIS);
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
            found += "{\"k\":\"solis\",\"ip\":\"" + IPAddress(me[0], me[1], me[2], hosts[i]).toString() + "\"}";
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
  scanNote = nf ? "" : "none";
}

static void task(void *) {
  bool wasOn = false;
  for (;;) {
    bool run; { G g; run = on; }
    if (cfgChanged) { cfgChanged = false; closeAll(); wasOn = false; }
    if (scanWanted) { scanWanted = false; doScan(); }
    if (run && WiFi.status() == WL_CONNECTED) {
      wasOn = true;
      for (int i = 0; i < MAXS; i++) service(i);
    } else if (wasOn) { wasOn = false; closeAll(); }
    vTaskDelay(pdMS_TO_TICKS(run ? 200 : 1000));
  }
}

void start() {
  mtx = xSemaphoreCreateMutex();
  load();
  xTaskCreatePinnedToCore(task, "invd", 8192, nullptr, 1, nullptr, 0);
}

static bool isFresh(const Pub &p) { return p.at && millis() - p.at < 60000; }
bool fresh() {
  G g;
  if (!on) return false;
  for (int i = 0; i < MAXS; i++) if (pub[i].kind != K_NONE && isFresh(pub[i])) return true;
  return false;
}

static void put(JsonObject o, const char *k, float v, int dec) {
  if (isnan(v)) return;
  float m = dec == 0 ? 1 : dec == 1 ? 10 : 100;
  o[k] = roundf(v * m) / m;
}
void cloudJson(JsonArray arr) {
  G g;
  for (int i = 0; i < MAXS; i++) {
    const Pub &p = pub[i];
    if (p.kind == K_NONE || !isFresh(p) || !p.sn[0]) continue;
    JsonObject o = arr.createNestedObject();
    o["sn"] = p.sn; o["brand"] = kindName(p.kind); o["model"] = p.model;
    o["age"] = (millis() - p.at) / 1000;
    const Val &v = p.val;
    put(o, "soc", v.soc, 0); put(o, "v", v.v, 2); put(o, "i", v.i, 2); put(o, "p", v.p, 0);
    put(o, "ppv", v.ppv, 0); put(o, "pload", v.pload, 0); put(o, "pgrid", v.pgrid, 0);
    put(o, "vac", v.vac, 1); put(o, "vpv1", v.vpv1, 1); put(o, "t1", v.t1, 1); put(o, "mos", v.mos, 1);
    put(o, "epv", v.epv, 1); put(o, "eload", v.eload, 1); put(o, "ein", v.ein, 1); put(o, "eout", v.eout, 1); put(o, "egrid", v.egrid, 1);
    if (v.gridEst) o["pgrid_est"] = 1;
    o["st"] = v.status;
  }
}

void statusJson(JsonObject o) {
  G g;
  o["on"] = on;
  JsonArray src = o.createNestedArray("src");
  JsonArray d = o.createNestedArray("d");
  for (int i = 0; i < MAXS; i++) {
    const Pub &p = pub[i];
    if (p.kind == K_NONE) continue;
    JsonObject s = src.createNestedObject();
    s["k"] = kindName(p.kind); s["ip"] = p.ip;
    if (p.logger) s["sn"] = String(p.logger);
    JsonObject x = d.createNestedObject();
    x["k"] = kindName(p.kind); x["ip"] = p.ip; x["st"] = on ? p.st : String("off");
    if (p.err.length()) x["err"] = p.err;
    if (p.model.length()) x["model"] = p.model;
  }
  JsonObject sc = o.createNestedObject("scan");
  sc["busy"] = scanBusy;
  sc["found"] = serialized(scanFound);
  if (scanNote.length()) sc["note"] = scanNote;
}

int views(luxd::View *out, int max) {
  G g;
  int n = 0;
  if (!on) return 0;
  auto z = [](float v) { return isnan(v) ? 0.f : v; };
  for (int i = 0; i < MAXS && n < max; i++) {
    const Pub &p = pub[i];
    if (p.kind == K_NONE || !p.at) continue;
    luxd::View &o = out[n++];
    // the screen prints the last 4 characters of a 10 character serial
    size_t l = strlen(p.sn);
    memset(o.sn, ' ', 10); o.sn[10] = 0;
    for (size_t k = 0; k < 10 && k < l; k++) o.sn[9 - k] = p.sn[l - 1 - k];
    o.fresh = isFresh(p);
    o.soc = z(p.val.soc); o.v = z(p.val.v); o.p = z(p.val.p); o.ppv = z(p.val.ppv); o.pload = z(p.val.pload);
    o.pgrid = z(p.val.pgrid); o.epv = z(p.val.epv); o.eload = z(p.val.eload); o.status = p.val.status;
  }
  return n;
}

} // namespace invd
