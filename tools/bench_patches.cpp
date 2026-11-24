// bench_patches.cpp - ranks .vital patches by the CPU Warpline's engine (VitalCore) needs to play them, to find the
// factory patches that are too heavy for Gen 1 MPCs. x86 numbers are only relative: compare patches with each other.
//
//   c++ -std=c++17 -O2 $(includes from build-extra.json) -Isrc tools/bench_patches.cpp src/VitalCore.cpp src/VitalEngine.cpp \
//       -lpthread -o bench_patches
//   ./bench_patches <max voices> <max unison> <patch.vital>...
//
// Each patch holds a 4-note chord for 2 s, then 1 s of release, in 128-sample blocks at 44.1 kHz with the given limits
// (Eco oversampling). Prints the share of real time each patch used, heaviest first. Part of Warpline. GPLv3 (see LICENSE).

#include "VitalCore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: bench_patches <max voices> <max unison> <patch.vital>...\n"); return 1; }
  int voices = std::atoi(argv[1]);
  int unison = std::atoi(argv[2]);
  const int kRate = 44100, kBlock = 128, kHold = 2 * kRate, kTotal = 3 * kRate;
  const int chord[4] = { 57, 60, 64, 69 };
  std::vector<float> L(kBlock), R(kBlock);
  std::vector<std::pair<double, std::string>> rows;
  for (int a = 3; a < argc; ++a) {
    std::string path = argv[a];
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    VitalCore core;
    core.prepare(kRate);
    core.setBpm(120.0f);
    PatchReport report;
    if (!core.loadPatch(ss.str(), report)) { std::fprintf(stderr, "%s: did not load\n", path.c_str()); continue; }
    core.applyLimits(unison, voices, 0, nullptr);
    if (const char* off = std::getenv("BENCH_OFF"))   // e.g. BENCH_OFF=reverb_on,chorus_on: switch those controls off
      for (std::stringstream list(off); std::getline(list, path, ',');)
        core.setControl(path, 0.0f);
    path = argv[a];
    core.warmUp();
    auto t0 = std::chrono::steady_clock::now();
    for (int n : chord) core.noteOn(n, 0.8f, 0, 0);
    for (int pos = 0; pos < kTotal; pos += kBlock) {
      if (pos == kHold) for (int n : chord) core.noteOff(n, 0.5f, 0, 0);
      core.render(L.data(), R.data(), kBlock);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string file = path.substr(path.find_last_of('/') + 1);
    rows.push_back({ 100.0 * s / 3.0, file.substr(0, file.size() - 6) });
  }
  std::sort(rows.rbegin(), rows.rend());
  for (auto& r : rows)
    std::printf("%6.2f%%  %s\n", r.first, r.second.c_str());
  return 0;
}
