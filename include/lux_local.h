// LuxPower Wi-Fi dongle, read directly on the home network (TCP port 8000).
// Same frames the dongle exchanges with the LuxPower cloud ("translated data"):
//   A1 1A | protocol u16 | len u16 (frame-6) | 01 | func (C1 heartbeat, C2 data) | datalog[10] | payload
//   data payload: len u16 | addr | dev func (3 hold, 4 input) | inverter[10] | reg u16 | ... | crc16 modbus
// The dongle copies every frame it sends to the cloud to all TCP clients too, so
// just listening gives data; we also ask for input registers 0-79 ourselves.
// Pure C++ (no Arduino) so it can be unit tested on the PC.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>

namespace luxl {

static inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}
static inline uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }

// read `count` input registers from `reg`. dl/inv: 10 chars (zeros when unknown). Returns frame length (38).
static inline size_t buildReadInput(uint8_t *o, const char *dl, const char *inv, uint16_t reg, uint16_t count) {
  memset(o, 0, 38);
  o[0] = 0xA1; o[1] = 0x1A;
  o[2] = 1; o[3] = 0;          // protocol 1
  o[4] = 32; o[5] = 0;         // 38 - 6
  o[6] = 1; o[7] = 0xC2;
  if (dl) memcpy(o + 8, dl, 10);
  uint8_t *d = o + 18;
  d[0] = 18; d[1] = 0;         // data length (incl. these 2 bytes, excl. crc)
  d[2] = 0;                    // address: 0 = to inverter
  d[3] = 4;                    // read input registers
  if (inv) memcpy(d + 4, inv, 10);
  d[14] = reg & 0xFF; d[15] = reg >> 8;
  d[16] = count & 0xFF; d[17] = count >> 8;
  uint16_t c = crc16(d + 2, 16);
  d[18] = c & 0xFF; d[19] = c >> 8;
  return 38;
}

struct Inv {
  char sn[11] = {0};     // inverter serial
  char dl[11] = {0};     // dongle (datalog) serial
  uint16_t r[80] = {0};  // input registers 0-79
  uint8_t have = 0;      // bit0: regs 0-39, bit1: regs 40-79
  uint32_t at = 0;       // millis of the last data (filled by the caller)
};

// Which frame was decoded.
enum Kind { NONE = 0, HEARTBEAT, INPUT, OTHER, BAD };

// Decode one complete frame `f` of length n (n = len field + 6).
// For input-register data, the registers are copied into `out` (sn/dl too).
static inline Kind decode(const uint8_t *f, size_t n, Inv &out, char *dlOut = nullptr) {
  if (n < 18 || f[0] != 0xA1 || f[1] != 0x1A) return BAD;
  if (size_t(u16(f + 4)) + 6 != n) return BAD;
  if (dlOut) { memcpy(dlOut, f + 8, 10); dlOut[10] = 0; }
  if (f[7] == 0xC1) return HEARTBEAT;
  if (f[7] != 0xC2 || n < 38) return OTHER;
  uint16_t proto = u16(f + 2);
  const uint8_t *d = f + 20;          // after the 2 data-length bytes
  size_t dn = n - 22;                 // without crc
  uint16_t c = crc16(d, dn);
  if (f[n - 2] != (c & 0xFF) || f[n - 1] != (c >> 8)) return BAD;
  uint8_t fn = d[1];
  if (fn != 4) return OTHER;
  uint16_t reg = u16(d + 12);
  size_t off = 14, len;
  if (proto != 1) { len = d[14]; off = 15; } else len = dn - 14;
  if (off + len > dn || len < 2 || (len & 1)) return BAD;
  if (d[0] == 0 && len == 2) return OTHER; // our own request echoed back
  size_t cnt = len / 2;
  bool any = false;
  for (size_t k = 0; k < cnt; k++) {
    size_t a = reg + k;
    if (a < 80) { out.r[a] = u16(d + off + 2 * k); any = true; }
  }
  if (!any) return OTHER;
  if (reg == 0 && cnt >= 40) out.have |= 1;
  if (reg <= 40 && reg + cnt >= 80) out.have |= 2;
  memcpy(out.sn, d + 2, 10); out.sn[10] = 0;
  memcpy(out.dl, f + 8, 10); out.dl[10] = 0;
  return INPUT;
}

