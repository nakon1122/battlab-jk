#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace jk {
inline bool validCode(const std::string &s) {
  if (s.empty() || s.size()>16) return false;
  for (unsigned char c:s) if(c<32 || c>126) return false;
  return true;
}
inline bool readCode(const uint8_t *frame, size_t size, size_t offset, std::string &out) {
  out.clear();
  if(size!=300 || offset+16>299 || frame[0]!=0x55 || frame[1]!=0xaa || frame[2]!=0xeb || frame[3]!=0x90 || frame[4]!=3) return false;
  uint8_t sum=0;for(size_t i=0;i<299;i++)sum+=frame[i];if(sum!=frame[299])return false;
  bool ended=false;
  for(size_t i=0;i<16;i++) {
    uint8_t c=frame[offset+i];
    if(c==0 || c==0xff){ended=true;continue;}
    if(ended || c<32 || c>126){out.clear();return false;}
    out.push_back(char(c));
  }
  return validCode(out);
}
inline bool sameCode(const std::string &a,const std::string &b) {
  unsigned diff=unsigned(a.size()^b.size());
  for(size_t i=0;i<16;i++)diff|=(i<a.size()?uint8_t(a[i]):0)^(i<b.size()?uint8_t(b[i]):0);
  return diff==0 && validCode(a) && validCode(b);
}
}
