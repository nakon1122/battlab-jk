# Generates include/bms_settings.h (firmware) and cloud/web/params.js (website)
# from one table, so the screen, the board web page and the cloud agree.
from pathlib import Path
import json
ROOT = Path(__file__).resolve().parents[1]
# key, reg, short name (screen), Thai name (web), unit, scale, offset, step, min, max, decimals, width, signed, below, above
JBD = [
 ('covp',0x24,'Cell OVP','แรงดันเซลล์สูงสุด (OVP)','V',0.001,0,0.01,2.5,4.5,3,2,0,None,None),
 ('covpr',0x25,'Cell OVP release','คืนค่าจาก OVP','V',0.001,0,0.01,2.4,4.4,3,2,0,'covp',None),
 ('cuvp',0x26,'Cell UVP','แรงดันเซลล์ต่ำสุด (UVP)','V',0.001,0,0.01,1.5,3.6,3,2,0,None,None),
 ('cuvpr',0x27,'Cell UVP release','คืนค่าจาก UVP','V',0.001,0,0.01,1.6,3.8,3,2,0,None,'cuvp'),
 ('povp',0x20,'Pack OVP','แรงดันรวมสูงสุด','V',0.01,0,0.1,5,150,1,2,0,None,None),
 ('povpr',0x21,'Pack OVP release','คืนค่าแรงดันรวมสูงสุด','V',0.01,0,0.1,5,150,1,2,0,'povp',None),
 ('puvp',0x22,'Pack UVP','แรงดันรวมต่ำสุด','V',0.01,0,0.1,3,140,1,2,0,None,None),
 ('puvpr',0x23,'Pack UVP release','คืนค่าแรงดันรวมต่ำสุด','V',0.01,0,0.1,3,145,1,2,0,None,'puvp'),
 ('coc',0x28,'Charge OC','กระแสชาร์จสูงสุด','A',0.01,0,1,1,500,0,2,0,None,None),
 ('doc',0x29,'Discharge OC','กระแสจ่ายสูงสุด','A',-0.01,0,1,1,600,0,2,1,None,None),
 ('cot',0x18,'Charge high temp','อุณหภูมิสูงสุดขณะชาร์จ','C',0.1,-273.1,1,20,90,0,2,0,None,None),
 ('cotr',0x19,'Chg high temp rel','คืนค่าอุณหภูมิสูงขณะชาร์จ','C',0.1,-273.1,1,15,85,0,2,0,'cot',None),
 ('cut',0x1A,'Charge low temp','อุณหภูมิต่ำสุดขณะชาร์จ','C',0.1,-273.1,1,-40,30,0,2,0,None,None),
 ('cutr',0x1B,'Chg low temp rel','คืนค่าอุณหภูมิต่ำขณะชาร์จ','C',0.1,-273.1,1,-35,35,0,2,0,None,'cut'),
 ('dot',0x1C,'Dsg high temp','อุณหภูมิสูงสุดขณะจ่าย','C',0.1,-273.1,1,20,100,0,2,0,None,None),
 ('dotr',0x1D,'Dsg high temp rel','คืนค่าอุณหภูมิสูงขณะจ่าย','C',0.1,-273.1,1,15,95,0,2,0,'dot',None),
 ('dut',0x1E,'Dsg low temp','อุณหภูมิต่ำสุดขณะจ่าย','C',0.1,-273.1,1,-40,30,0,2,0,None,None),
 ('dutr',0x1F,'Dsg low temp rel','คืนค่าอุณหภูมิต่ำขณะจ่าย','C',0.1,-273.1,1,-35,35,0,2,0,None,'dut'),
 ('bals',0x2A,'Balance start','เริ่มบาลานซ์ที่','V',0.001,0,0.01,2.5,4.4,3,2,0,None,None),
 ('bald',0x2B,'Balance delta','บาลานซ์เมื่อต่างกัน','V',0.001,0,0.001,0.003,0.2,3,2,0,None,None),
 ('dcap',0x10,'Design capacity','ความจุออกแบบ','Ah',0.01,0,1,1,650,0,2,0,None,None),
 ('fcap',0x11,'Full capacity','ความจุเต็ม (รอบ)','Ah',0.01,0,1,1,650,0,2,0,None,None),
]
A = []
def a(key,reg,name,th,unit,scale,step,mn,mx,dec,width=2,below=None,above=None):
    A.append((key,reg,name,th,unit,scale,0,step,mn,mx,dec,width,0,below,above))