// Stream splitter: feed bytes, get complete frames.
struct Splitter {
  uint8_t buf[600];
  size_t n = 0;
  // returns frame length when buf holds a complete frame at position 0 (call drop() after use)
  size_t frame() {
    for (;;) {
      size_t i = 0;
      while (i + 1 < n && !(buf[i] == 0xA1 && buf[i + 1] == 0x1A)) i++;
      if (i) { memmove(buf, buf + i, n - i); n -= i; }
      if (n < 6) return 0;
      size_t len = size_t(u16(buf + 4)) + 6;
      if (len < 18 || len > sizeof(buf)) { memmove(buf, buf + 1, n - 1); n--; continue; }
      return n >= len ? len : 0;
    }
  }
  void drop(size_t len) { memmove(buf, buf + len, n - len); n -= len; }
  void push(const uint8_t *p, size_t k) {
    if (k > sizeof(buf) - n) { n = 0; if (k > sizeof(buf)) return; }
    memcpy(buf + n, p, k); n += k;
  }
};

static inline const char *statusText(uint16_t s) {
  switch (s) {
    case 0x00: return "Standby"; case 0x02: return "FW updating"; case 0x04: return "PV on-grid";
    case 0x08: return "PV charge"; case 0x0C: return "PV charge on-grid"; case 0x10: return "Battery on-grid";
    case 0x11: return "Bypass"; case 0x14: return "PV & battery on-grid"; case 0x19: return "PV charge + bypass";
    case 0x20: return "AC charge"; case 0x28: return "PV & AC charge"; case 0x40: return "Battery off-grid";
    case 0x80: return "PV off-grid"; case 0xC0: return "PV & battery off-grid"; case 0x88: return "PV charge off-grid";
    default: return "normal";
  }
}

// Values in the same units/names the cloud poller uses.
struct Values {
  float v, p, soc, ppv, ppv1, ppv2, pload, pgrid, peps, pinv, prec, vac, vpv1, t1, mos;
  float epv, ein, eout, eload, egrid; // kWh today (only when regs 0-39 present)
  uint16_t status;
};
static inline Values values(const Inv &x) {
  const uint16_t *r = x.r;
  Values o{};
  o.status = r[0];
  o.vpv1 = r[1] / 10.0f;
  o.v = r[4] / 10.0f;
  o.soc = float(r[5] & 0xFF);
  o.ppv1 = r[7]; o.ppv2 = r[8];
  o.ppv = float(r[7]) + r[8] + r[9];
  o.p = float(r[10]) - float(r[11]);        // + charging
  o.vac = r[12] / 10.0f;
  o.pinv = r[16]; o.prec = r[17];
  o.peps = r[24];
  float toGrid = r[26], toUser = r[27];
  o.pgrid = toUser - toGrid;                // + import
  float l = o.pinv - o.prec + toUser - toGrid;
  o.pload = (l > 0 ? l : 0) + o.peps;
  o.epv = (float(r[28]) + r[29] + r[30]) / 10.0f;
  o.ein = r[33] / 10.0f;
  o.eout = r[34] / 10.0f;
  float el = (float(r[31]) - r[32] + r[37] - r[36]) / 10.0f + r[35] / 10.0f;
  o.eload = el > 0 ? el : 0;
  o.egrid = r[37] / 10.0f;
  o.mos = (x.have & 2) ? float(r[64]) : -100;
  o.t1 = (x.have & 2) ? float(r[67]) : -100;
  return o;
}

static inline bool validSerial(const char *s) {
  int n = 0;
  for (; n < 10 && s[n]; n++) {
    char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
  }
  return n == 10;
}

} // namespace luxl
