// Inverters of other brands read on the home network (no Arduino dependencies: unit-tested on the PC).
//   Deye / Sofar : Solarman Wi-Fi logger, TCP 8899, "V5" frames wrapping Modbus RTU (works next to the Solarman cloud)
//   Solis        : S2-WL-ST logger, Modbus TCP 502 (input registers 33000+)
//   PowMr & co.  : Eybond collector (SmartESS). The collector connects back to us after "set>server=ip:port;" on
//                  UDP 58899; frames = 8 byte header + the inverter's own protocol (PI30 text or SMG Modbus RTU)
// Read only by design: there is no code here that builds a write request.
// Framing follows pysolarmanv5 (Solarman V5), ha-solarman register definitions (Deye, Sofar, Solis) and the
// ha-dessmonitor local driver (Eybond, PI30, SMG).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace invl {

struct Val {
  float soc = NAN, v = NAN, i = NAN, p = NAN;          // battery, p > 0 = charging
  float ppv = NAN, pload = NAN, pgrid = NAN;           // W, pgrid > 0 = buying from the grid
  float vac = NAN, vpv1 = NAN, t1 = NAN, mos = NAN;    // t1 battery temp, mos inverter temp
  float epv = NAN, eload = NAN, ein = NAN, eout = NAN, egrid = NAN; // kWh today
  uint16_t status = 0;
  bool gridEst = false;                                // pgrid is an estimate (inverter has no grid meter)
};

inline int16_t s16(uint16_t x) { return (int16_t)x; }

// ---------------------------------------------------------------- checksums
inline uint16_t crcModbus(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}
inline uint16_t crcXmodem(const uint8_t *d, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
  }
  return c;
}

// ---------------------------------------------------------------- Modbus RTU (read only: 0x03 / 0x04)
inline size_t mbReq(uint8_t *o, uint8_t slave, uint8_t fc, uint16_t start, uint16_t count) {
  o[0] = slave; o[1] = fc; o[2] = start >> 8; o[3] = start & 0xFF; o[4] = count >> 8; o[5] = count & 0xFF;
  uint16_t c = crcModbus(o, 6);
  o[6] = c & 0xFF; o[7] = c >> 8;
  return 8;
}
// Returns the number of registers, or <0. Extra bytes after the frame are tolerated (some Deye loggers add two).
// slave 0 = accept any address (Eybond collectors answer with the inverter's real address).
inline int mbParse(const uint8_t *f, size_t n, uint8_t slave, uint8_t fc, uint16_t count, uint16_t *regs) {
  if (n < 5) return -1;
  if (slave && f[0] != slave) return -2;
  if (f[1] == (fc | 0x80)) return -10 - f[2];
  if (f[1] != fc) return -3;
  size_t need = 3 + (size_t)count * 2 + 2;
  if (f[2] != count * 2 || n < need) return -4;
  uint16_t c = crcModbus(f, need - 2);
  if (f[need - 2] != (c & 0xFF) || f[need - 1] != (c >> 8)) return -5;
  for (uint16_t i = 0; i < count; i++) regs[i] = (uint16_t)f[3 + i * 2] << 8 | f[4 + i * 2];
  return count;
}

// ---------------------------------------------------------------- Solarman V5 (little endian around a Modbus RTU frame)
inline size_t v5Req(uint8_t *o, uint32_t serial, uint8_t seq, const uint8_t *mb, size_t mbLen) {
  uint16_t len = 15 + mbLen;
  size_t k = 0;
  o[k++] = 0xA5; o[k++] = len & 0xFF; o[k++] = len >> 8; o[k++] = 0x10; o[k++] = 0x45; o[k++] = seq; o[k++] = 0x00;
  for (int i = 0; i < 4; i++) o[k++] = (serial >> (8 * i)) & 0xFF;
  o[k++] = 0x02; o[k++] = 0; o[k++] = 0;           // frame type: inverter, sensor type
  for (int i = 0; i < 12; i++) o[k++] = 0;         // total working / power on / offset time
  memcpy(o + k, mb, mbLen); k += mbLen;
  uint8_t cs = 0;
  for (size_t i = 1; i < k; i++) cs += o[i];
  o[k++] = cs; o[k++] = 0x15;
  return k;
}
// total frame length once the 3 byte prefix is in, 0 = not a V5 frame
inline size_t v5Total(const uint8_t *f, size_t n) { return n >= 3 && f[0] == 0xA5 ? 13 + ((size_t)f[1] | (size_t)f[2] << 8) : 0; }
// Returns 1 and the Modbus frame inside, 0 = other frame (heartbeat ...), <0 = bad.
inline int v5Parse(const uint8_t *f, size_t n, uint32_t serial, uint8_t seq, const uint8_t **mb, size_t *mbLen) {
  if (n < 13 || f[0] != 0xA5 || f[n - 1] != 0x15) return -1;
  size_t len = (size_t)f[1] | (size_t)f[2] << 8;
  if (n != 13 + len) return -2;
  uint8_t cs = 0;
  for (size_t i = 1; i < n - 2; i++) cs += f[i];
  if (cs != f[n - 2]) return -3;
  uint32_t sn = (uint32_t)f[7] | (uint32_t)f[8] << 8 | (uint32_t)f[9] << 16 | (uint32_t)f[10] << 24;
  if (sn != serial) return -4;
  if (f[3] != 0x10 || f[4] != 0x15) return 0;      // not a reply to a request
  if (f[5] != seq) return 0;                        // answer to an older request
  if (len < 14 + 5 || f[11] != 0x02) return -5;
  *mb = f + 25; *mbLen = len - 14;
  return 1;
}

