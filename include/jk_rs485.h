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
  if (capAh > 0) s.capacity = capAh * s.soc / 100.f;
  return haveV && s.count > 0;
}
}  // namespace jk485