a('covp',0x00,'Cell OVP','แรงดันเซลล์สูงสุด (OVP)','V',0.001,0.01,2.5,4.5,3)
a('covpr',0x02,'Cell OVP release','คืนค่าจาก OVP','V',0.001,0.01,2.4,4.4,3,below='covp')
a('covp2',0x04,'Cell OVP L2','OVP ระดับ 2','V',0.001,0.01,2.5,4.6,3)
a('covpr2',0x06,'Cell OVP L2 rel','คืนค่าจาก OVP ระดับ 2','V',0.001,0.01,2.4,4.5,3,below='covp2')
a('cuvp',0x0C,'Cell UVP','แรงดันเซลล์ต่ำสุด (UVP)','V',0.001,0.01,1.5,3.6,3)
a('cuvpr',0x0E,'Cell UVP release','คืนค่าจาก UVP','V',0.001,0.01,1.6,3.8,3,above='cuvp')
a('cuvp2',0x10,'Cell UVP L2','UVP ระดับ 2','V',0.001,0.01,1.4,3.5,3)
a('cuvpr2',0x12,'Cell UVP L2 rel','คืนค่าจาก UVP ระดับ 2','V',0.001,0.01,1.5,3.7,3,above='cuvp2')
a('povp',0x08,'Pack OVP','แรงดันรวมสูงสุด','V',0.1,0.1,5,200,1)
a('povpr',0x0A,'Pack OVP release','คืนค่าแรงดันรวมสูงสุด','V',0.1,0.1,5,200,1,below='povp')
a('puvp',0x14,'Pack UVP','แรงดันรวมต่ำสุด','V',0.1,0.1,3,190,1)
a('puvpr',0x16,'Pack UVP release','คืนค่าแรงดันรวมต่ำสุด','V',0.1,0.1,3,195,1,above='puvp')
a('cdp',0x18,'Cell diff prot.','ป้องกันเซลล์ต่างกันเกิน','V',0.001,0.01,0.01,1.5,3)
a('cdpr',0x1A,'Cell diff rel','คืนค่าเซลล์ต่างกัน','V',0.001,0.01,0.01,1.5,3,below='cdp')
a('cow',0x20,'Cell OV warn','เตือนเซลล์สูง','V',0.001,0.01,2.5,4.5,3)
a('cowr',0x22,'Cell OV warn rel','คืนค่าเตือนเซลล์สูง','V',0.001,0.01,2.4,4.4,3,below='cow')
a('pow',0x24,'Pack OV warn','เตือนแรงดันรวมสูง','V',0.1,0.1,5,200,1)
a('powr',0x26,'Pack OV warn rel','คืนค่าเตือนแรงดันรวมสูง','V',0.1,0.1,5,200,1,below='pow')
a('cuw',0x28,'Cell UV warn','เตือนเซลล์ต่ำ','V',0.001,0.01,1.5,3.6,3)
a('cuwr',0x2A,'Cell UV warn rel','คืนค่าเตือนเซลล์ต่ำ','V',0.001,0.01,1.6,3.8,3,above='cuw')
a('puw',0x2C,'Pack UV warn','เตือนแรงดันรวมต่ำ','V',0.1,0.1,3,190,1)
a('puwr',0x2E,'Pack UV warn rel','คืนค่าเตือนแรงดันรวมต่ำ','V',0.1,0.1,3,195,1,above='puw')
a('cdw',0x30,'Cell diff warn','เตือนเซลล์ต่างกัน','V',0.001,0.01,0.01,1.5,3)
a('cdwr',0x32,'Cell diff warn rel','คืนค่าเตือนเซลล์ต่างกัน','V',0.001,0.01,0.01,1.5,3,below='cdw')
a('coc',0x68,'Charge OC','กระแสชาร์จสูงสุด','A',0.1,1,1,600,0)
a('cocd',0x6A,'Charge OC delay','หน่วงเวลา OC ชาร์จ','s',1,1,1,300,0)
a('doc',0x6C,'Discharge OC','กระแสจ่ายสูงสุด','A',0.1,1,1,800,0)
a('docd',0x6E,'Dsg OC delay','หน่วงเวลา OC จ่าย','s',1,1,1,300,0)
a('doc2',0x70,'Discharge OC L2','กระแสจ่ายสูงสุด ระดับ 2','A',0.1,1,1,1000,0)
a('docd2',0x72,'Dsg OC L2 delay','หน่วงเวลา OC จ่าย ระดับ 2','ms',1,10,10,60000,0)
a('scp',0x74,'Short circuit','กระแสลัดวงจร','A',1,10,10,5000,0)
a('scpd',0x76,'Short circ. delay','หน่วงเวลาลัดวงจร','us',1,10,10,5000,0)
a('cocw',0x7C,'Charge OC warn','เตือนกระแสชาร์จสูง','A',0.1,1,1,600,0)
a('cocwr',0x7E,'Chg OC warn rel','คืนค่าเตือนกระแสชาร์จ','A',0.1,1,1,600,0,below='cocw')
a('docw',0x80,'Dsg OC warn','เตือนกระแสจ่ายสูง','A',0.1,1,1,800,0)
a('docwr',0x82,'Dsg OC warn rel','คืนค่าเตือนกระแสจ่าย','A',0.1,1,1,800,0,below='docw')
a('soc1',0x84,'SOC low warn 1','เตือน SOC ต่ำ ระดับ 1','%',1,1,0,100,0)
a('soc2',0x86,'SOC low warn 2','เตือน SOC ต่ำ ระดับ 2','%',1,1,0,100,0)
a('balv',0x8C,'Balance voltage','แรงดันบาลานซ์','V',0.001,0.01,2.5,4.4,3)
a('bals',0x8E,'Balance start','เริ่มบาลานซ์ที่','V',0.001,0.01,2.5,4.4,3)
a('balon',0x90,'Balance diff on','บาลานซ์เมื่อต่างกัน','V',0.001,0.001,0.001,0.5,3)
a('baloff',0x92,'Balance diff off','หยุดบาลานซ์เมื่อต่างกัน','V',0.001,0.001,0.001,0.5,3,below='balon')
a('bali',0x94,'Balance current','กระแสบาลานซ์','mA',1,10,0,2000,0)
a('balci',0x96,'Bal. charge curr.','กระแสชาร์จขณะบาลานซ์','A',0.1,0.1,0,100,1)
a('shut',0x9E,'Shutdown voltage','แรงดันปิดระบบ','V',0.001,0.01,1.5,3.5,3)
a('reqc',0xA0,'Request chg curr.','กระแสชาร์จที่ขอ','A',0.1,1,0,600,0)
a('ncap',0xA2,'Nominal capacity','ความจุแบต','Ah',1,1,1,2000,0,width=4)

