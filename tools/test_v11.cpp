// Warpline 1.1 feature test: a small VST2 host that drives the arp, GRAINZ, the mod matrix, search, favourites,
// drag/tap and a state reload through setParameter and prints what it measures (onset counts, RMS, display texts).
//   g++ -std=c++17 -O1 -g -fsanitize=address -I<sdk>/framework tools/test_v11.cpp -o test_v11 -ldl
//   WARPLINE_FAVOURITES=/tmp/fav.txt ASAN_OPTIONS=detect_leaks=0 ./test_v11 build/native/warpline.so
#include "mpc_vst2_abi.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
static VstTimeInfo ti{};
static bool playing=false; static double ppq=0;
static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float){
  if(op==audioMasterGetTime){ ti.sampleRate=44100; ti.tempo=120; ti.ppqPos=ppq;
    ti.flags=kVstTempoValid|kVstPpqPosValid|(playing?kVstTransportPlaying:0); return (VstIntPtr)&ti;}
  if(op==1) return 2400; return 0;}
AEffect* e; std::map<std::string,int> idx; const int B=128; float L[B],R[B]; float* outs[2]={L,R};
std::vector<VstMidiEvent> pend;
int P(const char* n){ auto it=idx.find(n); if(it==idx.end()){printf("NO PARAM %s\n",n); exit(1);} return it->second;}
void set(const char* n,float v){ e->setParameter(e,P(n),v);}
std::string disp(const char* n){ char b[256]={0}; e->dispatcher(e,effGetParamDisplay,P(n),0,b,0); return b;}
void midi(int s,int d1,int d2){ VstMidiEvent m{}; m.type=kVstMidiType; m.byteSize=sizeof m; m.midiData[0]=s;m.midiData[1]=d1;m.midiData[2]=d2; pend.push_back(m);}
std::vector<float> run(double secs){ std::vector<float> o; int n=(int)(secs*44100/B);
  for(int i=0;i<n;i++){ if(!pend.empty()){ std::vector<char> buf(sizeof(VstEvents)+pend.size()*sizeof(void*)); VstEvents* ev=(VstEvents*)buf.data(); ev->numEvents=pend.size(); for(size_t k=0;k<pend.size();k++) ev->events[k]=(VstEvent*)&pend[k]; e->dispatcher(e,effProcessEvents,0,0,ev,0); }
    e->processReplacing(e,nullptr,outs,B); pend.clear(); if(playing) ppq+=B/44100.0*2.0;
    for(int k=0;k<B;k++){ if(!std::isfinite(L[k])||!std::isfinite(R[k])){printf("NAN!\n");exit(1);} o.push_back(L[k]);} }
  return o;}
double rms(const std::vector<float>& v,size_t a,size_t b){ double s=0; for(size_t i=a;i<b&&i<v.size();i++) s+=v[i]*v[i]; return std::sqrt(s/std::max<size_t>(1,b-a));}
int onsets(const std::vector<float>& v){ int c=0; bool hi=false; double peak=0; std::vector<double> r; for(size_t i=0;i+220<=v.size();i+=220) r.push_back(rms(v,i,i+220)); for(double x:r) peak=std::max(peak,x);
  for(double x:r){ if(!hi&&x>peak*0.5){c++;hi=true;} else if(hi&&x<peak*0.2) hi=false;} return c;}
