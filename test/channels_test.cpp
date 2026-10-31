#include "ble_channels.h"
#include <cassert>
#include <iostream>
struct C{bool matching,w,nr,n;bool canWrite(){return w;}bool canWriteNoResponse(){return nr;}bool canNotify(){return n;}};
int main(){C tx{true,true,false,false},rx{true,false,false,true},both{true,false,true,true},other{false,true,true,true},bad{true,false,false,false};C *t,*r;
auto match=[](C*c){return c->matching;};
jk::selectChannels(std::vector<C*>{&other,&tx,&rx},t,r,match);assert(t==&tx&&r==&rx);
jk::selectChannels(std::vector<C*>{&both},t,r,match);assert(t==&both&&r==&both);
jk::selectChannels(std::vector<C*>{&bad,&rx},t,r,match);assert(t==nullptr&&r==&rx);
jk::selectChannels(std::vector<C*>{&other,nullptr},t,r,match);assert(!t&&!r);
std::cout<<"PASS: split TX/RX FFE1, combined characteristic, missing TX, unrelated UUID and null ignored\n";
}