// ---------------------------------------------------------------- Modbus TCP (Solis S2-WL-ST)
inline size_t mbtcpReq(uint8_t *o, uint16_t tid, uint8_t unit, uint8_t fc, uint16_t start, uint16_t count) {
  o[0] = tid >> 8; o[1] = tid & 0xFF; o[2] = 0; o[3] = 0; o[4] = 0; o[5] = 6;
  o[6] = unit; o[7] = fc; o[8] = start >> 8; o[9] = start & 0xFF; o[10] = count >> 8; o[11] = count & 0xFF;
  return 12;
}
inline size_t mbtcpTotal(const uint8_t *f, size_t n) { return n >= 6 ? 6 + ((size_t)f[4] << 8 | f[5]) : 0; }
inline int mbtcpParse(const uint8_t *f, size_t n, uint16_t tid, uint8_t fc, uint16_t count, uint16_t *regs) {
  if (n < 9 || ((uint16_t)f[0] << 8 | f[1]) != tid || f[2] || f[3]) return -1;
  if (f[7] == (fc | 0x80)) return -10 - f[8];
  if (f[7] != fc || f[8] != count * 2 || n < 9 + (size_t)count * 2) return -4;
  for (uint16_t i = 0; i < count; i++) regs[i] = (uint16_t)f[9 + i * 2] << 8 | f[10 + i * 2];
  return count;
}

// ---------------------------------------------------------------- Eybond collector transport
static const uint8_t EY_HEARTBEAT = 1, EY_FORWARD = 4;
struct EyHdr { uint16_t tid, devcode, wire; uint8_t addr, fc; size_t total() const { return (size_t)wire + 6; } };
inline size_t eyFrame(uint8_t *o, uint16_t tid, uint16_t devcode, uint8_t addr, uint8_t fc, const uint8_t *pl, size_t n) {
  uint16_t wire = 2 + n;
  o[0] = tid >> 8; o[1] = tid & 0xFF; o[2] = devcode >> 8; o[3] = devcode & 0xFF; o[4] = wire >> 8; o[5] = wire & 0xFF; o[6] = addr; o[7] = fc;
  if (n) memcpy(o + 8, pl, n);
  return 8 + n;
}
inline bool eyHeader(const uint8_t *f, size_t n, EyHdr &h) {
  if (n < 8) return false;
  h.tid = (uint16_t)f[0] << 8 | f[1]; h.devcode = (uint16_t)f[2] << 8 | f[3]; h.wire = (uint16_t)f[4] << 8 | f[5]; h.addr = f[6]; h.fc = f[7];
  return h.total() >= 8 && h.total() <= 1024;
}
// heartbeat request: UTC date/time + interval in seconds
inline size_t eyHeartbeat(uint8_t *o, uint16_t tid, int yy, int mo, int dd, int hh, int mi, int ss, uint16_t interval) {
  uint8_t p[8] = {(uint8_t)yy, (uint8_t)mo, (uint8_t)dd, (uint8_t)hh, (uint8_t)mi, (uint8_t)ss, (uint8_t)(interval >> 8), (uint8_t)(interval & 0xFF)};
  return eyFrame(o, tid, 0, 1, EY_HEARTBEAT, p, 8);
}

