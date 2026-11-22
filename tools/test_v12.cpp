// Warpline 1.2 feature test: the wavetable picker, the arp's chance / deja vu / loop, the Gen sources and
// a state round trip, driven through setParameter; prints what it measures.
//   g++ -std=c++17 -O1 -g -fsanitize=address -I<sdk>/framework tools/test_v12.cpp -o test_v12 -ldl
//   WARPLINE_FAVOURITES=/tmp/fav.txt ASAN_OPTIONS=detect_leaks=0 ./test_v12 build/native/warpline.so
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
float norm(const char* n, int choice, int count){ return count>1 ? choice/(float)(count-1) : 0; }
// zero crossings per second of the first 0.5 s: a rough brightness measure
double zc(const std::vector<float>& v){ int c=0; for(size_t i=1;i<v.size()&&i<22050;i++) if((v[i-1]<0)!=(v[i]<0)) c++; return c*2.0; }
std::vector<float> note(double secs){ midi(0x90,48,100); auto a=run(secs); midi(0x80,48,0); run(0.6); return a; }
int main(int argc,char**argv){
  void* h=dlopen(argv[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  auto entry=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain"); e=entry(host);
  e->dispatcher(e,effOpen,0,0,0,0); e->dispatcher(e,effSetSampleRate,0,0,0,44100); e->dispatcher(e,effSetBlockSize,0,B,0,0);
  e->dispatcher(e,effMainsChanged,0,1,0,0); e->dispatcher(e,effStartProcess,0,0,0,0);
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamName,i,0,b,0); idx[b]=i; }
  printf("params %d version %d\n",e->numParams,(int)e->dispatcher(e,effGetVendorVersion,0,0,0,0));
  wait(300); run(0.2);
  printf("init: Osc1 Table = %s, Osc2 Table = %s\n",disp("Osc1 Table").c_str(),disp("Osc2 Table").c_str());
  // Every table on OSC 1, at WT POS 0 and 1.
  const char* names[]={"Patch","Basic Shapes","Sine","Triangle","Saw","Square","PWM","Harmonics","Formant","Sync"};
  for(int t=0;t<10;t++){
    set("Osc1 Table",norm("",t,10)); wait(200);
    set("Osc1 WT Pos",0); wait(30); auto a=note(0.5);
    set("Osc1 WT Pos",1); wait(30); auto b=note(0.5);
    printf("table %-12s shown %-12s rms %.3f/%.3f  zc/s %6.0f/%6.0f\n",names[t],disp("Osc1 Table").c_str(),rms(a,0,a.size()),rms(b,0,b.size()),zc(a),zc(b));
  }
  set("Osc1 WT Pos",0);
  // Gen 1 -> Osc1 Level: the level should move from tick to tick.
  set("Osc1 Table",norm("",4,10)); wait(200);
  set("Gen Rate",norm("",3,11)); set("Gen Steps",1); set("Gen Deja Vu",0); set("Gen Length",(8-1)/15.0f);
  set("Mod 1 Source",norm("",16,18)); set("Mod 1 Dest",0); // set dest by display search below
  int dests=0; { char b[256]; for(float v=0;v<=1.0001f;v+=1/44.0f){ e->setParameter(e,P("Mod 1 Dest"),v); e->dispatcher(e,effGetParamDisplay,P("Mod 1 Dest"),0,b,0); if(std::string(b)=="Osc1 Level") break; dests++; } }
  set("Mod 1 Amount",0.5f+0.4f); set("Panel",6/6.0f); wait(300);
  printf("mod 1: %s -> %s (%s)\n",disp("Mod 1 Source").c_str(),disp("Mod 1 Dest").c_str(),disp("Mod 1 Amount").c_str());
  auto runGen=[&](const char* label){ midi(0x90,48,100); auto a=run(4.0); midi(0x80,48,0); run(0.5);
    // RMS per 1/8 (0.25 s at 120 bpm), skip the attack
    std::vector<double> r; for(size_t i=11025;i+11025<=a.size();i+=11025) r.push_back(rms(a,i+2000,i+9000));
    printf("%s:",label); for(double x:r) printf(" %.3f",x); printf("\n"); return r; };
  auto g1=runGen("gen dv=0   ");
  set("Gen Deja Vu",0.5f); set("Gen Length",(4-1)/15.0f); wait(50);
  auto g2=runGen("gen dv=50% L4");
  auto g3=runGen("gen dv=50% L4 again");
  printf("gen 1 out display %s\n",disp("Gen 1 Out").c_str());
  set("Mod 1 Source",0); wait(200);
  // Arp: chance and deja vu.
  set("Osc1 Table",0); wait(200);
  set("Arp On",1); set("Arp Gate",(25-5)/95.0f); wait(50);
  auto arp=[&](const char* label){ midi(0x90,60,100); midi(0x90,64,100); midi(0x90,67,100); auto a=run(4.0); midi(0x80,60,0);midi(0x80,64,0);midi(0x80,67,0); run(0.6);
    printf("%s onsets %d\n",label,onsets(a)); return a; };
  arp("arp chance 100   (expect ~32)");
  set("Arp Chance",0.5f); wait(50); arp("arp chance 50    (expect ~16)");
  set("Arp Chance",0.0f); wait(50); arp("arp chance 0     (expect 0)");
  set("Arp Chance",0.5f); set("Arp Deja Vu",0.5f); set("Arp Loop",(4-1)/15.0f); wait(50);
  printf("arp: chance %s deja vu %s loop %s\n",disp("Arp Chance").c_str(),disp("Arp Deja Vu").c_str(),disp("Arp Loop").c_str());
  arp("arp chance 50 locked loop 4");
  set("Arp Mode",norm("",4,7)); set("Arp Chance",1); wait(50); arp("arp random locked");
  set("Arp On",0); set("Arp Chance",1); set("Arp Deja Vu",0);
  // State round trip with a picked table.
  set("Osc2 Table",norm("",7,10)); set("Gen Spread",0.8f); wait(300);
  void* chunk=nullptr; VstIntPtr sz=e->dispatcher(e,effGetChunk,0,0,&chunk,0); std::vector<char> st((char*)chunk,(char*)chunk+sz);
  set("Osc2 Table",0); set("Gen Spread",0.5f); wait(300);
  e->dispatcher(e,effSetChunk,0,(VstIntPtr)st.size(),st.data(),0); wait(400);
  printf("restored: Osc2 Table %s, Gen Spread %s, Osc1 Table %s\n",disp("Osc2 Table").c_str(),disp("Gen Spread").c_str(),disp("Osc1 Table").c_str());
  auto a=note(0.5); printf("after restore rms %.3f\n",rms(a,0,a.size()));
  // Patch change resets the picks.
  set("Patch",0.5f); wait(2500);
  printf("after patch change: Osc1 %s Osc2 %s\n",disp("Osc1 Table").c_str(),disp("Osc2 Table").c_str());
  set("Patch",0.0f); wait(2500);
  printf("after Init: Osc1 %s Osc2 %s\n",disp("Osc1 Table").c_str(),disp("Osc2 Table").c_str());
  // 1.2 menu: a pick on the big TABLES menu closes it.
  set("Panel",8/9.0f); wait(100); printf("menu open: Panel %s\n",disp("Panel").c_str());
  set("Osc2 Table",norm("",3,10)); wait(400);
  printf("after pick: Panel %s, Osc2 Table %s\n",disp("Panel").c_str(),disp("Osc2 Table").c_str());
  // USER filter: no own patches here, so the list is empty; ALL shows factory rows without the tag.
  set("Fav Filter",1.0f); wait(300); printf("USER filter: row1 '%s' tag %s\n",disp("List 1").c_str(),disp("List User 1").c_str());
  set("Fav Filter",0.0f); wait(300); printf("ALL: row1 '%s' tag %s\n",disp("List 1").c_str(),disp("List User 1").c_str());
  e->dispatcher(e,effStopProcess,0,0,0,0); e->dispatcher(e,effMainsChanged,0,0,0,0); e->dispatcher(e,effClose,0,0,0,0);
  printf("done\n"); return 0;
}
