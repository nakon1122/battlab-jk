#pragma once
#include <array>
#include <cmath>
#include <stddef.h>
#include <stdint.h>
namespace jk {
inline uint16_t u16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t u32(const uint8_t *p) {
  return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16);
}
inline uint8_t sum(const uint8_t *p, size_t n) {
  uint8_t s = 0;
  while (n--)
    s += *p++;
  return s;
}
inline std::array<uint8_t, 20> request(uint8_t cmd) {
  std::array<uint8_t, 20> b{};
  b[0] = 0xaa;
  b[1] = 0x55;
  b[2] = 0x90;
  b[3] = 0xeb;
  b[4] = cmd;
  b[19] = sum(b.data(), 19);
  return b;
}
struct Sample {
  float cells[32]{}, voltage = 0, current = 0, soc = 0, capacity = 0, temp1 = 0,
                     temp2 = 0, mos = 0;
  float full = 0; // full / nominal capacity (Ah), 0 = unknown
  uint32_t cycles = 0;
  unsigned count = 0;
  bool charge = false, discharge = false;
};
struct Parameters {
  float uvp = 0, uvpr = 0, ovp = 0, ovpr = 0, balance = 0, charge = 0,
        discharge = 0;
};
inline bool decode(const uint8_t *b, bool s32, Sample &out) {
  if (b[4] != 2) {
    return false;
  }
  Sample s;
  unsigned off = s32 ? 32 : 0;
  uint32_t mask = u32(b + (s32 ? 70 : 54));
  for (unsigned i = 0; i < (s32 ? 32u : 24u); i++) {
    float v = u16(b + 6 + i * 2) * .001f;
    s.cells[i] = v;
    if (mask & (uint32_t(1) << i)) {
      if (v < .5f || v > 5.0f)
        return false;
      s.count++;
    }
  }
  if (!s.count)
    return false;
  s.voltage = u32(b + 118 + off) * .001f;
  s.current = int32_t(u32(b + 126 + off)) * .001f;
  s.soc = b[141 + off];
  s.capacity = u32(b + 142 + off) * .001f; // remaining Ah
  s.full = u32(b + 146 + off) * .001f;     // full / nominal Ah
  s.cycles = u32(b + 150 + off);
  s.temp1 = int16_t(u16(b + 130 + off)) * .1f;
  s.temp2 = int16_t(u16(b + 132 + off)) * .1f;
  s.mos = int16_t(u16(b + (s32 ? 112 + off : 134))) * .1f;
  s.charge = b[166 + off] != 0;
  s.discharge = b[167 + off] != 0;
  float total = 0;
  for (unsigned i = 0; i < 32; i++)
    if (mask & (uint32_t(1) << i))
      total += s.cells[i];
    else
      s.cells[i] = 0;
  if (s.soc > 100 || s.voltage < 1 || s.voltage > 170 ||
      std::fabs(s.voltage - total) > std::fmax(2.f, total * .05f) ||
      std::fabs(s.current) > 2000)
    return false;
  out = s;
  return true;
}
// Only select a layout when exactly one decoder accepts the frame.
inline int detect(const uint8_t *b, Sample &out) {
  Sample a, c;
  bool small = decode(b, false, a), large = decode(b, true, c);
  if (small == large)
    return 0;
  out = small ? a : c;
  return small ? 24 : 32;
}
class AutoDetector {
  int previous = 0;
  unsigned streak = 0;

public:
  void reset() {
    previous = 0;
    streak = 0;
  }
  int accept(const uint8_t *b, Sample &out) {
    int layout = detect(b, out);
    if (!layout) {
      reset();
      return 0;
    }
    streak = layout == previous ? streak + 1 : 1;
    previous = layout;
    return streak >= 3 ? layout : 0;
  }
};
inline bool parameters(const uint8_t *b, Parameters &p) {
  if (b[4] != 1)
    return false;
  Parameters q;
  q.uvp = u32(b + 10) * .001f;
  q.uvpr = u32(b + 14) * .001f;
  q.ovp = u32(b + 18) * .001f;
  q.ovpr = u32(b + 22) * .001f;
  q.balance = u32(b + 26) * .001f;
  q.charge = u32(b + 50) * .001f;
  q.discharge = u32(b + 62) * .001f;
  if (q.uvp < .5f || q.ovp > 5 || q.uvp >= q.ovp) {
    return false;
  }
  p = q;
  return true;
}
// Streaming synchronizer handles fragmented headers, multiple frames and
// 320-byte tails.
class Parser {
  std::array<uint8_t, 300> b{};
  size_t n = 0;

public:
  uint32_t good = 0, bad = 0;
  void reset() { n = 0; }
  template <class F> void feed(const uint8_t *p, size_t len, F frame) {
    static const uint8_t h[] = {0x55, 0xaa, 0xeb, 0x90};
    while (len--) {
      uint8_t c = *p++;
      if (n < 4) {
        if (c == h[n])
          b[n++] = c;
        else {
          n = c == h[0] ? 1 : 0;
          if (n)
            b[0] = c;
        }
        continue;
      }
      b[n++] = c;
      if (n == 300) {
        if (sum(b.data(), 299) == b[299]) {
          good++;
          frame(b.data());
          n = 0;
        } else {
          bad++;
          auto copy = b;
          n = 0;
          feed(copy.data() + 1, 299, frame);
        }
      }
    }
  }
};
} // namespace jk
