#include "jk_protocol.h"
#include <cassert>
#include <iostream>
#include <vector>
void put(std::array<uint8_t, 300> &b, unsigned i, uint32_t n,
         unsigned len = 4) {
  while (len--) {
    b[i++] = n;
    n >>= 8;
  }
}
std::array<uint8_t, 300> fixture(bool large) {
  std::array<uint8_t, 300> b{};
  b[0] = 0x55;
  b[1] = 0xaa;
  b[2] = 0xeb;
  b[3] = 0x90;
  b[4] = 2;
  for (int i = 0; i < 16; i++)
    put(b, 6 + 2 * i, 3300, 2);
  put(b, large ? 70 : 54, 65535);
  unsigned o = large ? 32 : 0;
  put(b, 118 + o, 52800);
  put(b, 126 + o, uint32_t(-12500));
  b[141 + o] = 75;
  put(b, 142 + o, 75000);
  put(b, 146 + o, 100000);
  put(b, 150 + o, 123);
  put(b, 130 + o, uint16_t(-50), 2);
  b[299] = jk::sum(b.data(), 299);
  return b;
}
int main() {
  for (bool large : {false, true}) {
    auto b = fixture(large);
    jk::Sample s;
    assert(jk::decode(b.data(), large, s));
    assert(s.count == 16);
    assert(std::fabs(s.voltage - 52.8) < .001);
    assert(std::fabs(s.current + 12.5f) < .001);
    assert(s.temp1 == -5);
    assert(s.cycles == 123);
    assert(std::fabs(s.capacity - 75) < .001 && std::fabs(s.full - 100) < .001);
    assert(!jk::decode(b.data(), !large, s));
    for (unsigned split = 0; split <= 300; split++) {
      jk::Parser p;
      int calls = 0;
      auto cb = [&](const uint8_t *f) {
        calls++;
        assert(jk::decode(f, large, s));
      };
      p.feed(b.data(), split, cb);
      p.feed(b.data() + split, 300 - split, cb);
      assert(calls == 1 && p.good == 1);
    }
    jk::Parser p;
    int calls = 0;
    auto cb = [&](const uint8_t *) { calls++; };
    auto bad = b;
    bad[20] ^= 1;
    p.feed(bad.data(), 300, cb);
    assert(calls == 0 && p.bad == 1);
    std::vector<uint8_t> v = {0, 3, 0x55, 0, 0x55};
    v.insert(v.end(), b.begin(), b.end());
    v.insert(v.end(), 20, 0);
    v.insert(v.end(), b.begin(), b.end());
    for (auto c : v)
      p.feed(&c, 1, cb);
    assert(calls == 2);
    b[141 + (large ? 32 : 0)] = 101;
    assert(!jk::decode(b.data(), large, s));
  }
  for (bool large : {false, true}) {
    auto b = fixture(large);
    jk::Sample sample;
    jk::AutoDetector detector;
    assert(jk::detect(b.data(), sample) == (large ? 32 : 24));
    assert(detector.accept(b.data(), sample) == 0);
    assert(detector.accept(b.data(), sample) == 0);
    assert(detector.accept(b.data(), sample) == (large ? 32 : 24));
    b[141 + (large ? 32 : 0)] = 101;
    assert(detector.accept(b.data(), sample) == 0);
    b = fixture(large);
    assert(detector.accept(b.data(), sample) == 0);
    auto alternate = fixture(!large);
    assert(detector.accept(alternate.data(), sample) == 0);
    detector.reset();
    assert(detector.accept(b.data(), sample) == 0);
  }
  // A crafted frame that satisfies both layouts must stay ambiguous.
  auto ambiguous = fixture(false);
  put(ambiguous, 70, 65535);
  put(ambiguous, 150, 52800);
  put(ambiguous, 158, 0);
  ambiguous[173] = 75;
  jk::Sample ambiguousSample;
  assert(jk::decode(ambiguous.data(), false, ambiguousSample));
  assert(jk::decode(ambiguous.data(), true, ambiguousSample));
  assert(jk::detect(ambiguous.data(), ambiguousSample) == 0);
  std::array<uint8_t, 300> invalid{};
  jk::Sample ignored;
  assert(jk::detect(invalid.data(), ignored) == 0);
  auto r = jk::request(0x97);
  assert(r[19] == jk::sum(r.data(), 19) && r[5] == 0);
  jk::Parameters p;
  std::array<uint8_t, 300> b{};
  b[4] = 1;
  put(b, 10, 2800);
  put(b, 18, 3650);
  assert(jk::parameters(b.data(), p));
  put(b, 10, 4000);
  assert(!jk::parameters(b.data(), p));
  std::cout << "PASS: Auto detection, three-frame verification, mismatch "
               "reset, both JK02 layouts, 602 split cases, checksum rejection, "
               "resync, tails, signed values, mismatch, settings and request "
               "checksum\n";
}
