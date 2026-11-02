// Simulated JK-PB parallel bus: master (DIP 0) + 4 slaves.
// Checks that a listening board files every frame under the right pack.
#include "../include/jk_rs485.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
static void put(uint8_t *b, unsigned i, uint32_t n, unsigned len = 4) { while (len--) { b[i++] = n; n >>= 8; } }
static std::vector<uint8_t> cellFrame(int pack) {
  std::vector<uint8_t> b(300, 0);
  b[0] = 0x55; b[1] = 0xAA; b[2] = 0xEB; b[3] = 0x90; b[4] = 2; b[5] = pack ? 5 : 0;
  for (int i = 0; i < 16; i++) put(b.data(), 6 + 2 * i, 3300 + pack * 10 + i, 2);
  put(b.data(), 70, 0xFFFF); // 32S layout, 16 cells
  float tot = 0; for (int i = 0; i < 16; i++) tot += (3300 + pack * 10 + i) * .001f;
  put(b.data(), 150, uint32_t(tot * 1000));
  put(b.data(), 158, uint32_t(int32_t(-1000 * (pack + 1))));
  b[173] = 50 + pack;
  put(b.data(), 178, 280000);
  b[299] = jk::sum(b.data(), 299);
  return b;
}
int main() {
  std::vector<uint8_t> bus;
  uint8_t f[16];
  auto add = [&](const uint8_t *p, int n) { bus.insert(bus.end(), p, p + n); };
  // master announces itself (request to 0) then its own frame
  int k = jk485::pbRequest(f, 0, 2); add(f, k);
  auto m = cellFrame(0); add(m.data(), 300);
  for (int a = 1; a <= 4; a++) {
    k = jk485::pbRequest(f, a, 2); add(f, k);
    const uint8_t ack[8] = {uint8_t(a), 0x10, 0x16, 0x20, 0x00, 0x01, 0x12, 0x34};
    auto fr = cellFrame(a); add(fr.data(), 300);
    add(ack, 8);
  }
  // a settings write from the master to pack 3 must be noticed as a write
  k = jk485::pbWrite(f, 3, 0x1078, 1); add(f, k);

  jk485::BusWatcher w;
  jk::Parser parser;
  uint8_t lastAddr = 0xFF; uint32_t lastAt = 0, now = 0;
  int got[16] = {0}; float soc[16] = {0}; int reqs = 0, writes = 0;
  jk::Sample samples[16]; bool fresh[16] = {false};
  for (uint8_t c : bus) {
    now++; // ~1 ms per byte is plenty for the window
    jk485::BusReq r;
    if (w.feed(c, r)) { if (r.write) writes++; else { reqs++; lastAddr = r.addr; lastAt = now; } }
    parser.feed(&c, 1, [&](const uint8_t *b) {
      uint8_t a = jk485::frameOwner(lastAddr, lastAt, now, 600);
      jk::Sample s;
      assert(jk::decode(b, true, s));
      got[a]++; soc[a] = s.soc; samples[a] = s; fresh[a] = true;
    });
  }
  printf("requests %d writes %d\n", reqs, writes);
  assert(reqs == 5 && writes == 1);
  for (int a = 0; a <= 4; a++) { printf("pack %d frames %d soc %.0f V %.2f I %.1f\n", a, got[a], soc[a], samples[a].voltage, samples[a].current); assert(got[a] == 1 && soc[a] == 50 + a); }
  auto t = jk485::bankTotals(samples, fresh, 16);
  printf("bank n=%d V=%.2f I=%.1f soc=%.1f cap=%.0f cmin=%.3f cmax=%.3f\n", t.n, t.v, t.i, t.soc, t.cap, t.cmin, t.cmax);
  assert(t.n == 5 && std::fabs(t.i + 15) < .01 && std::fabs(t.soc - 52) < .01 && std::fabs(t.cmin - 3.300f) < 1e-4 && std::fabs(t.cmax - 3.355f) < 1e-4);
  // frame long after the last request = master's own broadcast
  assert(jk485::frameOwner(4, 100, 5000) == 0);
  puts("PASS bank_test");
}
