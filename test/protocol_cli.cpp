// Host-side harness for DivaProtocol: reads "op kind sliderRev ledRev hex"
// lines on stdin, prints the translated result as hex. Driven by
// test_protocol.py, which compares it with diva_bridge.py.
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include "../src/DivaProtocol.h"
using namespace diva;
static std::string hex(const uint8_t* d, size_t n){std::string s;char b[3];for(size_t i=0;i<n;i++){snprintf(b,3,"%02x",d[i]);s+=b;}return s;}
int main(){
  std::string op,hx; int kind,sr,lr;
  while(std::cin>>op>>kind>>sr>>lr>>hx){
    uint8_t in[256]; size_t n=hx.size()/2;
    for(size_t i=0;i<n;i++) in[i]=std::stoi(hx.substr(2*i,2),nullptr,16);
    const Profile* p=profileForKind((Kind)kind);
    Options o; o.sliderReverse=sr; o.ledReverse=lr;
    if(op=="in"){uint8_t pkt[MAX_IN_PACKET]; uint16_t ext=0; size_t m=toDivaller(*p,o,in,n,pkt,ext);
      if(!m) std::cout<<"skip\n"; else std::cout<<hex(pkt,m)<<" "<<ext<<"\n";}
    else if(op=="led"){uint8_t out[MAX_DEVICE_OUT]; size_t m=ledsToDevice(*p,o,in,n,out);
      if(!m) std::cout<<"none\n"; else std::cout<<hex(out,m)<<"\n";}
    else if(op=="dec"){Decoded d=decode(in); std::cout<<(int)d.pressed<<" "<<d.sensors<<"\n";}
  }
}