def C(rows):
    keys=[r[0] for r in rows]
    out=[]
    for r in rows:
        key,reg,name,th,unit,scale,off,step,mn,mx,dec,width,sgn,below,above=r
        F=lambda x: repr(float(x))+'f'
        out.append('    {"%s", "%s", 0x%04X, %s, %s, %s, %s, %s, %d, %d, %s, %d, %d},' % (
            name,unit,reg,F(scale),F(off),F(step),F(mn),F(mx),dec,width,'true' if sgn else 'false',
            keys.index(below) if below else -1, keys.index(above) if above else -1))
    return '\n'.join(out)
h = '''#pragma once
// GENERATED by tools/gen_params.py - edit the table there.
// Protection settings of ANT (2021 BLE protocol) and JBD/Xiaoxiang BMS.
// value = raw * scale + offset. JBD registers are big-endian EEPROM words
// written in factory mode; ANT registers are little-endian (read 0x02/0x12,
// write 0x22 = experimental, every write is verified by reading it back).
#include <stdint.h>
#include <math.h>
namespace bset {
struct Def {
  const char *name, *unit;
  uint16_t reg;
  float scale, offset, step, min, max;
  uint8_t dec, width;
  bool sgn;
  int8_t below, above; // must stay below / above this other setting
};
static const Def JBD[] = {
%s
};
static const Def ANT[] = {
%s
};
static const int JBD_N = sizeof(JBD) / sizeof(JBD[0]), ANT_N = sizeof(ANT) / sizeof(ANT[0]);
static const int MAX_N = 64;
inline const Def *table(uint8_t fam, int &n) {
  if (fam == 2) { n = ANT_N; return ANT; }
  if (fam == 3) { n = JBD_N; return JBD; }
  n = 0;
  return nullptr;
}
inline float decode(const Def &d, int32_t raw) { return raw * d.scale + d.offset; }
inline int32_t encode(const Def &d, float v) { return int32_t(lroundf((v - d.offset) / d.scale)); }
// nullptr when the value may be written
inline const char *check(uint8_t fam, const float *cur, uint64_t have, int i, float v) {
  int n;
  const Def *t = table(fam, n);
  if (!t || i < 0 || i >= n) return "unknown setting";
  const Def &d = t[i];
  if (v < d.min - d.step / 2 || v > d.max + d.step / 2) return "outside the safe range";
  if (d.below >= 0 && (have >> d.below & 1) && v >= cur[d.below]) return "must be below its protection value";
  if (d.above >= 0 && (have >> d.above & 1) && v <= cur[d.above]) return "must be above its protection value";
  for (int k = 0; k < n; k++) { // protection values must stay outside their release values
    if (t[k].below == i && (have >> k & 1) && v <= cur[k]) return "must be above its release value";
    if (t[k].above == i && (have >> k & 1) && v >= cur[k]) return "must be below its release value";
  }
  return nullptr;
}
// ---- JBD frames
inline int jbdFrame(uint8_t *f, uint8_t cmd, uint8_t reg, const uint8_t *data, uint8_t len) {
  f[0] = 0xDD; f[1] = cmd; f[2] = reg; f[3] = len;
  uint16_t c = 0;
  c -= reg; c -= len;
  for (int i = 0; i < len; i++) { f[4 + i] = data[i]; c -= data[i]; }
  f[4 + len] = c >> 8; f[5 + len] = c & 0xFF; f[6 + len] = 0x77;
  return 7 + len;
}
inline int jbdRead(uint8_t *f, uint8_t reg) { return jbdFrame(f, 0xA5, reg, nullptr, 0); }
inline int jbdWrite(uint8_t *f, uint8_t reg, int32_t raw) {
  uint8_t d[2] = {uint8_t(uint16_t(raw) >> 8), uint8_t(uint16_t(raw) & 0xFF)};
  return jbdFrame(f, 0x5A, reg, d, 2);
}
inline int jbdFactory(uint8_t *f) { uint8_t d[2] = {0x56, 0x78}; return jbdFrame(f, 0x5A, 0x00, d, 2); }
inline int jbdExit(uint8_t *f, bool save) { uint8_t d[2] = {uint8_t(save ? 0x28 : 0), uint8_t(save ? 0x28 : 0)}; return jbdFrame(f, 0x5A, 0x01, d, 2); }
// ---- ANT frames (CRC-16/MODBUS over A1..payload)
inline uint16_t antCrc(const uint8_t *p, int n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}
inline int antFrame(uint8_t *f, uint8_t fn, uint16_t addr, uint8_t len, const uint8_t *data, uint8_t dlen) {
  f[0] = 0x7E; f[1] = 0xA1; f[2] = fn; f[3] = addr & 0xFF; f[4] = addr >> 8; f[5] = len;
  for (int i = 0; i < dlen; i++) f[6 + i] = data[i];
  uint16_t c = antCrc(f + 1, 5 + dlen);
  f[6 + dlen] = c & 0xFF; f[7 + dlen] = c >> 8; f[8 + dlen] = 0xAA; f[9 + dlen] = 0x55;
  return 10 + dlen;
}
inline int antRead(uint8_t *f, const Def &d) { return antFrame(f, 0x02, d.reg, d.width, nullptr, 0); }
inline int antWrite(uint8_t *f, const Def &d, int32_t raw) {
  uint8_t v[4] = {uint8_t(raw), uint8_t(raw >> 8), uint8_t(raw >> 16), uint8_t(raw >> 24)};
  return antFrame(f, 0x22, d.reg, d.width, v, d.width);
}
}  // namespace bset
''' % (C(JBD), C(A))
(ROOT/'include/bms_settings.h').write_text(h)
def J(rows):
    return [[r[0],r[2],r[3],r[4],r[7],r[8],r[9],r[10]] for r in rows]
js='// GENERATED by battlab-jk/tools/gen_params.py\nconst BMS_PARAMS={2:%s,3:%s};\n'%(json.dumps(J(A),ensure_ascii=False),json.dumps(J(JBD),ensure_ascii=False))
(ROOT/'cloud/web/params.js').write_text(js)
print(len(JBD),'JBD',len(A),'ANT')
