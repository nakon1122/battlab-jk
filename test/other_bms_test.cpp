#include "../include/bms_other.h"
#include <cassert>
#include <cstdio>
#include <vector>
static void putLe16(std::vector<uint8_t>&v,size_t i,uint16_t x){v[i]=x&255;v[i+1]=x>>8;}
static void putLe32(std::vector<uint8_t>&v,size_t i,uint32_t x){putLe16(v,i,x&0xffff);putLe16(v,i+2,x>>16);}
std::vector<uint8_t> antFrame(uint8_t cells,uint8_t temps){
  size_t off=cells*2+temps*2; size_t dlen=(34+off+100)-6; // payload big enough
  std::vector<uint8_t> f(6+dlen+4,0);
  f[0]=0x7E;f[1]=0xA1;f[2]=0x11;f[5]=dlen;f[8]=temps;f[9]=cells;
  for(int i=0;i<cells;i++)putLe16(f,34+i*2,3870);
  for(int i=0;i<temps;i++)putLe16(f,34+cells*2+i*2,uint16_t(int16_t(28+i)));
  putLe16(f,34+off,31);              // mos
  putLe16(f,38+off,uint16_t(cells*387)); // total 0.01V
  putLe16(f,40+off,uint16_t(int16_t(-125))); // -12.5 A
  putLe16(f,42+off,87);
  f[46+off]=1;f[47+off]=1;
  putLe32(f,54+off,34500000);        // 34.5 Ah
  size_t n=f.size(); uint16_t c=bms::antCrc16(f.data()+1,n-5);
  f[n-4]=c&255;f[n-3]=c>>8;f[n-2]=0xAA;f[n-1]=0x55;
  return f;
}
std::vector<uint8_t> jbdFrame(uint8_t cmd,const std::vector<uint8_t>&d){
  std::vector<uint8_t> f={0xDD,cmd,0x00,(uint8_t)d.size()};
  f.insert(f.end(),d.begin(),d.end());
  uint16_t c=bms::jbdChecksum(f.data()+2,d.size()+2);
  f.push_back(c>>8);f.push_back(c&255);f.push_back(0x77);return f;
}
int main(){
  // ANT: 20S frame split into 20-byte notifications, with garbage before it
  auto a=antFrame(20,2); bms::AntFramer af; int got=0; jk::Sample s;
  std::vector<uint8_t> stream={0xFC,0xAA,0x06}; stream.insert(stream.end(),a.begin(),a.end());
  for(size_t i=0;i<stream.size();i+=20){size_t n=std::min<size_t>(20,stream.size()-i);
    af.feed(stream.data()+i,n,[&](const uint8_t*f,size_t len){assert(bms::decodeAnt(f,len,s));got++;});}
  assert(got==1); assert(s.count==20); assert(std::fabs(s.voltage-77.4f)<.01f);
  assert(std::fabs(s.current+12.5f)<.01f); assert(s.soc==87); assert(s.charge&&s.discharge);
  assert(std::fabs(s.capacity-34.5f)<.01f); assert(std::fabs(s.cells[19]-3.87f)<.001f);
  assert(s.temp1==28&&s.temp2==29&&s.mos==31);
  // corrupt CRC -> rejected
  auto bad=a; bad[40]^=1; got=0; af.feed(bad.data(),bad.size(),[&](const uint8_t*,size_t){got++;}); assert(got==0&&af.bad>=1);
  // JBD basic + cells, split into 20-byte notifications
  std::vector<uint8_t> basic(29,0);
  basic[0]=7740>>8;basic[1]=7740&255;            // 77.40 V
  int16_t cur=-1250; basic[2]=uint16_t(cur)>>8;basic[3]=uint16_t(cur)&255; // -12.50 A
  basic[4]=3450>>8;basic[5]=3450&255;            // 34.50 Ah
  basic[8]=0;basic[9]=42; basic[19]=87; basic[20]=0x03; basic[21]=20; basic[22]=3;
  for(int i=0;i<3;i++){uint16_t k=2731+280+i*10;basic[23+i*2]=k>>8;basic[24+i*2]=k&255;}
  std::vector<uint8_t> cells; for(int i=0;i<20;i++){cells.push_back(3870>>8);cells.push_back(3870&255);}
  auto fb=jbdFrame(3,basic), fc=jbdFrame(4,cells);
  bms::JbdFramer jf; jk::Sample js; int gb=0,gc=0;
  auto cb=[&](uint8_t cmd,const uint8_t*p,size_t n){ if(cmd==3){assert(bms::decodeJbdBasic(p,n,js));gb++;} if(cmd==4){assert(bms::decodeJbdCells(p,n,js));gc++;} };
  for(auto *f:{&fb,&fc}) for(size_t i=0;i<f->size();i+=20){size_t n=std::min<size_t>(20,f->size()-i); jf.feed(f->data()+i,n,cb);}
  assert(gb==1&&gc==1); assert(js.count==20); assert(std::fabs(js.voltage-77.4f)<.01f);
  assert(std::fabs(js.current+12.5f)<.01f); assert(std::fabs(js.capacity-34.5f)<.01f); assert(js.cycles==42);
  assert(js.soc==87&&js.charge&&js.discharge); assert(std::fabs(js.temp1-28)<.01f&&std::fabs(js.mos-30)<.01f);
  // request checksums match the documented constants
  assert(bms::jbdChecksum(bms::JBD_BASIC_REQUEST+2,2)==0xFFFD);
  assert(bms::jbdChecksum(bms::JBD_CELLS_REQUEST+2,2)==0xFFFC);
  assert(bms::antCrc16(bms::ANT_STATUS_REQUEST+1,5)==0x5518);
  puts("other_bms_test PASS");
}
