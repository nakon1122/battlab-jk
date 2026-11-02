#include "../include/jk_rs485.h"
#include <cstdio>
int main(){uint8_t f[32];int n=jk485::pbRequest(f,1,2);for(int i=0;i<n;i++)printf("%02X ",f[i]);printf("\n");
n=jk485::pbRequest(f,1,1);for(int i=0;i<n;i++)printf("%02X ",f[i]);printf("\n");
n=jk485::pbWrite(f,2,0x1078,0);for(int i=0;i<n;i++)printf("%02X ",f[i]);printf("  want 02 10 10 78 00 02 04 00 00 00 00 37 A9\n");
n=jk485::pbWrite(f,2,0x1078,1);for(int i=0;i<n;i++)printf("%02X ",f[i]);printf("  want ...F6 69\n");
n=jk485::oldReadAll(f);for(int i=0;i<n;i++)printf("%02X ",f[i]);printf("\n");
uint8_t d[]={0x79,0x06,0x01,0x0C,0xE4,0x02,0x0C,0xE6,0x80,0x00,0x1D,0x81,0x00,0x1E,0x82,0x00,0x1C,0x83,0x14,0xEF,0x84,0x80,0xD0,0x85,0x0F,0x86,0x02,0x87,0x00,0x04,0x89,0,0,0,0,0x8A,0,2,0x8B,0,0,0x8C,0,3};
jk::Sample s;bool ok=jk485::decodeOld(d,sizeof d,s);printf("ok=%d V=%.2f I=%.2f soc=%.0f n=%u c1=%.3f mos=%.0f chg=%d dsg=%d\n",ok,s.voltage,s.current,s.soc,s.count,s.cells[0],s.mos,s.charge,s.discharge);}
