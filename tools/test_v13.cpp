// Warpline 1.3 test: the idle skip (no engine render after about 1 s of silence) wakes on a note without losing
// the attack, and the arp keeps it awake; prints block times and levels.
//   g++ -std=c++17 -O1 -g -fsanitize=address -I<sdk>/framework tools/test_v13.cpp -o test_v13 -ldl
//   ASAN_OPTIONS=detect_leaks=0 ./test_v13 build/native/warpline.so
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

// Mean time of one 128-sample block over secs of rendering, in microseconds.
double blockUs(double secs){ auto t0=std::chrono::steady_clock::now(); run(secs); return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/(secs*44100/B); }
int main(int argc,char**argv){
  void* h=dlopen(argv[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  auto entry=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain"); e=entry(host);
  e->dispatcher(e,effOpen,0,0,0,0); e->dispatcher(e,effSetSampleRate,0,0,0,44100); e->dispatcher(e,effSetBlockSize,0,B,0,0);
  e->dispatcher(e,effMainsChanged,0,1,0,0); e->dispatcher(e,effStartProcess,0,0,0,0);
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamName,i,0,b,0); idx[b]=i; }
  printf("params %d version %d, Max Voices %s\n",e->numParams,(int)e->dispatcher(e,effGetVendorVersion,0,0,0,0),disp("Max Voices").c_str());
  wait(300);
  printf("block us, first 0.3 s (awake):    %.1f\n",blockUs(0.3));
  run(1.0);
  printf("block us, after 1.3 s of silence: %.1f\n",blockUs(0.5));
  midi(0x90,60,100); auto a=run(0.5); midi(0x80,60,0);
  printf("note after idle: rms first 10 ms %.4f, first 0.5 s %.4f\n",rms(a,0,441),rms(a,0,a.size()));
  auto t=run(3.0); printf("release tail rms %.5f, last 0.5 s %.6f\n",rms(t,0,22050),rms(t,t.size()-22050,t.size()));
  printf("block us, idle again:             %.1f\n",blockUs(0.5));
  set("Arp On",1); set("Arp Latch",1); wait(50);
  midi(0x90,60,100); midi(0x90,64,100); run(0.1); midi(0x80,60,0); midi(0x80,64,0);
  auto l=run(3.0); printf("latched arp after 3 s: onsets %d, rms last 0.5 s %.4f\n",onsets(l),rms(l,l.size()-22050,l.size()));
  set("Arp Latch",0); set("Arp On",0); wait(50); run(3.0);
  printf("block us, after the arp stopped:  %.1f\n",blockUs(0.5));
  return 0;
}
