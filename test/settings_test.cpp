// Native checks for include/bms_settings.h (ANT / JBD settings frames)
#include "../include/bms_settings.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static bool eq(const uint8_t *a, int n, const char *hex) {
  char b[128] = {0};
  for (int i = 0; i < n; i++) sprintf(b + strlen(b), "%02X", a[i]);
  if (strcmp(b, hex)) { printf("got %s want %s\n", b, hex); return false; }
  return true;
}
int main() {
  uint8_t f[32];
  assert(eq(f, bset::jbdFactory(f), "DD5A00025678FF3077"));
  assert(eq(f, bset::jbdExit(f, true), "DD5A01022828FFAD77"));
  assert(eq(f, bset::jbdRead(f, 0x03), "DDA50300FFFD77"));
  assert(eq(f, bset::antFrame(f, 0x01, 0, 0xBE, nullptr, 0), "7EA1010000BE1855AA55"));
  assert(eq(f, bset::antFrame(f, 0x02, 0x026C, 0x20, nullptr, 0), "7EA1026C022058C4AA55"));
  const auto &doc = bset::JBD[9];
  assert(bset::encode(doc, 100) == -10000);
  assert(bset::encode(bset::JBD[10], 55) == 3281);
  float cur[64] = {4.25f, 4.15f};
  assert(bset::check(3, cur, 3, 1, 4.30f) != nullptr); // release above protection
  assert(bset::check(3, cur, 3, 0, 4.10f) != nullptr); // protection below release
  assert(bset::check(3, cur, 3, 1, 4.10f) == nullptr);
  printf("settings test OK (%d JBD, %d ANT)\n", bset::JBD_N, bset::ANT_N);
}
