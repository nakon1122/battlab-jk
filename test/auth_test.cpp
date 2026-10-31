#include "jk_auth.h"
#include <array>
#include <cassert>
#include <iostream>
void sum(std::array<uint8_t,300>& f){uint8_t s=0;for(int i=0;i<299;i++)s+=f[i];f[299]=s;}
int main(){
 std::array<uint8_t,300> f{};f[0]=0x55;f[1]=0xaa;f[2]=0xeb;f[3]=0x90;f[4]=3;
 const std::string bc="0421",pc="654321";std::copy(bc.begin(),bc.end(),f.begin()+62);std::copy(pc.begin(),pc.end(),f.begin()+118);sum(f);
 std::string b,p;assert(jk::readCode(f.data(),300,62,b));assert(jk::readCode(f.data(),300,118,p));assert(b==bc&&p==pc);
 assert(jk::sameCode("0421",b));assert(!jk::sameCode("421",b));assert(!jk::sameCode("0422",b));assert(!jk::sameCode("",b));assert(!jk::sameCode("", ""));assert(!jk::sameCode(b,p));
 assert(!jk::validCode(std::string(17,'1')));assert(!jk::validCode("abc\n"));assert(jk::validCode(std::string(16,'a')));
 assert(!jk::readCode(f.data(),299,62,b));f[63]^=1;assert(!jk::readCode(f.data(),300,62,b));f[63]^=1;f[4]=2;sum(f);assert(!jk::readCode(f.data(),300,62,b));
 f[4]=3;f[67]='x';sum(f);assert(!jk::readCode(f.data(),300,62,b));f[67]=0;f[62]=1;sum(f);assert(!jk::readCode(f.data(),300,62,b));
 std::cout<<"PASS: two independent codes, leading zero, wrong/missing/oversized code, invalid frame/type/checksum/ASCII/padding\n";
}
