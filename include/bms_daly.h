#pragma once
// DALY BMS - the two protocol families in use today.
//
//  * New DALY (H / K / M / S series, 100 Balance) - Modbus-like frames.
//      Bluetooth (service FFF0, write FFF2, notify FFF1): start byte 0xD2 both
//      ways, status block = 62 (or 80) registers from 0x0000, compact map.
//      Wire (UART / RS485): request start 0x80 + address, reply start 0x50 +
//      address, full register map (cells 0x00.., voltage 0x38 ..).
//      Read = function 0x03, write = 0x06. CRC = Modbus CRC16, low byte first.
//      MOS (Bluetooth map): charge 0x00A5, discharge 0x00A6, balancer 0x00CF.
//      MOS (wire map):      charge 0x0121, discharge 0x0122.
//      (References: syssi/esphome-daly-bms, patagonaa/esphome-daly-hkms-bms.)
//  * Older DALY (J / T / A / U / W / ND, Bluetooth dongle) - 13-byte frames:
//      A5 <addr> <id> 08 <8 data> <sum>. Host address 0x40 on the wire, 0x80
//      over Bluetooth; the BMS answers with address 0x01.
//      0x90 V/I/SOC, 0x93 MOS + remaining capacity, 0x94 counts + cycles,
//      0x95 cell voltages (3 per frame), 0x96 temperatures (7 per frame).
//      MOS: 0xDA charge, 0xD9 discharge (data byte 0 = 1 on / 0 off).
#include "jk_protocol.h"
#include <stdint.h>
#include <string.h>
namespace daly {
inline uint16_t crc16(const uint8_t *p, int n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }

// ---------------- new DALY (Modbus-like) ----------------
// start = 0xD2 over Bluetooth, 0x80 + address on the wire
inline int d2Frame(uint8_t *f, uint8_t start, uint8_t func, uint16_t reg, uint16_t val) {
  f[0] = start; f[1] = func; f[2] = reg >> 8; f[3] = reg; f[4] = val >> 8; f[5] = val;
  uint16_t c = crc16(f, 6);
  f[6] = c & 0xFF; f[7] = c >> 8;
  return 8;
}
const uint16_t BLE_STATUS_REGS = 62;
const uint16_t BLE_MOS_REG[3] = {0x00A5, 0x00A6, 0x00CF}; // charge, discharge, balancer
const uint16_t WIRE_MOS_REG[2] = {0x0121, 0x0122};
// Wire polling plan: three reads cover everything we show.
struct WireBlock { uint16_t reg, count; };
const WireBlock WIRE_BLOCKS[3] = {{0x00, 32}, {0x30, 0x18}, {0x48, 0x15}};

// Streaming framer for replies: [start][0x03][len][len bytes][crc lo][crc hi]
// and write echoes [start][0x06][reg 2][val 2][crc 2].
class D2Framer {
  uint8_t b[200];
  size_t n = 0;
  bool okStart(uint8_t c) const { return c == 0xD2 || (c >= 0x50 && c <= 0x5F); }
public:
  uint32_t good = 0, bad = 0;
  void reset() { n = 0; }
  template <class F> void feed(const uint8_t *p, size_t len, F frame) {
    while (len--) {
      uint8_t c = *p++;
      if (n == 0 && !okStart(c)) continue;
      if (n == 1 && c != 0x03 && c != 0x06) { n = okStart(c) ? 1 : 0; if (n) b[0] = c; continue; }
      b[n++] = c;
      size_t need = 0;
      if (n >= 3 && b[1] == 0x03) need = size_t(b[2]) + 5;
      if (b[1] == 0x06) need = 8;
      if (need && (need > sizeof b || (b[1] == 0x03 && b[2] % 2))) { n = 0; bad++; continue; }
      if (need && n == need) {
        uint16_t crc = crc16(b, int(n - 2));
        if (b[n - 2] == (crc & 0xFF) && b[n - 1] == crc >> 8) {
          good++;
          frame(b[0], b[1], b + 3, b[1] == 0x03 ? size_t(b[2]) : 0);
        } else bad++;
        n = 0;
      }
    }
  }
};
// Bluetooth status block (data = register bytes from register 0, >= 62 regs)
inline bool decodeBle(const uint8_t *d, size_t len, jk::Sample &out, uint8_t &balancer) {
  if (len < BLE_STATUS_REGS * 2) return false;
  auto R = [&](int r) { return be16(d + 2 * r); };
  jk::Sample s;
  unsigned cells = R(49);
  if (!cells || cells > 32) return false;
  float total = 0;
  for (unsigned i = 0; i < cells; i++) {
    float v = R(i) * .001f;
    if (v < .5f || v > 5.f) return false;
    s.cells[i] = v; total += v;
  }
  s.count = cells;
  unsigned nt = R(50) > 8 ? 8 : R(50);
  if (nt > 0) s.temp1 = int(R(32)) - 40;
  if (nt > 1) s.temp2 = int(R(33)) - 40;
  s.voltage = R(40) * .1f;
  s.current = (int(R(41)) - 30000) * .1f;
  s.soc = R(42) * .1f;
  s.capacity = R(48) * .1f;
  s.cycles = R(51);
  balancer = R(52) == 1;
  s.charge = R(53) == 1;
  s.discharge = R(54) == 1;
  s.mos = len >= 80 * 2 ? int(R(66)) - 40 : s.temp1;
  if (s.soc > 100.5f || s.voltage < 1 || std::fabs(s.voltage - total) > std::fmax(2.f, total * .05f)) return false;
  out = s;
  return true;
}
// Wire (UART / RS485) register cache, filled block by block.
struct WireRegs {
  uint16_t r[0x60]{};
  uint8_t have = 0; // bit k = WIRE_BLOCKS[k] received
  void store(int block, const uint8_t *d, size_t len) {
    const WireBlock &w = WIRE_BLOCKS[block];
    for (size_t i = 0; i < len / 2 && i < w.count && w.reg + i < 0x60; i++) r[w.reg + i] = be16(d + 2 * i);
    have |= 1 << block;
  }
};
inline bool decodeWire(const WireRegs &w, jk::Sample &out) {
  if ((w.have & 7) != 7) return false;
  const uint16_t *r = w.r;
  jk::Sample s;
  unsigned cells = r[0x3C];
  if (!cells || cells > 32) return false;
  float total = 0;
  for (unsigned i = 0; i < cells; i++) {
    float v = r[i] * .001f;
    if (v < .5f || v > 5.f) return false;
    s.cells[i] = v; total += v;
  }
  s.count = cells;
  unsigned nt = r[0x3D];
  if (nt > 0 && r[0x30] != 255) s.temp1 = int(r[0x30]) - 40;
  if (nt > 1 && r[0x31] != 255) s.temp2 = int(r[0x31]) - 40;
  s.voltage = r[0x38] * .1f;
  s.current = (int(r[0x39]) - 30000) * .1f;
  s.soc = r[0x3A] * .1f;
  s.capacity = r[0x4B] * .1f;
  s.cycles = r[0x4C];
  s.charge = r[0x52] > 0;
  s.discharge = r[0x53] > 0;
  s.mos = r[0x5A] != 255 ? int(r[0x5A]) - 40 : s.temp1;
  if (s.soc > 100.5f || s.voltage < 1 || std::fabs(s.voltage - total) > std::fmax(2.f, total * .05f)) return false;
  out = s;
  return true;
}

// ---------------- older DALY (A5) ----------------
inline int a5Frame(uint8_t *f, uint8_t host, uint8_t id, uint8_t d0 = 0) {
  memset(f, 0, 13);
  f[0] = 0xA5; f[1] = host; f[2] = id; f[3] = 8; f[4] = d0;
  uint8_t s = 0;
  for (int i = 0; i < 12; i++) s += f[i];
  f[12] = s;
  return 13;
}
const uint8_t A5_POLL[5] = {0x90, 0x93, 0x94, 0x95, 0x96};
class A5Framer {
  uint8_t b[13];
  size_t n = 0;
public:
  uint32_t good = 0, bad = 0;
  void reset() { n = 0; }
  template <class F> void feed(const uint8_t *p, size_t len, F frame) {
    while (len--) {
      uint8_t c = *p++;
      if (n == 0 && c != 0xA5) continue;
      if (n == 1 && c != 0x01) { n = c == 0xA5 ? 1 : 0; continue; }
      if (n == 3 && c != 0x08) { n = c == 0xA5 ? 1 : 0; continue; }
      b[n++] = c;
      if (n == 13) {
        uint8_t s = 0;
        for (int i = 0; i < 12; i++) s += b[i];
        if (s == b[12]) { good++; frame(b[2], b + 4); } else bad++;
        n = 0;
      }
    }
  }
};
// Assembles the A5 answers into one sample.
struct A5State {
  jk::Sample s;
  unsigned cells = 0, temps = 0;
  uint32_t seen = 0;   // bit per id (0x90..0x96 -> bit 0..6)
  uint32_t cellMask = 0;
  void reset() { *this = A5State(); }
  // returns true when a complete, plausible sample is ready
  bool add(uint8_t id, const uint8_t *d) {
    if (id < 0x90 || id > 0x96) return false;
    seen |= 1u << (id - 0x90);
    switch (id) {
      case 0x90:
        s.voltage = be16(d) * .1f;
        s.current = (int(be16(d + 4)) - 30000) * .1f;
        s.soc = be16(d + 6) * .1f;
        break;
      case 0x93:
        s.charge = d[1]; s.discharge = d[2];
        s.capacity = (uint32_t(d[4]) << 24 | uint32_t(d[5]) << 16 | uint32_t(d[6]) << 8 | d[7]) * .001f;
        break;
      case 0x94:
        cells = d[0] > 32 ? 32 : d[0];
        temps = d[1];
        s.cycles = be16(d + 5);
        break;
      case 0x95: {
        int fr = d[0];
        for (int k = 0; k < 3; k++) {
          int i = (fr - 1) * 3 + k;
          if (fr >= 1 && i < 32) { s.cells[i] = be16(d + 1 + 2 * k) * .001f; cellMask |= 1u << i; }
        }
        break;
      }
      case 0x96:
        if (d[0] == 1) {
          if (temps > 0) s.temp1 = int(d[1]) - 40;
          if (temps > 1) s.temp2 = int(d[2]) - 40;
          s.mos = s.temp1;
        }
        break;
    }
    if (!cells || (seen & 0x11) != 0x11) return false; // need 0x90 + 0x94
    uint32_t want = cells >= 32 ? 0xFFFFFFFFu : (1u << cells) - 1;
    if ((cellMask & want) != want) return false;
    float total = 0;
    for (unsigned i = 0; i < cells; i++) { if (s.cells[i] < .5f || s.cells[i] > 5.f) return false; total += s.cells[i]; }
    for (unsigned i = cells; i < 32; i++) s.cells[i] = 0;
    s.count = cells;
    return s.voltage > 1 && s.soc <= 100.5f && std::fabs(s.voltage - total) <= std::fmax(2.f, total * .05f);
  }
};
} // namespace daly
