// g++ -std=c++17 -Iinclude -o inv_test test/inv_test.cpp && ./inv_test
#include "inv_local.h"
#include <cassert>
#include <vector>
using namespace invl;
static bool near(float a, float b, float e = 0.011f) { return fabsf(a - b) < e; }
// independent builder of a Modbus RTU read reply
static std::vector<uint8_t> mbReply(uint8_t slave, uint8_t fc, const std::vector<uint16_t> &regs) {
  std::vector<uint8_t> f = {slave, fc, (uint8_t)(regs.size() * 2)};
  for (auto r : regs) { f.push_back(r >> 8); f.push_back(r & 0xFF); }
  uint16_t c = crcModbus(f.data(), f.size());
  f.push_back(c & 0xFF); f.push_back(c >> 8);
  return f;
}
// independent builder of a Solarman V5 reply (pysolarmanv5 layout)
static std::vector<uint8_t> v5Reply(uint32_t serial, uint8_t seq, const std::vector<uint8_t> &mb) {
  std::vector<uint8_t> f = {0xA5};
  uint16_t len = 14 + mb.size();
  f.push_back(len & 0xFF); f.push_back(len >> 8);
  f.push_back(0x10); f.push_back(0x15); f.push_back(seq); f.push_back(0x77);
  for (int i = 0; i < 4; i++) f.push_back((serial >> (8 * i)) & 0xFF);
  f.push_back(0x02); f.push_back(0x01);
  for (int i = 0; i < 12; i++) f.push_back(i);
  f.insert(f.end(), mb.begin(), mb.end());
  uint8_t cs = 0;
  for (size_t i = 1; i < f.size(); i++) cs += f[i];
  f.push_back(cs); f.push_back(0x15);
  return f;
}
int main() {
  // --- Modbus RTU: textbook request 01 03 00 00 00 0A C5 CD
  uint8_t q[8];
  assert(mbReq(q, 1, 3, 0, 10) == 8);
  const uint8_t wantQ[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
  assert(!memcmp(q, wantQ, 8));
  uint16_t out[130];
  auto rp = mbReply(1, 3, {0x1234, 0xFFFE});
  assert(mbParse(rp.data(), rp.size(), 1, 3, 2, out) == 2 && out[0] == 0x1234 && out[1] == 0xFFFE);
  rp.push_back(0); rp.push_back(0);                       // Deye loggers append two bytes
  assert(mbParse(rp.data(), rp.size(), 1, 3, 2, out) == 2);
  rp[4] ^= 1; assert(mbParse(rp.data(), rp.size(), 1, 3, 2, out) == -5);
  const uint8_t exc[5] = {1, 0x83, 2, 0xC0, 0xF1};
  assert(mbParse(exc, 5, 1, 3, 2, out) == -12);
  assert(mbParse(mbReply(5, 3, {1}).data(), 7, 0, 3, 1, out) == 1);   // slave 0 = any address

  // --- Solarman V5: request bytes as pysolarmanv5 builds them (serial 1712345678, seq 1, read 0x0003 x 5)
  uint8_t mb[8], fr[64];
  mbReq(mb, 1, 3, 3, 5);
  size_t n = v5Req(fr, 1712345678u, 1, mb, 8);
  assert(n == 36 && fr[0] == 0xA5 && fr[1] == 23 && fr[2] == 0 && fr[3] == 0x10 && fr[4] == 0x45 && fr[5] == 1 && fr[6] == 0);
  assert(fr[7] == 0x4E && fr[8] == 0x52 && fr[9] == 0x10 && fr[10] == 0x66);   // 1712345678 = 0x6610524E little endian
  assert(fr[11] == 0x02 && fr[26] == 0x01 && fr[27] == 0x03 && fr[35] == 0x15);
  uint8_t cs = 0; for (size_t i = 1; i < 34; i++) cs += fr[i];
  assert(fr[34] == cs);
  auto vr = v5Reply(1712345678u, 1, mbReply(1, 3, {0x3233, 0x3031, 0x3233, 0x3435, 0x3637}));
  const uint8_t *m; size_t ml;
  assert(v5Total(vr.data(), 3) == vr.size());
  assert(v5Parse(vr.data(), vr.size(), 1712345678u, 1, &m, &ml) == 1);
  assert(mbParse(m, ml, 1, 3, 5, out) == 5 && out[0] == 0x3233);
  char sn[24];
  assert(regAscii(out, 5, sn, sizeof sn) && !strcmp(sn, "2301234567"));
  assert(v5Parse(vr.data(), vr.size(), 1712345678u, 2, &m, &ml) == 0);      // answer to an older request
  assert(v5Parse(vr.data(), vr.size(), 99u, 1, &m, &ml) == -4);              // other logger
  vr[vr.size() - 2] ^= 1; assert(v5Parse(vr.data(), vr.size(), 1712345678u, 1, &m, &ml) == -3);
  uint16_t bad[2] = {0x0001, 0x0203};
  assert(!regAscii(bad, 2, sn, sizeof sn));

  // --- Modbus TCP
  uint8_t t[12];
  assert(mbtcpReq(t, 7, 1, 4, 33139, 1) == 12 && t[1] == 7 && t[5] == 6 && t[7] == 4 && t[8] == 0x81 && t[9] == 0x73);
  const uint8_t tr[11] = {0, 7, 0, 0, 0, 5, 1, 4, 2, 0, 71};
  assert(mbtcpTotal(tr, 6) == 11 && mbtcpParse(tr, 11, 7, 4, 1, out) == 1 && out[0] == 71);
  assert(mbtcpParse(tr, 11, 8, 4, 1, out) == -1);

  // --- Deye single phase hybrid
  Regs R; Val v;
  const Block *b; int nb = blocks(M_DEYE_1P, &b);
  assert(nb == 2);
  for (int i = 0; i < nb; i++) { uint16_t *d = R.add(b[i].start, b[i].count); assert(d); memset(d, 0, b[i].count * 2); }
  auto set = [&](uint16_t a, uint16_t val) { for (int i = 0; i < R.nb; i++) if (a >= R.blk[i].start && a < R.blk[i].start + R.blk[i].count) { R.d[R.off[i] + a - R.blk[i].start] = val; return; } assert(false); };
  set(59, 2); set(70, 31); set(71, 12); set(76, 5); set(84, 96); set(108, 123); set(109, 3105); set(91, 1415);
  set(152, 2301); set(169, (uint16_t)-250); set(178, 800); set(182, 1265); set(183, 5290); set(184, 66);
  set(186, 1500); set(187, 900); set(190, (uint16_t)-1100); set(191, (uint16_t)-2080);
  assert(decode(M_DEYE_1P, R, v));
  assert(v.soc == 66 && near(v.v, 52.9f) && v.p == 1100 && near(v.i, 20.8f) && v.ppv == 2400 && v.pload == 800 && v.pgrid == -250);
  assert(near(v.t1, 26.5f) && near(v.mos, 41.5f) && near(v.epv, 12.3f) && near(v.eload, 9.6f) && near(v.vac, 230.1f) && v.status == 2);
  assert(deyeModel(0x0003) == M_DEYE_1P && deyeModel(0x0300) == M_DEYE_1P && deyeModel(0x0005) == M_DEYE_3P && deyeModel(0x0600) == M_DEYE_3P);
  assert(deyeHv(0x0006) && deyeHv(0x0600) && !deyeHv(0x0005));
  set(184, 250); assert(!decode(M_DEYE_1P, R, v));        // impossible SOC = wrong map
  // a register outside the blocks is a miss, not a silent zero
  R.miss = false; R.r(400); assert(R.miss);

  // --- Deye three phase (LV)
  R.clear(); nb = blocks(M_DEYE_3P, &b);
  for (int i = 0; i < nb; i++) { uint16_t *d = R.add(b[i].start, b[i].count); assert(d); memset(d, 0, b[i].count * 2); }
  set(586, 1250); set(587, 5120); set(588, 80); set(590, 2000); set(625, 150); set(653, 3100); set(672, 2500); set(673, 2400); set(529, 201); set(541, 1380);
  Val v3; assert(decode(M_DEYE_3P, R, v3, false));
  assert(v3.soc == 80 && near(v3.v, 51.2f) && v3.p == -2000 && v3.ppv == 4900 && v3.pload == 3100 && v3.pgrid == 150 && near(v3.epv, 20.1f));

  // --- Sofar G3
  R.clear(); nb = blocks(M_SOFAR_G3, &b);
  for (int i = 0; i < nb; i++) { uint16_t *d = R.add(b[i].start, b[i].count); assert(d); memset(d, 0, b[i].count * 2); }
  set(1160, (uint16_t)-30); set(1199, 120); set(1414, 100); set(1417, 80); set(1540, 512); set(1542, 50); set(1544, 73); set(1668, 0); set(1669, 1234);
  Val vs; assert(decode(M_SOFAR_G3, R, vs));
  assert(vs.pgrid == 300 && vs.pload == 1200 && vs.ppv == 1800 && near(vs.v, 51.2f) && vs.p == 500 && vs.soc == 73 && near(vs.epv, 12.34f));

  // --- Solis hybrid
  R.clear(); nb = blocks(M_SOLIS, &b);
  for (int i = 0; i < nb; i++) { uint16_t *d = R.add(b[i].start, b[i].count); assert(d); memset(d, 0, b[i].count * 2); }
  set(33057, 0); set(33058, 3200); set(33133, 508); set(33134, 100); set(33135, 1); set(33139, 71); set(33147, 900); set(33148, 100);
  set(33149, 0); set(33150, 508); set(33257, 0xFFFF); set(33258, (uint16_t)-400); set(33035, 81);
  Val vl; assert(decode(M_SOLIS, R, vl));
  assert(vl.ppv == 3200 && near(vl.v, 50.8f) && near(vl.i, -10.0f) && vl.p == -508 && vl.soc == 71 && vl.pload == 1000 && vl.pgrid == 400 && near(vl.epv, 8.1f));

  // --- Eybond transport
  uint8_t e[64];
  const uint8_t pl[3] = {'A', 'B', 'C'};
  assert(eyFrame(e, 0x0102, 2452, 255, EY_FORWARD, pl, 3) == 11);
  EyHdr h; assert(eyHeader(e, 8, h) && h.tid == 0x0102 && h.devcode == 2452 && h.wire == 5 && h.addr == 255 && h.fc == 4 && h.total() == 11);
  assert(eyHeartbeat(e, 1, 26, 11, 10, 9, 30, 0, 60) == 16 && e[5] == 10 && e[7] == 1 && e[8] == 26 && e[15] == 60);

  // --- PI30: published command bytes
  uint8_t c[16];
  assert(pi30Cmd(c, "QPIGS") == 8 && c[5] == 0xB7 && c[6] == 0xA9 && c[7] == '\r');
  assert(pi30Cmd(c, "QPI") == 6 && c[3] == 0xBE && c[4] == 0xAC);
  assert(pi30Cmd(c, "QMOD") == 7 && c[4] == 0x49 && c[5] == 0xC1);
  assert(pi30Cmd(c, "QID") == 6 && c[3] == 0xD6 && c[4] == 0xEA);
  assert(pi30Cmd(c, "POP02") == 0);                       // a setting command is never built
  // QPIGS answer of a 24 V Axpert-type inverter, 21 fields
  const char *txt = "229.8 49.9 230.1 49.9 0437 0412 013 380 26.40 012 087 0041 0015 118.2 26.40 00000 00010110 00 00 00750 010";
  std::vector<uint8_t> f = {'('};
  f.insert(f.end(), txt, txt + strlen(txt));
  uint8_t cr[2]; pi30Crc(f.data(), f.size(), cr);
  f.push_back(cr[0]); f.push_back(cr[1]); f.push_back('\r');
  char body[160];
  assert(pi30Parse(f.data(), f.size(), body, sizeof body) && !strcmp(body, txt));
  Val vp; assert(pi30Qpigs(body, 'B', vp));
  assert(near(vp.v, 26.4f) && vp.soc == 87 && near(vp.i, 12) && near(vp.p, 316.8f, 0.1f) && vp.ppv == 750 && vp.pload == 412 && vp.pgrid == 0 && vp.gridEst && vp.mos == 41);
  Val vg; assert(pi30Qpigs("229.8 49.9 230.1 49.9 0437 0900 013 380 26.40 000 087 0041 0000 000.0 26.40 00000 00010000 00 00 00000 010", 'L', vg));
  assert(vg.pgrid == 900);                                // line mode, nothing from PV or battery: all from the grid
  f[10] ^= 1; assert(!pi30Parse(f.data(), f.size(), body, sizeof body));
  assert(!pi30Qpigs("1 2 3", 0, vp));

  // --- Solarman discovery answer
  char ip[20]; uint32_t ser;
  assert(solarmanDiscovery("192.168.1.50,E8FDF8AABBCC,2712345678", ip, sizeof ip, ser) && !strcmp(ip, "192.168.1.50") && ser == 2712345678u);
  assert(!solarmanDiscovery("garbage", ip, sizeof ip, ser));
  puts("inv_test ok");
  return 0;
}
