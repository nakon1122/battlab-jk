#pragma once
// JK BMS over the RS485 cable.
//  * JK-PB / inverter BMS ("new" RS485): Modbus write 0x10 to 0x1620 / 0x161E /
//    0x161C makes the BMS answer with the same 300-byte 55 AA EB 90 frames as
//    Bluetooth (cells / settings / device info). Settings are written with
//    Modbus 0x10 at 0x1000 + (settings-frame offset - 6), 4 bytes big-endian.
//    (Reference: txubelaxu/esphome-jk-bms jk_rs485_sniffer.)
//  * Older JK BMS with the RS485 adapter: 4E 57 "read all" frame, TLV answer
//    (reference: syssi/esphome-jk-bms jk_modbus / jk_bms). Read only.
#include "jk_protocol.h"
#include <stdint.h>
#include <string.h>
namespace jk485 {
inline uint16_t modbusCrc(const uint8_t *p, int n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c; // send low byte first
}
// type 1 settings, 2 cells, 3 device info
inline int pbRequest(uint8_t *f, uint8_t addr, uint8_t type) {
  uint8_t reg = type == 1 ? 0x1E : type == 2 ? 0x20 : 0x1C;
  const uint8_t t[9] = {addr, 0x10, 0x16, reg, 0x00, 0x01, 0x02, 0x00, 0x00};
  memcpy(f, t, 9);
  uint16_t c = modbusCrc(f, 9);
  f[9] = c & 0xFF; f[10] = c >> 8;
  return 11;
}
inline int pbWrite(uint8_t *f, uint8_t addr, uint16_t reg, uint32_t value) {
  f[0] = addr; f[1] = 0x10; f[2] = reg >> 8; f[3] = reg & 0xFF; f[4] = 0x00; f[5] = 0x02; f[6] = 4;
  f[7] = value >> 24; f[8] = value >> 16; f[9] = value >> 8; f[10] = value;
  uint16_t c = modbusCrc(f, 11);
  f[11] = c & 0xFF; f[12] = c >> 8;
  return 13;
}
inline int oldReadAll(uint8_t *f) {
  const uint8_t t[17] = {0x4E, 0x57, 0x00, 0x13, 0, 0, 0, 0, 0x06, 0x03, 0x00, 0x00, 0, 0, 0, 0, 0x68};
  memcpy(f, t, 17);
  uint16_t s = 0;
  for (int i = 0; i < 17; i++) s += f[i];
  f[17] = 0; f[18] = 0; f[19] = s >> 8; f[20] = s & 0xFF;
  return 21;
}
// Collects 4E 57 frames; returns true and the TLV payload when one is complete.
class OldFramer {
  uint8_t b[1024];
  int n = 0;
public:
  const uint8_t *data = nullptr;
  int dataLen = 0;
  bool feed(uint8_t c) {
    if (n == 0 && c != 0x4E) return false;
    if (n == 1 && c != 0x57) { n = 0; return false; }
    if (n >= (int)sizeof b) n = 0;
    b[n++] = c;
    if (n < 4) return false;
    int len = b[2] << 8 | b[3];
    if (len < 20 || len + 2 > (int)sizeof b) { n = 0; return false; }
    if (n < len + 2) return false;
    uint16_t s = 0;
    for (int i = 0; i < len; i++) s += b[i];
    bool ok = s == (uint16_t(b[len]) << 8 | b[len + 1]);
    n = 0;
    if (!ok) return false;
    data = b + 11;
    dataLen = len - 3 - 11;
    return dataLen > 4;
  }
};
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline float oldTemp(uint16_t v) { return v > 100 ? float(100 - int(v)) : float(v); }
// Decode the TLV answer of an older JK BMS into a Sample.
inline bool decodeOld(const uint8_t *d, int n, jk::Sample &s) {
  int i = 0;
  float capAh = 0;
  bool haveV = false;
  s = jk::Sample();
  while (i < n) {
    uint8_t id = d[i++];
    int len;
    if (id == 0x79) { if (i >= n) break; len = d[i++]; }
    else if (id == 0x85 || id == 0x86 || id == 0x9D || id == 0xA9 || id == 0xAB || id == 0xAC || id == 0xAE || id == 0xAF ||
             id == 0xB1 || id == 0xB3 || id == 0xB8 || id == 0xC0) len = 1;
    else if (id == 0x89 || id == 0xAA || id == 0xB5 || id == 0xB6 || id == 0xB9) len = 4;
    else if (id == 0xB2) len = 10;
    else if (id == 0xB4) len = 8;
    else if (id == 0xB7) len = 15;
    else if (id == 0xBA) len = 24;
    else if (id >= 0x80 && id <= 0xBF) len = 2;
    else break;
    if (i + len > n) break;
    const uint8_t *v = d + i;
    switch (id) {
      case 0x79:
        for (int k = 0; k + 2 < len && s.count < 32; k += 3) {
          float cv = be16(v + k + 1) * 0.001f;
          if (cv > 0.5f && cv < 5.f) s.cells[s.count++] = cv;
        }
        break;
      case 0x80: s.mos = oldTemp(be16(v)); break;
      case 0x81: s.temp1 = oldTemp(be16(v)); break;
      case 0x82: s.temp2 = oldTemp(be16(v)); break;
      case 0x83: s.voltage = be16(v) * 0.01f; haveV = true; break;
      case 0x84: { uint16_t r = be16(v); s.current = (r & 0x8000 ? float(r & 0x7FFF) : -float(r & 0x7FFF)) * 0.01f; break; }
      case 0x85: s.soc = v[0]; break;
      case 0x87: s.cycles = be16(v); break;
      case 0x8C: { uint16_t m = be16(v); s.charge = m & 1; s.discharge = m & 2; break; }
      case 0xAA: capAh = float(uint32_t(be16(v)) << 16 | be16(v + 2)); break;
    }
    i += len;
  }
  if (capAh > 0) { s.capacity = capAh * s.soc / 100.f; s.full = capAh; }
  return haveV && s.count > 0;
}

// ---- Several JK-PB packs on one RS485 bus --------------------------------
// When one pack is the parallel master (DIP 0) it polls the others itself:
//   master -> "aa 10 16 20 00 01 02 00 00 crc" (aa = slave address)
//   slave  -> 300-byte 55 AA EB 90 frame (cells / settings / info)
// and it puts its own frames on the bus after a request to address 0.
// BusWatcher spots those Modbus requests in the byte stream so every frame
// can be filed under the pack that was asked. The board then only listens
// (never talks over the master), so the master keeps its CAN link to the
// inverter. Without a master the board polls the addresses itself.
struct BusReq { uint8_t addr = 0xFF, type = 0; bool write = false; };
class BusWatcher {
  uint8_t w[13]{};
  int n = 0;
public:
  // feed one byte; returns true when the last bytes form a valid JK request
  bool feed(uint8_t c, BusReq &out) {
    if (n < 13) w[n++] = c;
    else { memmove(w, w + 1, 12); w[12] = c; }
    // 11-byte trigger: aa 10 16 rr 00 01 02 00 00 crcL crcH
    if (n >= 11) {
      const uint8_t *q = w + n - 11;
      if (q[0] <= 15 && q[1] == 0x10 && q[2] == 0x16 && (q[3] == 0x20 || q[3] == 0x1E || q[3] == 0x1C) &&
          q[4] == 0 && q[5] == 1 && q[6] == 2 && q[7] == 0 && q[8] == 0) {
        uint16_t crc = modbusCrc(q, 9);
        if (q[9] == (crc & 0xFF) && q[10] == crc >> 8) {
          out.addr = q[0];
          out.type = q[3] == 0x20 ? 2 : q[3] == 0x1E ? 1 : 3;
          out.write = false;
          n = 0;
          return true;
        }
      }
    }
    // 13-byte settings write: aa 10 10 rr 00 02 04 v v v v crcL crcH
    if (n == 13 && w[0] <= 15 && w[1] == 0x10 && w[2] == 0x10 && w[4] == 0 && w[5] == 2 && w[6] == 4) {
      uint16_t crc = modbusCrc(w, 11);
      if (w[11] == (crc & 0xFF) && w[12] == crc >> 8) {
        out.addr = w[0]; out.type = 0; out.write = true; n = 0;
        return true;
      }
    }
    return false;
  }
};
// Which pack a 55 AA EB 90 frame belongs to: the last request seen on the bus
// if it was recent, otherwise the master itself (address 0).
inline uint8_t frameOwner(uint8_t lastReqAddr, uint32_t lastReqAt, uint32_t now, uint32_t window = 600) {
  return (lastReqAddr <= 15 && now - lastReqAt < window) ? lastReqAddr : 0;
}
// Bank totals from the packs that are fresh.
struct BankTotals { float v = 0, i = 0, p = 0, soc = 0, cap = 0, full = 0, tmax = -99, mos = -99, cmin = 9, cmax = 0; uint32_t cycles = 0; int n = 0; bool chg = false, dsg = false; };
inline BankTotals bankTotals(const jk::Sample *s, const bool *fresh, int count) {
  BankTotals t;
  float socw = 0, capSum = 0, vSum = 0;
  for (int k = 0; k < count; k++) {
    if (!fresh[k]) continue;
    const jk::Sample &x = s[k];
    t.n++;
    vSum += x.voltage;
    t.i += x.current;
    t.p += x.voltage * x.current;
    // weight SOC by each pack's full capacity (remaining Ah would favour fuller packs)
    float full = x.full > 0 ? x.full : (x.soc > 3 && x.capacity > 0 ? x.capacity * 100.f / x.soc : 0);
    float w = full > 0 ? full : 1;
    socw += x.soc * w; capSum += w;
    if (x.capacity > 0) t.cap += x.capacity;
    if (full > 0) t.full += full;
    if (x.temp1 > t.tmax) t.tmax = x.temp1;
    if (x.temp2 > t.tmax) t.tmax = x.temp2;
    if (x.mos > t.mos) t.mos = x.mos;
    if (x.cycles > t.cycles) t.cycles = x.cycles;
    t.chg |= x.charge; t.dsg |= x.discharge;
    for (int c = 0; c < 32; c++) if (x.cells[c] > 0) { if (x.cells[c] < t.cmin) t.cmin = x.cells[c]; if (x.cells[c] > t.cmax) t.cmax = x.cells[c]; }
  }
  if (t.n) { t.v = vSum / t.n; t.soc = socw / capSum; }
  return t;
}
}  // namespace jk485
