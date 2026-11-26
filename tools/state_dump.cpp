// (helpers shared with tools/test_v14.cpp)
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



// state_dump.cpp - plays a plugin like the tests do, then prints a state.json for tools_preview_skin.py
// (normalised value + display text of every parameter). Settings are "Param Name=normalised" or "Param Name~choice text"
// (the first choice whose text matches), applied in order; "note=60" holds a note, "wait=ms" waits and renders.
//   g++ -std=c++17 -O1 -I<sdk>/framework tools/state_dump.cpp -o state_dump -ldl
//   ./state_dump build/native/warpline.so "Patch~LD Wavy Saw Lead" "note=57" "wait=500" > state.json
//   ./state_dump build/native/warpline.so --choices Patch > patch_names.json   (the factory list for the skin, 1.4)
int main(int argc,char**argv){
  void* h=dlopen(argv[1],RTLD_NOW); if(!h){fprintf(stderr,"%s\n",dlerror());return 1;}
  auto entry=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain"); e=entry(host);
  e->dispatcher(e,effOpen,0,0,0,0); e->dispatcher(e,effSetSampleRate,0,0,0,44100); e->dispatcher(e,effSetBlockSize,0,B,0,0);
  e->dispatcher(e,effMainsChanged,0,1,0,0); e->dispatcher(e,effStartProcess,0,0,0,0);
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamName,i,0,b,0); idx[b]=i; }
  wait(300); run(0.05);
  if(argc>3 && std::string(argv[2])=="--choices"){   // the display texts of a parameter's choices, in order, as JSON
    int p=P(argv[3]); std::vector<std::string> seen; for(int k=0;k<=200000;k++){ e->setParameter(e,p,k/200000.0f); char b[256]={0}; e->dispatcher(e,effGetParamDisplay,p,0,b,0); if(seen.empty()||seen.back()!=b) seen.push_back(b); }
    printf("["); for(size_t i=0;i<seen.size();i++){ std::string d; for(char c: seen[i]){ if(c=='"'||c=='\\') d+='\\'; d+=c; } printf("%s\"%s\"",i?", ":"",d.c_str()); } printf("]\n"); return 0; }
  for(int a=2;a<argc;a++){ std::string s=argv[a]; size_t eq=s.find('='), tl=s.find('~');
    if(s.rfind("note=",0)==0){ midi(0x90,atoi(s.c_str()+5),100); run(0.05); continue; }
    if(s.rfind("wait=",0)==0){ int ms=atoi(s.c_str()+5); for(int t=0;t<ms;t+=20){ wait(20); run(0.02);} continue; }
    if(tl!=std::string::npos && (eq==std::string::npos || tl<eq)){ std::string n=s.substr(0,tl), want=s.substr(tl+1); int p=P(n.c_str()); bool ok=false;
      int lo=-1, hi=-1;   // the middle of the values that show the wanted text
      for(int k=0;k<=4000;k++){ float v=k/4000.0f; e->setParameter(e,p,v); char b[256]={0}; e->dispatcher(e,effGetParamDisplay,p,0,b,0); if(want==b){ if(lo<0) lo=k; hi=k; } else if(lo>=0) break; }
      ok = lo>=0; if(ok) e->setParameter(e,p,(lo+hi)/8000.0f);
      if(!ok) fprintf(stderr,"no choice '%s' for %s\n",want.c_str(),n.c_str()); continue; }
    if(eq!=std::string::npos){ set(s.substr(0,eq).c_str(), (float)atof(s.c_str()+eq+1)); continue; }
  }
  wait(400); run(0.1); wait(300); run(0.05);
  printf("{\"params\": [\n");
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamDisplay,i,0,b,0); std::string d;
    for(char* c=b;*c;c++){ if(*c=='"'||*c=='\\') d+='\\'; d+=*c; }
    printf("  {\"i\": %d, \"v\": %.6f, \"display\": \"%s\"}%s\n",i,e->getParameter(e,i),d.c_str(),i+1<e->numParams?",":""); }
  printf("]}\n");
  return 0;
}
