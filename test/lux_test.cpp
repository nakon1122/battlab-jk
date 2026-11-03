// g++ -std=c++17 -Iinclude -o lux_test test/lux_test.cpp && ./lux_test
#include "lux_local.h"
#include <cassert>
#include <cmath>
#include <vector>
using namespace luxl;
int main() {
  // request frame matches lxp-bridge build_read_inputs
  uint8_t o[38];
  buildReadInput(o, "2222222222", "5555555555", 0, 40);
  const uint8_t want[38] = {161, 26, 1, 0, 32, 0, 1, 194, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 18, 0, 0, 4, 53,
                            53, 53, 53, 53, 53, 53, 53, 53, 53, 0, 0, 40, 0, 42, 132};
  assert(memcmp(o, want, 38) == 0);
  // reply (protocol 2, regs 0-39)
  std::vector<uint8_t> r = {161, 26, 2, 0, 111, 0, 1, 194, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 97, 0, 1, 4, 53, 53,
        53, 53, 53, 53, 53, 53, 53, 53, 0, 0, 80, 32, 0, 0, 0, 0, 0, 0, 0, 250, 1, 77, 0, 0, 53, 0,
        0, 0, 0, 0, 0, 128, 13, 0, 0, 114, 9, 0, 16, 132, 0, 142, 19, 0, 0, 198, 13, 202, 5, 232,
        3, 114, 9, 0, 10, 80, 112, 142, 19, 0, 0, 0, 0, 0, 0, 36, 15, 0, 0, 0, 0, 0, 0, 91, 0, 83,
        0, 87, 0, 114, 0, 0, 0, 1, 0, 102, 0, 174, 14, 183, 12, 71, 187};
  // split across pushes with junk in front
  Splitter sp; Inv x; char dl[11];
  uint8_t junk[3] = {1, 2, 0xA1};
  sp.push(junk, 3); sp.push(r.data(), 50);
  assert(sp.frame() == 0);
  sp.push(r.data() + 50, r.size() - 50);
  size_t n = sp.frame();
  assert(n == r.size());
  assert(decode(sp.buf, n, x, dl) == INPUT);
  sp.drop(n); assert(sp.n == 0);
  assert(!strcmp(x.sn, "5555555555") && !strcmp(x.dl, "2222222222") && (x.have & 1));
  Values v = values(x);
  assert(v.status == 0x20);
  assert(fabsf(v.v - 50.6f) < 0.01f);       // r4 = 506
  assert(v.soc == 77);
  assert(v.ppv == 0);
  assert(v.p == 3456);                      // r10 = 128+13*256, r11 = 0
  assert(fabsf(v.vac - 241.8f) < 0.01f);
  // corrupt crc -> BAD
  r[r.size() - 1] ^= 1;
  Inv y; assert(decode(r.data(), r.size(), y) == BAD);
  // heartbeat
  uint8_t hb[19] = {161, 26, 2, 0, 13, 0, 1, 193, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 0};
  assert(decode(hb, 19, y, dl) == HEARTBEAT && !strcmp(dl, "2222222222"));
  // our own request echoed -> not data
  assert(decode(o, 38, y) == OTHER);
  assert(validSerial("61730V3039") && !validSerial("617\x01") && !validSerial("123"));
  puts("lux tests ok");
}