void wait(int ms){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int main(int argc,char**argv){
  void* h=dlopen(argv[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  auto entry=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain"); e=entry(host);
  e->dispatcher(e,effOpen,0,0,0,0); e->dispatcher(e,effSetSampleRate,0,0,0,44100); e->dispatcher(e,effSetBlockSize,0,B,0,0);
  e->dispatcher(e,effMainsChanged,0,1,0,0); e->dispatcher(e,effStartProcess,0,0,0,0);
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamName,i,0,b,0); idx[b]=i; }
  printf("params %d\n",e->numParams);
  wait(300); run(0.2);
  // Plain chord, no arp
  midi(0x90,60,100); auto a=run(1.0); midi(0x80,60,0); run(1.0);
  printf("plain onsets %d rms %.3f\n",onsets(a),rms(a,0,a.size()));
  // Arp on 1/16 @120 = 8 steps/s
  set("Arp On",1); set("Arp Gate",(25-5)/95.0f); wait(50);
  printf("arp info: %s rate %s\n",disp("Arp Info").c_str(),disp("Arp Rate").c_str());
  midi(0x90,60,100); midi(0x90,64,100); midi(0x90,67,100); a=run(2.0); midi(0x80,60,0);midi(0x80,64,0);midi(0x80,67,0); auto t=run(1.0);
  printf("arp onsets in 2s: %d (expect ~16), tail rms %.4f\n",onsets(a), rms(t,22050,44100));
  // step pattern: every other step off
  for(int k=1;k<=16;k+=2){ char n[32]; snprintf(n,32,"Step Tap %d",k+1); set(n,1); set(n,1); set(n,1); } // On->Accent->Tie->Off
  wait(50); printf("step2 %s step3 %s\n",disp("Arp Step 2").c_str(),disp("Arp Step 3").c_str());
  midi(0x90,60,100); a=run(2.0); midi(0x80,60,0); run(1.0);
  printf("arp half-steps onsets: %d (expect ~8)\n",onsets(a));
  // transport playing, synced
  playing=true; ppq=0.37; midi(0x90,60,100); a=run(2.0); midi(0x80,60,0); run(0.5); playing=false;
  printf("synced onsets: %d\n",onsets(a));
  // latch
  set("Arp Latch",1); midi(0x90,62,100); run(0.1); midi(0x80,62,0); a=run(1.0); printf("latch onsets after release: %d (expect ~4)\n",onsets(a));
  set("Arp Latch",0); a=run(0.5); printf("after unlatch rms %.4f\n",rms(a,11025,22050));
  set("Arp On",0); wait(50);
  // Granulator freeze
  set("Grain On",1); set("Grain Mix",1.0f); wait(50); printf("grain info %s size %s pitch %s\n",disp("Grain Info").c_str(),disp("Grain Size").c_str(),disp("Grain Pitch").c_str());
  midi(0x90,60,100); run(0.6); set("Grain Freeze",1); midi(0x80,60,0); a=run(1.5);
  printf("frozen tail rms %.4f (expect >0)\n",rms(a,44100,66150));
  set("Grain Freeze",0); a=run(2.5); printf("unfrozen tail rms %.4f\n",rms(a,88200,110000));
  set("Grain Pitch",1.0f); set("Grain Feedback",1.0f); set("Grain Size",1.0f); set("Grain Reverse",0.5f); set("Grain Space",1.0f);
  midi(0x90,48,127); a=run(2.0); midi(0x80,48,0); auto b2=run(2.0); double pk=0; for(float x:a) pk=std::max(pk,(double)std::fabs(x)); for(float x:b2) pk=std::max(pk,(double)std::fabs(x));
  printf("extreme grain peak %.3f\n",pk);
  for(const char* n:{"Grain Sync"}) { set(n,0.5f); } midi(0x90,60,100); run(1.0); midi(0x80,60,0); run(0.5);
  set("Grain On",0); set("Grain Sync",0); wait(50);
  // Mod matrix: Mod Wheel -> Volume -100%
  midi(0xB0,1,127); midi(0x90,60,100); a=run(1.0); midi(0x80,60,0); run(1);
  double base=rms(a,22050,44100);
  set("Mod 1 Source",9/15.0f); set("Mod 1 Dest",(44)/(44.0f)); set("Mod 1 Amount",0.0f); wait(200);
  printf("mod1: %s -> %s %s; info %s\n",disp("Mod 1 Source").c_str(),disp("Mod 1 Dest").c_str(),disp("Mod 1 Amount").c_str(),disp("Mod Info").c_str());
  midi(0x90,60,100); a=run(1.0); midi(0x80,60,0); run(1);
  printf("modwheel->volume -100%%: rms %.4f vs %.4f\n",rms(a,22050,44100),base);
  set("Mod 1 Source",0); wait(200);
  midi(0x90,60,100); a=run(1.0); midi(0x80,60,0); run(1); printf("slot off again rms %.4f\n",rms(a,22050,44100));
  // LFO1 square -> F1 Cutoff
  set("Mod 2 Source",4/15.0f); set("Mod 2 Dest",1.0f); set("Mod 2 Amount",0.25f); set("LFO1 Shape",1.0f); wait(200);
  printf("mod2: %s -> %s %s lfo1 %s rate %s sync %s\n",disp("Mod 2 Source").c_str(),disp("Mod 2 Dest").c_str(),disp("Mod 2 Amount").c_str(),disp("LFO1 Shape").c_str(),disp("LFO1 Rate").c_str(),disp("LFO1 Sync").c_str());
  midi(0x90,60,100); a=run(2.0); midi(0x80,60,0); run(1);
  { std::vector<double> r; for(size_t i=22050;i+2205<a.size();i+=2205) r.push_back(rms(a,i,i+2205)); double mn=1e9,mx=0; for(double x:r){mn=std::min(mn,x);mx=std::max(mx,x);} printf("lfo->cutoff rms range %.4f..%.4f\n",mn,mx);}
  for(int variant=0;variant<4;variant++){
    if(variant==0){ set("LFO1 Shape",0); }
    if(variant==1){ set("LFO1 Sync",0); }
    if(variant==2){ set("Mod 2 Source",5/15.0f); }  // LFO 2 default
    if(variant==3){ set("Mod 2 Source",11/15.0f); } // random
    wait(200);
    midi(0x90,60,100); a=run(2.0); midi(0x80,60,0); run(1);
    std::vector<double> r; for(size_t i=22050;i+2205<a.size();i+=2205) r.push_back(rms(a,i,i+2205)); double mn=1e9,mx=0; for(double x:r){mn=std::min(mn,x);mx=std::max(mx,x);}
    printf("variant %d (%s %s %s): %.4f..%.4f\n",variant,disp("Mod 2 Source").c_str(),disp("LFO1 Shape").c_str(),disp("LFO1 Sync").c_str(),mn,mx);
  }
  set("Mod 2 Source",0); wait(200);
  // patch change keeps matrix
  set("Mod 3 Source",1/15.0f); set("Mod 3 Dest",22/44.0f); set("Patch",0.5f); wait(600); run(0.1); wait(200);
  printf("after patch load: patch %s mod3 %s->%s info %s\n",disp("Patch").c_str(),disp("Mod 3 Source").c_str(),disp("Mod 3 Dest").c_str(),disp("Mod Info").c_str());
  midi(0x90,60,100); a=run(1); midi(0x80,60,0); run(1); printf("patch plays rms %.4f\n",rms(a,0,a.size()));
  // Search
  set("Browser",1); set("Panel",0.2f); wait(50); printf("panel %s browser %s\n",disp("Panel").c_str(),disp("Browser").c_str());
  set("Browser",1); wait(50); printf("after browser open: panel %s\n",disp("Panel").c_str());
  printf("search empty: '%s'\n",disp("Search").c_str());
  for(const char* k:{"Key P","Key A","Key D"}) set(k,1.0f);
  wait(150); printf("search '%s' row1 '%s' row2 '%s' key '%s'\n",disp("Search").c_str(),disp("List 1").c_str(),disp("List 2").c_str(),disp("Key P").c_str());
  set("Key DEL",1); wait(100); printf("after del '%s' row1 '%s'\n",disp("Search").c_str(),disp("List 1").c_str());
  set("Key CLR",1); wait(100); printf("after clr '%s' row1 '%s'\n",disp("Search").c_str(),disp("List 1").c_str());
  // Favourites
  set("Patch",0.25f); wait(400); std::string fav=disp("Patch"); set("Favourite",1); wait(150); printf("t150 %s\n",disp("Fav State").c_str()); wait(800); run(0.05); wait(200);
  printf("fav state %s for %s\n",disp("Fav State").c_str(),fav.c_str());
  std::ifstream f(getenv("WARPLINE_FAVOURITES")); std::stringstream ss; ss<<f.rdbuf(); printf("file: [%s]\n",ss.str().c_str());
  set("Fav Filter",1); wait(150); printf("fav view row1 '%s' row2 '%s'\n",disp("List 1").c_str(),disp("List 2").c_str());
  set("Fav Filter",0); wait(150);
  // Drag
  set("List Scroll",1.0f); wait(150); printf("top row '%s'\n",disp("List 1").c_str());
  set("List Drag",0.5f); for(int i=1;i<=10;i++){ wait(30); set("List Drag",0.5f+0.04f*i);} wait(400);
  printf("after drag up: row1 '%s' scroll %.3f\n",disp("List 1").c_str(),e->getParameter(e,P("List Scroll")));
  std::string want=disp("List 3"); set("List Drag",1.0f-2.5f/12); wait(450); printf("tap row3 '%s' -> patch '%s'\n",want.c_str(),disp("Patch").c_str());
  // state roundtrip
  std::string before=disp("Patch"); set("List Drag",0.9f); wait(400); printf("drag strip value set before save; patch %s\n", disp("Patch").c_str()); before=disp("Patch");
  void* chunk=nullptr; int n=e->dispatcher(e,effGetChunk,0,0,&chunk,0); std::vector<char> c((char*)chunk,(char*)chunk+n);
  set("Arp On",0); set("Mod 3 Source",0);
  set("Patch",0.9f); wait(500); e->dispatcher(e,effSetChunk,0,n,c.data(),0); wait(900); printf("after restore patch %s (saved %s)\n",disp("Patch").c_str(),before.c_str()); printf("state: mod3 %s->%s\n",disp("Mod 3 Source").c_str(),disp("Mod 3 Dest").c_str());
  e->dispatcher(e,effClose,0,0,0,0); printf("done\n"); return 0; }