// ---------------------------------------------------------------- PI30 (Voltronic Axpert family, text protocol)
inline void pi30Crc(const uint8_t *d, size_t n, uint8_t out[2]) {
  uint16_t c = crcXmodem(d, n);
  out[0] = c >> 8; out[1] = c & 0xFF;
  for (int i = 0; i < 2; i++) if (out[i] == 0x28 || out[i] == 0x0A || out[i] == 0x0D) out[i]++;
}
// only the read-only inquiries are ever built
inline size_t pi30Cmd(uint8_t *o, const char *cmd) {
  static const char *ok[] = {"QPI", "QID", "QPIGS", "QPIGS2", "QMOD", "QPIRI"};
  bool allowed = false;
  for (auto a : ok) if (!strcmp(a, cmd)) allowed = true;
  if (!allowed) return 0;
  size_t n = strlen(cmd);
  memcpy(o, cmd, n);
  pi30Crc(o, n, o + n);
  o[n + 2] = '\r';
  return n + 3;
}
// "(....<crc><cr>" -> text between the bracket and the crc. false on bad frame or NAK.
inline bool pi30Parse(const uint8_t *f, size_t n, char *out, size_t cap) {
  if (n < 5 || f[0] != '(' || f[n - 1] != '\r') return false;
  uint8_t c[2];
  pi30Crc(f, n - 3, c);
  if (c[0] != f[n - 3] || c[1] != f[n - 2]) return false;
  size_t len = n - 4;
  if (len >= cap) return false;
  memcpy(out, f + 1, len); out[len] = 0;
  return strcmp(out, "NAK") != 0;
}
// QPIGS text -> values. mode = QMOD letter ('L' line, 'B' battery, ...), 0 if unknown.
inline bool pi30Qpigs(const char *txt, char mode, Val &o) {
  float f[24]; int n = 0; char flags[12] = "";
  const char *p = txt;
  while (*p && n < 24) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (n == 16) { size_t l = e - p; if (l > 11) l = 11; memcpy(flags, p, l); flags[l] = 0; f[n] = 0; }
    else {
      char *end; f[n] = strtof(p, &end);
      if (end == p) return false;
    }
    n++; p = e;
  }
  if (n != 17 && n < 21) return false;
  if (f[0] > 300 || f[2] > 300 || f[8] > 1000 || f[10] > 100 || f[5] > 150000) return false;
  o.vac = f[2]; o.pload = f[5]; o.v = f[8]; o.soc = f[10]; o.mos = f[11]; o.vpv1 = f[13];
  o.i = f[9] - f[15]; o.p = o.v * o.i;
  o.ppv = n >= 21 ? f[19] : f[12] * f[8];          // older models report the PV current on the battery side
  o.status = mode ? (uint16_t)mode : 0;
  // these inverters do not measure the grid: estimate it from the balance when the grid is feeding the load
  o.gridEst = true;
  float g = o.pload + o.p - o.ppv;
  o.pgrid = (f[0] > 100 && mode != 'B') ? (g > 0 ? g : 0) : 0;
  return true;
}

// ---------------------------------------------------------------- register blocks
struct Block { uint16_t start, count; };
static const int MAXBLK = 4, MAXREG = 250;
// registers of one read cycle, addressed by their real address
struct Regs {
  Block blk[MAXBLK]; int nb = 0; uint16_t d[MAXREG]; int off[MAXBLK]; int used = 0; bool miss = false;
  void clear() { nb = 0; used = 0; miss = false; }
  uint16_t *add(uint16_t start, uint16_t count) {
    if (nb >= MAXBLK || used + count > MAXREG) return nullptr;
    blk[nb] = {start, count}; off[nb] = used; used += count; nb++;
    return d + off[nb - 1];
  }
  uint16_t r(uint16_t a) {
    for (int i = 0; i < nb; i++) if (a >= blk[i].start && a < blk[i].start + blk[i].count) return d[off[i] + a - blk[i].start];
    miss = true; return 0;
  }
  int16_t s(uint16_t a) { return (int16_t)r(a); }
  uint32_t u32(uint16_t hi, uint16_t lo) { return (uint32_t)r(hi) << 16 | r(lo); }
};

