#pragma once
// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/): lets the
// website's USB installer (ESP Web Tools) ask for the home Wi-Fi right after
// flashing, so a new board can be set up from a computer without a phone.
//   packet: "IMPROV" | version 1 | type | length | data[length] | checksum
//   checksum = low byte of the sum of every byte before it
//   type 1 current state, 2 error, 3 RPC (host -> board), 4 RPC result
//   RPC data: command | length | payload ; result: command | length | strings
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
namespace improv {
enum : uint8_t { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum : uint8_t { S_AUTHORIZED = 2, S_PROVISIONING = 3, S_PROVISIONED = 4 };
enum : uint8_t { E_NONE = 0, E_INVALID = 1, E_UNKNOWN_RPC = 2, E_CONNECT = 3, E_UNKNOWN = 0xFF };
enum : uint8_t { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

inline std::vector<uint8_t> packet(uint8_t type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> p = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, uint8_t(data.size())};
  p.insert(p.end(), data.begin(), data.end());
  uint8_t cs = 0;
  for (uint8_t b : p) cs += b;
  p.push_back(cs);
  return p;
}
inline std::vector<uint8_t> result(uint8_t cmd, const std::vector<std::string> &strs) {
  std::vector<uint8_t> d = {cmd, 0};
  for (auto &s : strs) {
    d.push_back(uint8_t(s.size()));
    d.insert(d.end(), s.begin(), s.end());
  }
  d[1] = uint8_t(d.size() - 2);
  return d;
}
// Byte-by-byte parser; ignores anything that is not a valid packet (log text, JSON lines).
class Parser {
  uint8_t b[300];
  size_t n = 0;

public:
  // returns true when a complete, valid packet ended with this byte
  bool feed(uint8_t c, uint8_t &type, std::vector<uint8_t> &data) {
    static const char H[] = "IMPROV";
    if (n < 6) {
      if (c == uint8_t(H[n])) b[n++] = c;
      else n = c == 'I' ? (b[0] = c, 1) : 0;
      return false;
    }
    b[n++] = c;
    if (n == 7 && c != 1) { n = 0; return false; }      // version
    if (n < 9) return false;
    size_t len = b[8];
    if (n < 9 + len + 1) { if (n >= sizeof b) n = 0; return false; }
    uint8_t cs = 0;
    for (size_t i = 0; i < 9 + len; i++) cs += b[i];
    bool ok = cs == b[9 + len];
    if (ok) { type = b[7]; data.assign(b + 9, b + 9 + len); }
    n = 0;
    return ok;
  }
};
// Decode the Wi-Fi settings RPC payload: ssid_len ssid pass_len pass
inline bool wifiSettings(const std::vector<uint8_t> &rpc, std::string &ssid, std::string &pass) {
  if (rpc.size() < 3 || rpc[0] != C_WIFI) return false;
  size_t len = rpc[1], p = 2;
  if (rpc.size() < 2 + len || len < 1) return false;
  size_t sl = rpc[p++];
  if (p + sl + 1 > 2 + len) return false;
  ssid.assign(rpc.begin() + p, rpc.begin() + p + sl);
  p += sl;
  size_t pl = rpc[p++];
  if (p + pl > 2 + len) return false;
  pass.assign(rpc.begin() + p, rpc.begin() + p + pl);
  return !ssid.empty();
}
} // namespace improv
