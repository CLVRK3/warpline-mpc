// render_demo.cpp - renders a short audio demo of .vital patches through Warpline's engine (VitalCore), for
// listening to factory patches without a device. Writes one 44.1 kHz stereo 16-bit WAV per patch.
//
//   c++ -std=c++17 -O2 $(includes from build-extra.json) -Isrc tools/render_demo.cpp src/VitalCore.cpp src/VitalEngine.cpp \
//       -lpthread -o render_demo
//   ./render_demo <out dir> <bpm> <patch.vital>...
//
// Each patch plays a 4-bar figure at the given tempo, picked by its type code: chords (Pad, Pluck, Keys, Lead, Bell
// held as chords), a gliding 808 line (Bass), then 1.5 s of tail. Part of Warpline. GPLv3 (see LICENSE).

#include "VitalCore.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
  struct Ev { double beat; int note; bool on; };

  void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t n = static_cast<uint32_t>(l.size());
    f.write("RIFF", 4); u32(36 + n * 4); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(44100); u32(44100 * 4); u16(4); u16(16);
    f.write("data", 4); u32(n * 4);
    for (uint32_t i = 0; i < n; ++i) {
      int16_t a = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, l[i])) * 32767));
      int16_t b = static_cast<int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, r[i])) * 32767));
      f.write(reinterpret_cast<const char*>(&a), 2); f.write(reinterpret_cast<const char*>(&b), 2);
    }
  }

  // A minor-key progression (Am - F - C - G voicings), the usual dark rage loop.
  std::vector<Ev> chords(bool stabs) {
    const int prog[4][4] = { { 57, 60, 64, 69 }, { 53, 57, 60, 65 }, { 55, 60, 64, 67 }, { 55, 59, 62, 67 } };
    std::vector<Ev> e;
    for (int bar = 0; bar < 4; ++bar) {
      if (stabs) {
        const double hits[] = { 0.0, 0.75, 1.5, 2.5, 3.0 };
        for (double h : hits)
          for (int n : prog[bar]) { e.push_back({ bar * 4 + h, n, true }); e.push_back({ bar * 4 + h + 0.4, n, false }); }
      }
      else
        for (int n : prog[bar]) { e.push_back({ bar * 4.0, n, true }); e.push_back({ bar * 4 + 3.8, n, false }); }
    }
    return e;
  }

  std::vector<Ev> bassLine() {
    const double beats[] = { 0, 1.5, 3, 4, 5.5, 7, 8, 9.5, 11, 12, 13.5, 14.5, 15 };
    const int notes[] = { 33, 33, 36, 29, 29, 31, 33, 33, 40, 31, 31, 34, 36 };
    std::vector<Ev> e;
    for (int i = 0; i < 13; ++i) {
      double end = i < 12 ? beats[i + 1] : 16.0;
      e.push_back({ beats[i], notes[i], true });
      e.push_back({ end - 0.05, notes[i], false });
    }
    return e;
  }
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: render_demo <out dir> <bpm> <patch.vital>...\n"); return 1; }
  std::string out = argv[1];
  double bpm = std::atof(argv[2]);
  for (int a = 3; a < argc; ++a) {
    std::string path = argv[a];
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    VitalCore core;
    core.prepare(44100);
    core.setBpm(static_cast<float>(bpm));
    PatchReport report;
    if (!core.loadPatch(ss.str(), report)) { std::fprintf(stderr, "%s: did not load\n", path.c_str()); continue; }
    core.applyLimits(8, 8, 0, nullptr);   // as the MPC plays it by default (Eco)
    core.warmUp();
    std::string file = path.substr(path.find_last_of('/') + 1);
    std::string code = file.substr(0, 2);
    std::vector<Ev> ev = code == "BS" ? bassLine() : chords(code == "PL");
    double spb = 44100.0 * 60.0 / bpm;
    int total = static_cast<int>(16 * spb + 1.5 * 44100);
    std::vector<float> L(total), R(total);
    size_t k = 0;
    std::sort(ev.begin(), ev.end(), [](const Ev& x, const Ev& y) { return x.beat < y.beat || (x.beat == y.beat && !x.on && y.on); });
    for (int pos = 0; pos < total; pos += 64) {
      while (k < ev.size() && ev[k].beat * spb <= pos) {
        if (ev[k].on) core.noteOn(ev[k].note, 0.8f, 0, 0); else core.noteOff(ev[k].note, 0.5f, 0, 0);
        ++k;
      }
      int n = std::min(64, total - pos);
      core.render(L.data() + pos, R.data() + pos, n);
    }
    std::string name = file.substr(0, file.size() - 6);
    writeWav(out + "/" + name + ".wav", L, R);
    float peak = 0; for (int i = 0; i < total; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    std::printf("%-28s peak %.2f\n", name.c_str(), peak);
  }
  return 0;
}
