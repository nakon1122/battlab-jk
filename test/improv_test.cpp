// Improv serial: packet round trip, garbage tolerance, Wi-Fi settings decode
#include "../include/improv_serial.h"
#include <cassert>
#include <cstdio>
int main() {
  using namespace improv;
  // host -> board: RPC "send Wi-Fi settings" ssid "MyHome", password "pass1234"
  std::vector<uint8_t> rpc = {C_WIFI, 0, 6, 'M', 'y', 'H', 'o', 'm', 'e', 8, 'p', 'a', 's', 's', '1', '2', '3', '4'};
  rpc[1] = uint8_t(rpc.size() - 2);
  auto pk = packet(T_RPC, rpc);
  std::string noise = "[CLOUD] ok\r\nIMPRO junk I";
  Parser p;
  uint8_t type = 0;
  std::vector<uint8_t> d;
  int got = 0;
  for (char c : noise) got += p.feed(uint8_t(c), type, d);
  for (uint8_t c : pk) got += p.feed(c, type, d);
  assert(got == 1 && type == T_RPC && d == rpc);
  std::string ss, pw;
  assert(wifiSettings(d, ss, pw) && ss == "MyHome" && pw == "pass1234");
  // bad checksum is ignored
  auto bad = pk; bad.back() ^= 1;
  got = 0;
  for (uint8_t c : bad) got += p.feed(c, type, d);
  assert(got == 0);
  // board -> host result: device info strings
  auto r = result(C_INFO, {"BATTLAB", "2.8.1-s3", "ESP32-S3", "BATTLAB-A1B2C3"});
  assert(r[0] == C_INFO && r[1] == r.size() - 2 && r[2] == 7);
  auto out = packet(T_RESULT, r);
  uint8_t cs = 0;
  for (size_t i = 0; i + 1 < out.size(); i++) cs += out[i];
  assert(out.back() == cs && out[6] == 1 && out[7] == T_RESULT && out[8] == r.size());
  // open network: empty password
  std::vector<uint8_t> o = {C_WIFI, 0, 3, 'C', 'a', 'f', 0};
  o[1] = uint8_t(o.size() - 2);
  assert(wifiSettings(o, ss, pw) && ss == "Caf" && pw.empty());
  puts("PASS improv_test");
}
