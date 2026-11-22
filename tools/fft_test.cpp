// fft_test.cpp - checks the fast real inverse transform (vital/common/fourier_transform.h, FastRealInverse)
// against the original kissfft inverse and against the formula itself in double precision, then times both.
//
// Build and run from the project folder, for example:
//   zig c++ -std=c++17 -O2 -Ishim -Ivital/common -Ivital/third_party tools/fft_test.cpp -o fft_test && ./fft_test
// For the ARM (NEON) path, build with -target arm-linux-musleabihf -mcpu=cortex_a17 and run it under qemu-arm.
// Exit code 0 when every case is within tolerance.
#include "JuceHeader.h"
#include "fourier_transform.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace {
  constexpr int kBits = 11;
  constexpr int kN = 1 << kBits;

  // The definition the kissfft inverse implements: out[n] = (2/N) Re(sum_k Y[k] e^(+2 pi i k n / N)),
  // with Y[0] = (data[0] / 2, data[N]) and Y[k] = (data[2k], data[2k+1]) otherwise.
  std::vector<double> reference(const std::vector<float>& data) {
    std::vector<double> out(kN);
    const double two_pi = 6.283185307179586476925286766559;
    for (int n = 0; n < kN; ++n) {
      double sum = 0.5 * data[0];   // Re(Y[0])
      for (int k = 1; k < kN; ++k) {
        double a = two_pi * static_cast<double>(static_cast<long long>(k) * n % kN) / kN;
        sum += data[2 * k] * std::cos(a) - data[2 * k + 1] * std::sin(a);
      }
      out[n] = 2.0 * sum / kN;
    }
    return out;
  }

  struct Errors { double vs_kiss, vs_formula, peak; bool tail_zero; };

  Errors compare(vital::FourierTransform& fft, const std::vector<float>& input) {
    std::vector<float> a = input, b = input;
    fft.transformRealInverse(a.data());
    fft.transformRealInverseKiss(b.data());
    std::vector<double> r = reference(input);
    Errors e = { 0.0, 0.0, 0.0, true };
    for (int n = 0; n < kN; ++n) {
      e.vs_kiss = std::max(e.vs_kiss, std::fabs(static_cast<double>(a[n]) - b[n]));
      e.vs_formula = std::max(e.vs_formula, std::fabs(a[n] - r[n]));
      e.peak = std::max(e.peak, std::fabs(r[n]));
    }
    for (int i = kN; i < 2 * kN; ++i)
      e.tail_zero = e.tail_zero && a[i] == 0.0f;
    return e;
  }
}

int main() {
  vital::FourierTransform fft(kBits);
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
  bool ok = true;

  auto run_case = [&](const char* name, const std::vector<float>& input) {
    Errors e = compare(fft, input);
    double kiss_rel = e.vs_kiss / std::max(e.peak, 1e-30);
    double form_rel = e.vs_formula / std::max(e.peak, 1e-30);
    bool pass = kiss_rel < 1e-5 && form_rel < 1e-5 && e.tail_zero;
    std::printf("%-34s peak %.3e  max diff vs kissfft %.2e (%.1f dB)  vs formula %.2e (%.1f dB)  tail %s  %s\n",
                name, e.peak, e.vs_kiss, 20.0 * std::log10(std::max(kiss_rel, 1e-30)), e.vs_formula,
                20.0 * std::log10(std::max(form_rel, 1e-30)), e.tail_zero ? "zero" : "NOT ZERO", pass ? "ok" : "FAIL");
    ok = ok && pass;
  };

  // 1. Random values everywhere, including the upper half the oscillator normally leaves at zero.
  for (int t = 0; t < 3; ++t) {
    std::vector<float> d(2 * kN);
    for (float& x : d) x = uni(rng);
    run_case("random, all 2N floats", d);
  }
  // 2. What the oscillator writes: harmonics 0..last_harmonic+1 (so bins 1024 and 1025 can be set), rest zero.
  for (int last : { 1, 7, 100, 511, 1023, 1024 }) {
    std::vector<float> d(2 * kN, 0.0f);
    for (int k = 0; k <= std::min(last + 1, kN - 1); ++k) {
      float amp = 1.0f / (1 + k);
      d[2 * k] = amp * uni(rng);
      d[2 * k + 1] = amp * uni(rng);
    }
    char name[64];
    std::snprintf(name, sizeof name, "harmonics 0..%d", last + 1);
    run_case(name, d);
  }
  // 3. Single bins: DC, Nyquist, 1, 1023, 1025.
  for (int k : { 0, 1024, 1, 1023, 1025 }) {
    std::vector<float> d(2 * kN, 0.0f);
    d[2 * k] = 1.0f;
    if (k != 0 && k != 1024) d[2 * k + 1] = -0.5f;
    char name[64];
    std::snprintf(name, sizeof name, "single bin %d", k);
    run_case(name, d);
  }

  // Timing (host or emulator: only the ratio means anything).
  std::vector<float> d(2 * kN, 0.0f), work(2 * kN);
  for (int k = 0; k <= 1024; ++k) { d[2 * k] = uni(rng) / (1 + k); d[2 * k + 1] = uni(rng) / (1 + k); }
  const int reps = 2000;
  auto time_it = [&](bool fast) {
    auto t0 = std::chrono::steady_clock::now();
    float sink = 0.0f;
    for (int r = 0; r < reps; ++r) {
      std::copy(d.begin(), d.end(), work.begin());
      if (fast) fft.transformRealInverse(work.data());
      else fft.transformRealInverseKiss(work.data());
      sink += work[r % kN];
    }
    auto t1 = std::chrono::steady_clock::now();
    if (sink == 12345.0f) std::printf(" ");
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
  };
  time_it(true); time_it(false);
  double fast_us = time_it(true), kiss_us = time_it(false);
  std::printf("time per inverse transform: fast %.2f us, kissfft %.2f us (%.1fx)\n", fast_us, kiss_us, kiss_us / fast_us);
  std::printf("%s\n", ok ? "ALL OK" : "SOME CASES FAILED");
  return ok ? 0 : 1;
}
