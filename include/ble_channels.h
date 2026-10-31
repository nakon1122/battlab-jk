#pragma once
#include <vector>
namespace jk {
template<class Characteristic,class Matches>
void selectChannels(const std::vector<Characteristic*>& chars, Characteristic*& tx, Characteristic*& rx, Matches matches) {
 tx=nullptr;rx=nullptr;
 for(auto candidate:chars) {
  if(!candidate || !matches(candidate))continue;
  if(!tx && (candidate->canWrite() || candidate->canWriteNoResponse()))tx=candidate;
  if(!rx && candidate->canNotify())rx=candidate;
 }
}
}
