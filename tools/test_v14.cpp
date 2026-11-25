// Warpline 1.4 test: chord mode, poly pressure, RANDOM, the LIGHT filter, macro names, your own WAV wavetables and
// the engine thread (its output must be the inline output, kEngineAhead blocks later); prints what it measures.
//   g++ -std=c++17 -O1 -g -fsanitize=address -I<sdk>/framework tools/test_v14.cpp -o test_v14 -ldl
//   WARPLINE_USER_TABLES=<dir with .wav> ASAN_OPTIONS=detect_leaks=0 ./test_v14 build/native/warpline.so
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


AEffect* open1(void* h){ auto entry=(AEffect*(*)(audioMasterCallback))dlsym(h,"VSTPluginMain"); AEffect* x=entry(host);
  x->dispatcher(x,effOpen,0,0,0,0); x->dispatcher(x,effSetSampleRate,0,0,0,44100); x->dispatcher(x,effSetBlockSize,0,B,0,0);
  x->dispatcher(x,effMainsChanged,0,1,0,0); x->dispatcher(x,effStartProcess,0,0,0,0); return x; }
int count(const char* n){ int c=0; float last=-1; for(int k=0;k<=200;k++){ float v=k/200.0f; char b[256]={0}; e->setParameter(e,P(n),v); e->dispatcher(e,effGetParamDisplay,P(n),0,b,0); (void)b; } (void)last; return c; }
// choice index -> normalised for a choice parameter with n choices
float ch(int i,int n){ return n>1? i/(float)(n-1):0; }
int main(int argc,char**argv){
  void* h=dlopen(argv[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  e=open1(h);
  for(int i=0;i<e->numParams;i++){ char b[256]={0}; e->dispatcher(e,effGetParamName,i,0,b,0); idx[b]=i; }
  printf("params %d version %d\n",e->numParams,(int)e->dispatcher(e,effGetVendorVersion,0,0,0,0));
  wait(300);
  // Chord mode
  midi(0x90,60,100); auto one=run(0.4); midi(0x80,60,0); run(1.5);
  set("Chord",ch(1,14)); wait(20);
  midi(0x90,60,100); auto three=run(0.4); midi(0x80,60,0); auto rel=run(2.0);
  printf("chord: single rms %.4f, Major rms %.4f, zero crossings %.0f vs %.0f, after release %.6f\n",rms(one,0,one.size()),rms(three,0,three.size()),zc(one),zc(three),rms(rel,rel.size()-22050,rel.size()));
  set("Chord Voicing",ch(4,5)); midi(0x90,60,100); midi(0x90,64,100); run(0.2); midi(0xA0,60,90); midi(0xD0,70,0); run(0.1); midi(0x80,60,0); midi(0x80,64,0); auto r2=run(2.5);
  printf("spread chords overlapping + pressure: after release %.6f\n",rms(r2,r2.size()-22050,r2.size()));
  midi(0x90,60,100); run(0.1); midi(0x90,60,100); run(0.1); midi(0x80,60,0); auto r3=run(2.5);
  printf("same key twice then one off: after release %.6f\n",rms(r3,r3.size()-22050,r3.size()));
  set("Arp On",1); midi(0x90,48,100); auto ar=run(1.0); midi(0x80,48,0); auto ar2=run(2.0); set("Arp On",0);
  printf("chord into arp: onsets %d, after release %.6f\n",onsets(ar),rms(ar2,ar2.size()-22050,ar2.size()));
  set("Chord",0); wait(20);
  // Macro names
  printf("macro names (Init): %s | %s\n",disp("Macro 1 Name").c_str(),disp("Macro 4 Name").c_str());
  set("Patch",ch(5,e->numParams>0? 520:1)); wait(400); run(0.1); wait(300);
  printf("macro names (%s): %s | %s | %s | %s\n",disp("Patch").c_str(),disp("Macro 1 Name").c_str(),disp("Macro 2 Name").c_str(),disp("Macro 3 Name").c_str(),disp("Macro 4 Name").c_str());
  // RANDOM inside a filter
  set("Fav Filter",ch(3,4)); wait(300); run(0.05); wait(200);
  printf("LIGHT filter: row1 '%s' cpu %s, row2 '%s' cpu %s\n",disp("List 1").c_str(),disp("List CPU 1").c_str(),disp("List 2").c_str(),disp("List CPU 2").c_str());
  std::string before=disp("Patch");
  for(int i=0;i<3;i++){ set("Random Patch",1); wait(400); run(0.05); wait(200); printf("random -> %s (Random Patch back to %s)\n",disp("Patch").c_str(),disp("Random Patch").c_str()); }
  set("Fav Filter",0); wait(200);
  // Your own wavetables
  std::string t0=disp("Osc1 Table");
  set("Osc1 WAV Next",1); wait(500); run(0.05); wait(200);
  std::string t1=disp("Osc1 Table");
  midi(0x90,60,100); auto w=run(0.3); midi(0x80,60,0); run(1.0);
  set("Osc1 WAV Next",1); wait(500); std::string t2=disp("Osc1 Table");
  set("Osc1 WAV Prev",1); wait(500); std::string t3=disp("Osc1 Table");
  printf("WAV tables: %s -> %s -> %s -> %s, note rms %.4f\n",t0.c_str(),t1.c_str(),t2.c_str(),t3.c_str(),rms(w,0,w.size()));

  // Engine thread: two fresh instances, Init, same notes; B's output should equal A's 2 blocks later.
  AEffect* a=open1(h); AEffect* b=open1(h);
  e=b; set("Engine Thread",1); wait(300);
  auto render2=[&](AEffect* x, std::vector<float>& out, int blocks, int note_at, int off_at){
    for(int i=0;i<blocks;i++){ e=x; if(i==note_at){ midi(0x90,57,100); midi(0x90,64,90);} if(i==off_at){ midi(0x80,57,0); midi(0x80,64,0);}
      if(!pend.empty()){ std::vector<char> buf(sizeof(VstEvents)+pend.size()*sizeof(void*)); VstEvents* ev=(VstEvents*)buf.data(); ev->numEvents=pend.size(); for(size_t k=0;k<pend.size();k++) ev->events[k]=(VstEvent*)&pend[k]; x->dispatcher(x,effProcessEvents,0,0,ev,0); pend.clear(); }
      x->processReplacing(x,nullptr,outs,B); for(int k=0;k<B;k++) out.push_back(L[k]); } };
  AEffect* c2=open1(h);
  std::vector<float> oa, ob, oc; render2(a,oa,400,20,200); render2(b,ob,400,20,200); render2(c2,oc,400,20,200);
  { double err=0, ref=0; for(size_t i=0;i<oa.size();i++){ double d=oa[i]-oc[i]; err+=d*d; ref+=oa[i]*oa[i]; } printf("  two inline instances, same input: relative error %.3g\n",std::sqrt(err/std::max(ref,1e-12))); }
  // onset of the note in each: first sample above 0.01
  auto onset=[](const std::vector<float>& v){ for(size_t i=0;i<v.size();i++) if(std::fabs(v[i])>0.01f) return (long)i; return -1L; };
  printf("  note onset: inline sample %ld, threaded sample %ld (difference %ld = %.2f blocks)\n",onset(oa),onset(ob),onset(ob)-onset(oa),(onset(ob)-onset(oa))/(double)B);
  int best=-1; double best_err=1e9;
  for(int lag=0; lag<=4*B; lag+=B){ double err=0, ref=0; for(size_t i=0;i+lag<ob.size()&&i<oa.size()-4*B;i++){ double d=oa[i]-ob[i+lag]; err+=d*d; ref+=oa[i]*oa[i]; } double rel=std::sqrt(err/std::max(ref,1e-12)); printf("  lag %d blocks: relative error %.3g\n",lag/B,rel); if(rel<best_err){best_err=rel;best=lag/B;} }
  printf("engine thread: best lag %d blocks, relative error %.3g, rms A %.4f B %.4f\n",best,best_err,rms(oa,0,oa.size()),rms(ob,0,ob.size()));
  // Toggle while playing, and timing
  e=b; auto t_on0=std::chrono::steady_clock::now(); midi(0x90,60,100); run(1.0); double us_on=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t_on0).count()/(44100/B);
  set("Engine Thread",0); run(0.3); set("Engine Thread",1); run(0.3); midi(0x80,60,0); auto tail=run(2.5);
  printf("toggled while playing: after release %.6f; audio-thread time per block, threaded: %.1f us\n",rms(tail,tail.size()-22050,tail.size()),us_on);
  e=a; auto t0b=std::chrono::steady_clock::now(); midi(0x90,60,100); run(1.0); midi(0x80,60,0); double us_off=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0b).count()/(44100/B);
  printf("audio-thread time per block, inline: %.1f us\n",us_off);
  // Paced like a real host (one 128-frame block every 2.9 ms): time spent inside processReplacing, chord held.
  auto paced=[&](AEffect* x){ e=x; midi(0x90,57,100); midi(0x90,60,100); midi(0x90,64,100); double tot=0, worst=0; int n=400;
    for(int i=0;i<n;i++){ auto t0=std::chrono::steady_clock::now();
      if(!pend.empty()){ std::vector<char> buf(sizeof(VstEvents)+pend.size()*sizeof(void*)); VstEvents* ev=(VstEvents*)buf.data(); ev->numEvents=pend.size(); for(size_t k=0;k<pend.size();k++) ev->events[k]=(VstEvent*)&pend[k]; x->dispatcher(x,effProcessEvents,0,0,ev,0); pend.clear(); }
      x->processReplacing(x,nullptr,outs,B);
      double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count(); tot+=us; worst=std::max(worst,us);
      std::this_thread::sleep_until(t0+std::chrono::microseconds(2902)); }
    midi(0x80,57,0); midi(0x80,60,0); midi(0x80,64,0); run(2.0); printf("    mean %.1f us, worst %.1f us\n",tot/n,worst); };
  printf("paced, inline:\n"); paced(a);
  printf("paced, engine thread:\n"); paced(b);
  for(AEffect* x: {a,b,c2}){ x->dispatcher(x,effStopProcess,0,0,0,0); x->dispatcher(x,effMainsChanged,0,0,0,0); x->dispatcher(x,effClose,0,0,0,0); }
  printf("done\n");
  return 0;
}