enum Model : uint8_t { M_NONE, M_DEYE_1P, M_DEYE_3P, M_SOFAR_G3, M_SOFAR_ES, M_SOLIS, M_PI30, M_SMG };
inline const char *modelName(Model m) {
  switch (m) {
    case M_DEYE_1P: return "Deye 1P hybrid"; case M_DEYE_3P: return "Deye 3P hybrid";
    case M_SOFAR_G3: return "Sofar HYD G3"; case M_SOFAR_ES: return "Sofar HYD-ES";
    case M_SOLIS: return "Solis hybrid"; case M_PI30: return "PI30"; case M_SMG: return "SMG";
    default: return "";
  }
}
// blocks to read each cycle (all with the model's function code)
inline int blocks(Model m, const Block **b) {
  static const Block deye1[] = {{59, 54}, {150, 46}};
  static const Block deye3[] = {{500, 60}, {586, 92}};
  static const Block sofG3[] = {{1157, 43}, {1412, 6}, {1540, 5}, {1668, 22}};
  static const Block sofEs[] = {{512, 60}};
  static const Block solis[] = {{33035, 61}, {33133, 48}, {33257, 2}};
  static const Block smg[] = {{201, 34}};
  switch (m) {
    case M_DEYE_1P: *b = deye1; return 2; case M_DEYE_3P: *b = deye3; return 2;
    case M_SOFAR_G3: *b = sofG3; return 4; case M_SOFAR_ES: *b = sofEs; return 1;
    case M_SOLIS: *b = solis; return 3; case M_SMG: *b = smg; return 1;
    default: *b = nullptr; return 0;
  }
}
inline uint8_t fcOf(Model m) { return m == M_SOLIS ? 4 : 3; }

// Deye register 0 = device type: 3 = single phase hybrid, 5 / 6 = three phase hybrid (LV / HV). Some firmwares
// put the type in the high byte.
inline Model deyeModel(uint16_t r0) {
  uint8_t t = (r0 & 0xFF) ? (r0 & 0xFF) : (r0 >> 8);
  if ((r0 >> 8) >= 2 && (r0 >> 8) <= 6 && (r0 & 0xFF) == 0) t = r0 >> 8;
  return (t == 5 || t == 6) ? M_DEYE_3P : M_DEYE_1P;
}
inline bool deyeHv(uint16_t r0) { uint8_t t = ((r0 >> 8) >= 2 && (r0 & 0xFF) == 0) ? (r0 >> 8) : (r0 & 0xFF); return t == 6; }

