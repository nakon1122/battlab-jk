#pragma once
// Writable BMS settings. Register numbers, scaling and settings-frame offsets
// follow syssi/esphome-jk-bms (jk_bms_ble number/switch) and
// syssi/esphome-ant-bms / esphome-jbd-bms (switches). Only JK exposes
// protection parameters here; ANT and JBD expose MOSFET switches only.
#include <array>
#include <cmath>
#include <stdint.h>
#include <string.h>

namespace bmsw {

enum : uint8_t {
  P_OVP, P_OVPR, P_UVP, P_UVPR, P_POWER_OFF, P_BAL_START, P_BAL_TRIGGER,
  P_MAX_BAL, P_MAX_CHG, P_MAX_DSG, P_CHG_OTP, P_DSG_OTP, P_CHG_UTP, P_MOS_OTP,
  P_CAPACITY, JK_PARAM_COUNT
};

struct JkParam {
  const char *name;  // short label for the screen
  const char *unit;
  uint8_t reg24;     // JK02_24S holding register
  uint8_t reg32;     // JK02_32S holding register
  uint8_t offset;    // byte offset in the 0x01 settings frame
  float factor;      // raw = value * factor
  bool isSigned;
  float step, min, max;
  uint8_t decimals;
};

// Bounds are deliberately conservative; anything outside is refused.
static const JkParam JK_PARAMS[JK_PARAM_COUNT] = {
    {"Cell OVP", "V", 0x04, 0x04, 18, 1000.f, false, .01f, 2.80f, 4.30f, 3},
    {"OVP recovery", "V", 0x05, 0x05, 22, 1000.f, false, .01f, 2.70f, 4.25f, 3},
    {"Cell UVP", "V", 0x02, 0x02, 10, 1000.f, false, .01f, 2.00f, 3.40f, 3},
    {"UVP recovery", "V", 0x03, 0x03, 14, 1000.f, false, .01f, 2.10f, 3.60f, 3},
    {"Power off", "V", 0x0B, 0x0B, 46, 1000.f, false, .01f, 2.00f, 3.30f, 3},
    {"Balance start", "V", 0x26, 0x22, 138, 1000.f, false, .01f, 2.80f, 4.20f, 3},
    {"Balance trigger", "V", 0x06, 0x06, 26, 1000.f, false, .001f, .003f, .100f, 3},
    {"Max balance I", "A", 0x13, 0x13, 78, 1000.f, false, .1f, .3f, 4.0f, 1},
    {"Max charge I", "A", 0x0C, 0x0C, 50, 1000.f, false, 1.f, 1.f, 300.f, 0},
    {"Max discharge I", "A", 0x0F, 0x0F, 62, 1000.f, false, 1.f, 1.f, 400.f, 0},
    {"Charge OTP", "C", 0x14, 0x14, 82, 10.f, false, 1.f, 30.f, 70.f, 0},
    {"Discharge OTP", "C", 0x16, 0x16, 90, 10.f, false, 1.f, 30.f, 80.f, 0},
    {"Charge UTP", "C", 0x18, 0x18, 98, 10.f, true, 1.f, -30.f, 15.f, 0},
    {"MOS OTP", "C", 0x1A, 0x1A, 106, 10.f, false, 1.f, 50.f, 100.f, 0},
    {"Capacity", "Ah", 0x20, 0x20, 130, 1000.f, false, 1.f, 1.f, 2000.f, 0},
};

// JK switch registers (value 1 = on, 0 = off, 4 bytes) and their offsets in
// the settings frame.
enum : uint8_t { SW_CHARGE, SW_DISCHARGE, SW_BALANCER, SW_COUNT };
static const uint8_t JK_SW_REG[SW_COUNT] = {0x1D, 0x1E, 0x1F};
static const uint8_t JK_SW_OFFSET[SW_COUNT] = {118, 122, 126};

struct JkSettings {
  float v[JK_PARAM_COUNT]{};
  bool sw[SW_COUNT]{};
  bool valid = false;
};

inline uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Parse a 300-byte JK 0x01 settings frame.
inline bool parseJkSettings(const uint8_t *b, JkSettings &out) {
  if (b[4] != 1) return false;
  JkSettings s;
  for (int i = 0; i < JK_PARAM_COUNT; i++) {
    const JkParam &p = JK_PARAMS[i];
    uint32_t raw = le32(b + p.offset);
    s.v[i] = (p.isSigned ? float(int32_t(raw)) : float(raw)) / p.factor;
  }
  for (int i = 0; i < SW_COUNT; i++) s.sw[i] = le32(b + JK_SW_OFFSET[i]) != 0;
  // sanity: a real settings frame has OVP above UVP and plausible values
  if (s.v[P_OVP] < 2.f || s.v[P_OVP] > 5.f || s.v[P_UVP] < 1.f ||
      s.v[P_UVP] >= s.v[P_OVP])
    return false;
  s.valid = true;
  out = s;
  return true;
}

// Check a new value against limits and the other current settings.
// Returns nullptr when OK, otherwise a short reason for the screen.
inline const char *checkJkValue(const JkSettings &cur, int idx, float v) {
  if (idx < 0 || idx >= JK_PARAM_COUNT) return "unknown setting";
  if (!cur.valid) return "settings not read yet";
  const JkParam &p = JK_PARAMS[idx];
  if (std::isnan(v) || v < p.min - 1e-6f || v > p.max + 1e-6f) return "out of safe range";
  const float m = .019f; // minimum gap between related voltages
  auto V = [&](int i) { return i == idx ? v : cur.v[i]; };
  if (V(P_OVP) < V(P_OVPR) + m) return "OVP must be above OVP recovery";
  if (V(P_UVPR) < V(P_UVP) + m) return "UVP recovery must be above UVP";
  if (V(P_OVPR) < V(P_UVPR) + m) return "OVP recovery must be above UVP recovery";
  if (V(P_UVP) < V(P_POWER_OFF) + m) return "UVP must be above power off";
  if (idx == P_BAL_START && (v > V(P_OVP))) return "balance start above OVP";
  return nullptr;
}

// JK write frame: AA 55 90 EB reg len value(LE 4) 0.. crc(sum of 0..18)
inline std::array<uint8_t, 20> jkWriteFrame(uint8_t reg, uint32_t value, uint8_t len = 4) {
  std::array<uint8_t, 20> f{};
  f[0] = 0xAA; f[1] = 0x55; f[2] = 0x90; f[3] = 0xEB;
  f[4] = reg;
  f[5] = len;
  f[6] = value; f[7] = value >> 8; f[8] = value >> 16; f[9] = value >> 24;
  uint8_t s = 0;
  for (int i = 0; i < 19; i++) s += f[i];
  f[19] = s;
  return f;
}
inline uint32_t jkRaw(int idx, float v) {
  const JkParam &p = JK_PARAMS[idx];
  return uint32_t(int32_t(std::lround(v * p.factor)));
}

// ---- ANT: authenticate, then write a switch register (value 0) ----
inline uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= *p++;
    for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}
