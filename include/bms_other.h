#pragma once
// ANT (new protocol, 2021+) and JBD / Xiaoxiang BLE support.
// Layouts follow syssi/esphome-ant-bms (ant_bms_ble) and
// syssi/esphome-jbd-bms (jbd_bms_ble).
#include "jk_protocol.h"
#include <array>
#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace bms {

// ---------------------------------------------------------------- ANT ----
// Service 0xFFE0, characteristic 0xFFE1 (write + notify).
// Frame: 7E A1 func addr_lo addr_hi len data[len] crc_lo crc_hi AA 55
static const uint8_t ANT_STATUS_REQUEST[10] = {0x7E, 0xA1, 0x01, 0x00, 0x00,
                                               0xBE, 0x18, 0x55, 0xAA, 0x55};

inline uint16_t antCrc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

class AntFramer {
  uint8_t b[300];
  size_t n = 0;

public:
  uint32_t good = 0, bad = 0;
  void reset() { n = 0; }
  // Calls frame(buffer, length) for every complete, CRC-valid frame.
  template <class F> void feed(const uint8_t *p, size_t len, F frame) {
    // A notification starting with the preamble always starts a new frame.
    if (len >= 2 && p[0] == 0x7E && p[1] == 0xA1)
      n = 0;
    for (size_t i = 0; i < len; i++) {
      if (n == 0 && p[i] != 0x7E)
        continue;
      if (n == 1 && p[i] != 0xA1) {
        n = p[i] == 0x7E ? 1 : 0;
        continue;
      }
      if (n >= sizeof(b)) {
        n = 0;
        bad++;
        continue;
      }
      b[n++] = p[i];
      if (n >= 10 && b[n - 2] == 0xAA && b[n - 1] == 0x55) {
        size_t want = 6 + size_t(b[5]) + 4;
        if (want == n) {
          uint16_t crc = antCrc16(b + 1, n - 5);
          uint16_t remote = uint16_t(b[n - 4]) | uint16_t(b[n - 3]) << 8;
          if (crc == remote) {
            good++;
            frame(b, n);
          } else
            bad++;
          n = 0;
        } else if (n > want) {
          bad++;
          n = 0;
        }
      }
    }
  }
};

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }

// Decode an ANT status frame (function 0x11).
inline bool decodeAnt(const uint8_t *d, size_t n, jk::Sample &out) {
  if (n < 10 || d[2] != 0x11 || n != size_t(6 + d[5] + 4))
    return false;
  uint8_t temps = d[8], cells = d[9];
  if (!cells || cells > 32 || temps > 8)
    return false;
  size_t off = size_t(cells) * 2 + size_t(temps) * 2;
  if (54 + off + 4 > n - 4)
    return false;
  jk::Sample s;
  float total = 0;
  for (uint8_t i = 0; i < cells; i++) {
    float v = le16(d + 34 + i * 2) * .001f;
    if (v < .5f || v > 5.f)
      return false;
    s.cells[i] = v;
    total += v;
  }
  s.count = cells;
  size_t c2 = size_t(cells) * 2;
  if (temps >= 1) s.temp1 = int16_t(le16(d + 34 + c2));
  if (temps >= 2) s.temp2 = int16_t(le16(d + 36 + c2));
  s.mos = int16_t(le16(d + 34 + off));
  s.voltage = le16(d + 38 + off) * .01f;
  s.current = int16_t(le16(d + 40 + off)) * .1f;
  s.soc = int16_t(le16(d + 42 + off));
  s.charge = d[46 + off] == 1;
  s.discharge = d[47 + off] == 1;
  s.capacity = le32(d + 54 + off) * .000001f; // remaining Ah
  s.cycles = 0; // ANT reports cycle capacity, not a cycle count
  if (s.soc > 100 || s.voltage < 1 || s.voltage > 170 ||
      std::fabs(s.voltage - total) > std::fmax(2.f, total * .05f))
    return false;
  out = s;
  return true;
}

// ---------------------------------------------------------------- JBD ----
// Service 0xFF00, notify 0xFF01, write 0xFF02.
// Request: DD A5 cmd 00 chk_hi chk_lo 77
// Response: DD cmd status len data[len] chk_hi chk_lo 77
static const uint8_t JBD_BASIC_REQUEST[7] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
static const uint8_t JBD_CELLS_REQUEST[7] = {0xDD, 0xA5, 0x04, 0x00, 0xFF, 0xFC, 0x77};

inline uint16_t jbdChecksum(const uint8_t *p, size_t n) {
  uint16_t c = 0;
  while (n--)
    c -= *p++;
  return c;
}

class JbdFramer {
  uint8_t b[300];
  size_t n = 0;

public:
  uint32_t good = 0, bad = 0;
  void reset() { n = 0; }
  // Calls frame(cmd, payload, payloadLength) for each valid frame.
  template <class F> void feed(const uint8_t *p, size_t len, F frame) {
    if (len >= 3 && p[0] == 0xDD && p[2] == 0x00)
      n = 0;
    for (size_t i = 0; i < len; i++) {
      if (n == 0 && p[i] != 0xDD)
        continue;
      if (n >= sizeof(b)) {
        n = 0;
        bad++;
        continue;
      }
      b[n++] = p[i];
      if (n >= 4) {
        size_t want = 4 + size_t(b[3]) + 3;
        if (n == want) {
          uint16_t remote = uint16_t(b[n - 3]) << 8 | b[n - 2];
          if (b[n - 1] == 0x77 && b[2] == 0x00 &&
              jbdChecksum(b + 2, b[3] + 2) == remote) {
            good++;
            frame(b[1], b + 4, size_t(b[3]));
          } else
            bad++;
          n = 0;
        }
      }
    }
  }
};

inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }

// Basic info (0x03): everything except per-cell voltages.
inline bool decodeJbdBasic(const uint8_t *d, size_t n, jk::Sample &s) {
  if (n < 23)
    return false;
  float scale = (d[20] & 0x80) ? .1f : .01f;
  s.voltage = be16(d) * .01f;
  s.current = int16_t(be16(d + 2)) * scale;
  s.capacity = be16(d + 4) * scale;
  s.cycles = be16(d + 8);
  s.soc = d[19];
  s.charge = d[20] & 0x01;
  s.discharge = d[20] & 0x02;
  uint8_t ntc = d[22];
  if (ntc >= 1 && n >= 25) s.temp1 = (int(be16(d + 23)) - 2731) * .1f;
  if (ntc >= 2 && n >= 27) s.temp2 = (int(be16(d + 25)) - 2731) * .1f;
  if (ntc >= 3 && n >= 29) s.mos = (int(be16(d + 27)) - 2731) * .1f;
  if (s.soc > 100 || s.voltage < 1 || s.voltage > 170)
    return false;
  return true;
}

// Cell info (0x04): big-endian mV per cell.
inline bool decodeJbdCells(const uint8_t *d, size_t n, jk::Sample &s) {
  unsigned cells = n / 2;
  if (!cells || cells > 32)
    return false;
  for (unsigned i = 0; i < 32; i++)
    s.cells[i] = 0;
  for (unsigned i = 0; i < cells; i++) {
    float v = be16(d + i * 2) * .001f;
    if (v < .5f || v > 5.f)
      return false;
    s.cells[i] = v;
  }
  s.count = cells;
  return true;
}

} // namespace bms