// hv: Deye high-voltage three phase models scale battery volts and several powers by 10
inline bool decode(Model m, Regs &R, Val &o, bool hv = false) {
  R.miss = false;
  switch (m) {
    case M_DEYE_1P:
      o.status = R.r(59); o.ein = R.r(70) * 0.1f; o.eout = R.r(71) * 0.1f; o.egrid = R.r(76) * 0.1f; o.eload = R.r(84) * 0.1f;
      o.epv = R.r(108) * 0.1f; o.vpv1 = R.r(109) * 0.1f; o.mos = (R.s(91) - 1000) * 0.1f; o.vac = R.r(152) * 0.1f;
      o.pgrid = R.s(169); o.pload = R.s(178); o.t1 = (R.r(182) - 1000) * 0.1f; o.v = R.r(183) * 0.01f; o.soc = R.r(184);
      o.ppv = (float)R.r(186) + R.r(187); o.p = -(float)R.s(190); o.i = -R.s(191) * 0.01f;   // Deye: + = discharging
      break;
    case M_DEYE_3P: {
      float k = hv ? 10.0f : 1.0f;
      o.status = R.r(500); o.ein = R.r(514) * 0.1f; o.eout = R.r(515) * 0.1f; o.egrid = R.r(520) * 0.1f; o.eload = R.r(526) * 0.1f;
      o.epv = R.r(529) * 0.1f; o.mos = (R.s(541) - 1000) * 0.1f; o.t1 = (R.r(586) - 1000) * 0.1f;
      o.v = R.r(587) * (hv ? 0.1f : 0.01f); o.soc = R.r(588); o.p = -(float)R.s(590) * k;
      o.pgrid = R.s(625); o.pload = R.s(653); o.ppv = ((float)R.r(672) + R.r(673) + R.r(674) + R.r(675)) * k; o.vpv1 = R.r(676) * 0.1f;
      o.vac = R.r(598) * 0.1f;
      if (o.v > 1) o.i = o.p / o.v;
      break;
    }
    case M_SOFAR_G3:
      o.pgrid = -(float)R.s(1160) * 10; o.pload = (float)R.s(1199) * 10; o.vpv1 = R.r(1412) * 0.1f;
      o.ppv = ((float)R.r(1414) + R.r(1417)) * 10; o.v = R.r(1540) * 0.1f; o.i = R.s(1541) * 0.01f; o.p = (float)R.s(1542) * 10;
      o.t1 = R.s(1543); o.soc = R.r(1544);
      o.epv = R.u32(1668, 1669) * 0.01f; o.eload = R.u32(1672, 1673) * 0.01f; o.egrid = R.u32(1676, 1677) * 0.01f;
      o.ein = R.u32(1684, 1685) * 0.01f; o.eout = R.u32(1688, 1689) * 0.01f;
      break;
    case M_SOFAR_ES: {
      uint16_t bv = R.r(526);
      o.status = R.r(512); o.p = (float)R.s(525) * 10; o.v = bv * (bv > 1000 ? 0.01f : 0.1f); o.i = R.s(527) * 0.01f;
      o.soc = R.r(528); o.t1 = R.s(529); o.pgrid = -(float)R.s(530) * 10; o.pload = (float)R.r(531) * 10; o.ppv = (float)R.r(533) * 10;
      o.epv = R.r(536) * 0.01f; o.egrid = R.r(538) * 0.01f; o.eload = R.r(539) * 0.01f; o.ein = R.r(548) * 0.01f; o.eout = R.r(549) * 0.01f;
      o.vac = R.r(560) * 0.1f; o.mos = R.s(568);
      break;
    }
    case M_SOLIS: {
      bool dis = R.r(33135) == 1;
      o.epv = R.r(33035) * 0.1f; o.vpv1 = R.r(33049) * 0.1f; o.ppv = (float)R.u32(33057, 33058); o.vac = R.r(33073) * 0.1f;
      o.mos = R.s(33093) * 0.1f; o.status = R.r(33095);
      o.v = R.r(33133) * 0.1f; o.i = R.s(33134) * 0.1f * (dis ? -1 : 1); o.soc = R.r(33139);
      o.pload = (float)R.r(33147) + R.r(33148); o.p = (float)R.u32(33149, 33150) * (dis ? -1 : 1);
      o.ein = R.r(33163) * 0.1f; o.eout = R.r(33167) * 0.1f; o.egrid = R.r(33171) * 0.1f; o.eload = R.r(33179) * 0.1f;
      o.pgrid = -(float)(int32_t)R.u32(33257, 33258);   // meter: + = feeding the grid
      break;
    }
    case M_SMG:
      o.status = R.r(201); o.pgrid = R.s(204); o.vac = R.r(210) * 0.1f; o.pload = R.s(213); o.v = R.r(215) * 0.1f;
      o.p = R.s(217); o.vpv1 = R.r(219) * 0.1f; o.ppv = R.s(223); o.mos = R.s(227); o.soc = R.r(229); o.i = R.s(232) * 0.1f;
      if (R.r(202) < 1000 && R.r(215) == 0 && R.r(210) == 0) return false;
      break;
    default: return false;
  }
  if (R.miss) return false;
  // plausibility: a wrong register map shows up as an impossible battery
  if (!(o.soc >= 0 && o.soc <= 100) || !(o.v >= 0 && o.v < 1000)) return false;
  return true;
}

// ASCII serial out of registers (two characters per register, high byte first). Returns false when it is not text.
inline bool regAscii(const uint16_t *r, int count, char *out, size_t cap) {
  size_t k = 0;
  for (int i = 0; i < count && k + 2 < cap; i++) {
    for (uint8_t c : {(uint8_t)(r[i] >> 8), (uint8_t)(r[i] & 0xFF)}) {
      if (c == 0 || c == ' ' || c == 0xFF) continue;
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) { out[0] = 0; return false; }
      out[k++] = (char)c;
    }
  }
  out[k] = 0;
  return k >= 6;
}

// "ip,mac,serial" answer of a Solarman logger to the UDP discovery on port 48899
inline bool solarmanDiscovery(const char *txt, char *ip, size_t ipCap, uint32_t &serial) {
  const char *c1 = strchr(txt, ',');
  if (!c1) return false;
  const char *c2 = strchr(c1 + 1, ',');
  if (!c2) return false;
  size_t n = c1 - txt;
  if (n < 7 || n >= ipCap) return false;
  memcpy(ip, txt, n); ip[n] = 0;
  char *end; unsigned long s = strtoul(c2 + 1, &end, 10);
  if (end == c2 + 1 || s < 1000000) return false;
  serial = (uint32_t)s;
  return true;
}

} // namespace invl