inline std::array<uint8_t, 22> antAuthFrame() {
  std::array<uint8_t, 22> f{0x7E, 0xA1, 0x23, 0x6A, 0x01, 0x0C, '1', '2', '3', '4',
                            '5', '6', '7', '8', '9', 'a', 'b', 'c'};
  uint16_t c = crc16(f.data() + 1, 17);
  f[18] = c; f[19] = c >> 8; f[20] = 0xAA; f[21] = 0x55;
  return f;
}
inline std::array<uint8_t, 10> antWriteFrame(uint16_t addr, uint8_t value) {
  std::array<uint8_t, 10> f{0x7E, 0xA1, 0x51, uint8_t(addr), uint8_t(addr >> 8), value};
  uint16_t c = crc16(f.data() + 1, 5);
  f[6] = c; f[7] = c >> 8; f[8] = 0xAA; f[9] = 0x55;
  return f;
}
// on / off registers: charge, discharge, balancer
static const uint16_t ANT_SW_ON[SW_COUNT] = {0x0006, 0x0003, 0x000D};
static const uint16_t ANT_SW_OFF[SW_COUNT] = {0x0004, 0x0001, 0x000E};

// ---- JBD: register 0xE1, bit0 charge / bit1 discharge, 1 = OFF ----
inline std::array<uint8_t, 9> jbdMosFrame(bool chargeOn, bool dischargeOn) {
  uint8_t v = (chargeOn ? 0 : 1) | (dischargeOn ? 0 : 2);
  std::array<uint8_t, 9> f{0xDD, 0x5A, 0xE1, 0x02, 0x00, v};
  uint16_t c = 0;
  for (int i = 2; i < 6; i++) c -= f[i];
  f[6] = c >> 8; f[7] = c; f[8] = 0x77;
  return f;
}

} // namespace bmsw
