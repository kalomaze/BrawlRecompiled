// Local control surface for the private Brawl Online protocol sidecar.
// Enabled only by the launcher; TCP control binds exclusively to loopback.
#include "brawl_online_ipc.h"
#include "frame_input.h"
#include "memory.h"
#include "save_states.h"
#include "platform/local_control_socket.h"
#include <cerrno>
#include <chrono>
#include <thread>
#include <string>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <array>
namespace BrawlRollback {
void BeginOnlineMatch(unsigned,unsigned,unsigned,unsigned);
void StopOnlineMatch();
std::string OnlineStatus();
unsigned OnlineHold();
void SetOnlineCountdown(const std::array<unsigned, 4>&);
}
namespace BrawlOnline { namespace {
RuntimePlatform::LocalControlSocket control;
std::string phase="boot";
bool paused=false,css=false,barrier=false,start=false,go=false,stageSetup=false;
unsigned long long ticks=0;
bool armed=false,padsActive=false;
std::mutex padMutex;
std::array<FrameInput::GcPad,4> pads{};
std::array<unsigned,4> padFrames{};
bool MenuLoop(){return std::getenv("BRAWL_ONLINE_LOOP")!=nullptr;}
bool Enabled(){return std::getenv("BRAWL_ONLINE_IPC")!=nullptr;}
unsigned Read(unsigned p){return Memory::Contains(p,4)?Memory::Read32(p):0;}
void Reply(const std::string& text){control.Send(text);}
void SetEnvironment(const char* key,const char* value){
#ifdef _WIN32
 if(_putenv_s(key,value))throw std::runtime_error("online environment update failed");
#else
 if(setenv(key,value,1))throw std::runtime_error("online environment update failed");
#endif
}
std::string Hex(unsigned a,unsigned n){if(n>65536||!Memory::Contains(a,n))throw std::runtime_error("invalid memory span");std::string s;for(unsigned i=0;i<n;i++){char b[3];std::snprintf(b,3,"%02x",Memory::Read8(a+i));s+=b;}return s;}
void Command(const std::string& line){
 std::istringstream in(line);std::string op;in>>op;
 try {
  if(op=="status")Reply(phase+" "+SaveStates::SceneName()+" "+std::to_string(Read(0x901812A4))+" "+std::to_string(ticks));
  else if(op=="pad"){
   unsigned port,buttons,frames;int x,y;
   if(!(in>>port>>buttons>>x>>y>>frames)||port>3||buttons>65535||x< -128||x>127||y< -128||y>127||frames>36000)throw std::runtime_error("invalid pad command");
   std::lock_guard<std::mutex> lock(padMutex);padsActive=true;pads[port]={true,uint16_t(buttons),int8_t(x),int8_t(y),0,0,uint8_t(buttons&0x40?255:0),uint8_t(buttons&0x20?255:0)};padFrames[port]=frames;Reply("ok");
  }
  else if(op=="arm"){
   unsigned index,pad,mailbox,size;
   if(!(in>>index>>pad>>std::hex>>mailbox>>size)||index>1000000||pad>3||size>65536||!Memory::Contains(mailbox,size))throw std::runtime_error("invalid arm command");
   BrawlRollback::BeginOnlineMatch(index,pad,mailbox,size);armed=true;barrier=start=go=stageSetup=false;Reply("ok");
  }
  else if(op=="hold")Reply(std::to_string(BrawlRollback::OnlineHold()));
  else if(op=="slot"){
   unsigned slot;
   if (!(in >> slot) || slot > 1 || armed)
    throw std::runtime_error("invalid online slot or match already armed");
   SetEnvironment("BRAWL_ROLLBACK_SLOT", std::to_string(slot).c_str());
   Reply("ok");
  }
  else if(op=="countdown"){
   std::array<unsigned, 4> holds{};
   for (auto& h : holds)
    if (!(in >> h) || (h & ~0x3f7fu))
     throw std::runtime_error("invalid countdown hold");
   BrawlRollback::SetOnlineCountdown(holds);
   Reply("ok");
  }
  else if(op=="disarm"){armed=false;paused=false;go=true;BrawlRollback::StopOnlineMatch();Reply("ok");}
  else if(op=="rollback")Reply(BrawlRollback::OnlineStatus());
  else if(op=="read"){unsigned a,n;in>>std::hex>>a>>n;Reply(Hex(a,n));}
  else if(op=="write"){unsigned a;std::string h;in>>std::hex>>a>>h;if(h.size()%2||h.size()>131072||!Memory::Contains(a,h.size()/2))throw std::runtime_error("invalid write span");for(unsigned i=0;i<h.size()/2;i++)Memory::Write8(a+i,std::stoul(h.substr(2*i,2),nullptr,16));Reply("ok");}
  else if(op=="seed"){std::string s;in>>s;std::stoul(s);SetEnvironment("BRAWL_PROTOCOL_SEED",s.c_str());Reply("ok");}
  else if(op=="release"){paused=false;Reply("ok");}
  else if(op=="go"){go=true;Reply("ok");}
  else throw std::runtime_error("unknown command");
 }catch(const std::exception& e){Reply(std::string("error ")+e.what());}
}
void Hold(const char* at){phase=at;paused=true;std::fprintf(stderr,"[online] gate %s\n",at);auto since=std::chrono::steady_clock::now();while(paused){Poll();if(std::chrono::steady_clock::now()-since>std::chrono::seconds(180))throw std::runtime_error("online gate timed out: "+phase);std::this_thread::sleep_for(std::chrono::milliseconds(1));}phase="run";}
}
void Poll(){
 if(!Enabled())return;
 if(!control.IsOpen()){
  const char* transport=std::getenv("BRAWL_ONLINE_IPC_TRANSPORT");
  const char* token=std::getenv("BRAWL_ONLINE_IPC_TOKEN");
  control.Open(std::filesystem::u8path(std::getenv("BRAWL_ONLINE_IPC")),transport?transport:"unix",token?token:"");
 }
 control.Poll(Command);
}
void Loop(bool stageSeen){
 if(!Enabled())return;++ticks;
 {std::lock_guard<std::mutex> lock(padMutex);for(unsigned p=0;p<4;++p)if(padFrames[p]&&!--padFrames[p])pads[p]={true};}
 Poll();auto scene=SaveStates::SceneName();
 if(MenuLoop()&&!armed)return;
 if(!MenuLoop()&&!css&&scene=="scSelctCharacter"){css=true;Hold("css");}
 auto gf=Read(0x901812A4);
 if(!barrier&&stageSeen&&scene=="scMelee"&&gf==1&&Read(0x901812B4)>gf){barrier=true;Hold("barrier");}
 unsigned at=std::getenv("BRAWL_ROLLBACK_START")?std::stoul(std::getenv("BRAWL_ROLLBACK_START")):119;
 if(barrier&&!start&&scene=="scMelee"&&gf>=at){start=true;Hold("start");}
}
void Stage(){
 // Apply the agreed substage before the first file load, not after the stage
 // has retained pointers into that file. Changing it at construction can
 // reload the archive over those live pointers (e.g. Smashville's lighting).
 // The constructor remains a setup boundary for paths without a file load.
 if(Enabled()&&(!MenuLoop()||armed)&&!stageSetup){Hold("stage");stageSetup=true;}
}
void SamplePads(FrameInput::GcPad* out){std::lock_guard<std::mutex> lock(padMutex);if(padsActive)for(unsigned p=0;p<4;++p)out[p]=pads[p];}
void WaitForGo(){if(!Enabled())return;auto since=std::chrono::steady_clock::now();while(!go){Poll();if(std::chrono::steady_clock::now()-since>std::chrono::seconds(60))throw std::runtime_error("online go timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
}
