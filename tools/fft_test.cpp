// fft_test.cpp - checks the fast real inverse transform (vital/common/fourier_transform.h, FastRealInverse)
// against desktop Vital's definition in double precision, and against the kissfft inverse Warpline used
// before wherever the two definitions agree; then times both.
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
  constexpr int kH = kN / 2;

  // Desktop Vital's real inverse (JUCE / IPP / vDSP backends):
  //   out[n] = (1/N) (X[0] + X[N/2] (-1)^n + 2 Re(sum_{k=1}^{N/2-1} X[k] e^(+2 pi i k n / N)))
  // with X[0] = data[0], X[N/2] = data[N], X[k] = (data[2k], data[2k+1]). The rest of the buffer is ignored.
  std::vector<double> reference(const std::vector<float>& data) {
    std::vector<double> out(kN);
    const double two_pi = 6.283185307179586476925286766559;
    for (int n = 0; n < kN; ++n) {
      double sum = data[0] + data[kN] * ((n & 1) ? -1.0 : 1.0);
      for (int k = 1; k < kH; ++k) {
        double a = two_pi * static_cast<double>(static_cast<long long>(k) * n % kN) / kN;
        sum += 2.0 * (data[2 * k] * std::cos(a) - data[2 * k + 1] * std::sin(a));
      }
      out[n] = sum / kN;
    }
    return out;
  }

  struct Errors { double vs_formula, vs_kiss, peak; bool tail_zero; };

  Errors compare(vital::FourierTransform& fft, const std::vector<float>& input) {
    std::vector<float> a = input, b = input;
    fft.transformRealInverse(a.data());
    fft.transformRealInverseKiss(b.data());
    std::vector<double> r = reference(input);
    Errors e = { 0.0, 0.0, 0.0, true };
    for (int n = 0; n < kN; ++n) {
      e.vs_formula = std::max(e.vs_formula, std::fabs(a[n] - r[n]));
      e.vs_kiss = std::max(e.vs_kiss, std::fabs(static_cast<double>(a[n]) - b[n]));
      e.peak = std::max(e.peak, std::fabs(r[n]));
    }
    for (int i = kN; i < 2 * kN; ++i)
      e.tail_zero = e.tail_zero && a[i] == 0.0f;
    return e;
  }

  double db(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }
}

int main() {
  vital::FourierTransform fft(kBits);
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
  bool ok = true;

  // kiss_must_match: the old kissfft path agrees with desktop Vital only when the upper half of the buffer
  // (from data[N] on) is zero, so it is checked only for those inputs.
  auto run_case = [&](const char* name, const std::vector<float>& input, bool kiss_must_match) {
    Errors e = compare(fft, input);
    double form_rel = e.vs_formula / std::max(e.peak, 1e-30);
    double kiss_rel = e.vs_kiss / std::max(e.peak, 1e-30);
    bool pass = form_rel < 1e-5 && e.tail_zero && (!kiss_must_match || kiss_rel < 1e-5);
    std::printf("%-34s peak %.3e  vs desktop formula %.2e (%6.1f dB)  vs old kissfft %6.1f dB%s  tail %s  %s\n",
                name, e.peak, e.vs_formula, db(form_rel), db(kiss_rel), kiss_must_match ? " (must match)" : "",
                e.tail_zero ? "zero" : "NOT ZERO", pass ? "ok" : "FAIL");
    ok = ok && pass;
  };

  // 1. Random values everywhere, including the upper half (desktop Vital ignores it).
  for (int t = 0; t < 3; ++t) {
    std::vector<float> d(2 * kN);
    for (float& x : d) x = uni(rng);
    run_case("random, all 2N floats", d, false);
  }
  // 2. Bins 0..last only, upper half zero: old and new must agree.
  for (int last : { 1, 7, 100, 511, 1023 }) {
    std::vector<float> d(2 * kN, 0.0f);
    for (int k = 0; k <= last; ++k) {
      float amp = 1.0f / (1 + k);
      d[2 * k] = amp * uni(rng);
      d[2 * k + 1] = amp * uni(rng);
    }
    char name[64];
    std::snprintf(name, sizeof name, "harmonics 0..%d, rest zero", last);
    run_case(name, d, true);
  }
  // 3. What the oscillator buffers can hold: bins 0..1023, plus leftovers from 2048 on.
  {
    std::vector<float> d(2 * kN, 0.0f);
    for (int k = 0; k < kH; ++k) { d[2 * k] = uni(rng) / (1 + k); d[2 * k + 1] = uni(rng) / (1 + k); }
    for (int i = kN; i < kN + 8; ++i) d[i] = uni(rng);
    run_case("bins 0..1023 + leftovers at 2048+", d, false);
  }
  // 4. Single bins: DC, Nyquist (data[N]), 1, 1023.
  for (int k : { 0, 1024, 1, 1023 }) {
    std::vector<float> d(2 * kN, 0.0f);
    d[2 * k] = 1.0f;
    if (k != 0 && k != 1024) d[2 * k + 1] = -0.5f;
    char name[64];
    std::snprintf(name, sizeof name, "single bin %d", k);
    run_case(name, d, k != 1024);
  }

  // 5. Forward transform (wavetable frames): desktop Vital's layout, checked against a direct DFT, then a
  //    round trip through the inverse. Inputs end with a strong Nyquist component, the bin the old code dropped.
  {
    std::vector<double> cos_table(kN), sin_table(kN);
    const double two_pi = 6.283185307179586476925286766559;
    for (int i = 0; i < kN; ++i) { cos_table[i] = std::cos(two_pi * i / kN); sin_table[i] = std::sin(two_pi * i / kN); }
    for (int t = 0; t < 3; ++t) {
      std::vector<float> x(kN), buf(2 * kN, 0.0f);
      for (int n = 0; n < kN; ++n) x[n] = uni(rng) * 0.5f + ((n & 1) ? -0.4f : 0.4f);
      std::copy(x.begin(), x.end(), buf.begin());   // as WaveFrame::toFrequencyDomain does: upper half zero
      fft.transformRealForward(buf.data());

      double peak = 0.0, err = 0.0;
      for (int k = 0; k <= kH; ++k) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < kN; ++n) {
          int a = static_cast<int>((static_cast<long long>(k) * n) % kN);
          re += x[n] * cos_table[a];
          im -= x[n] * sin_table[a];
        }
        double got_re = k == kH ? buf[kN] : buf[2 * k];
        double got_im = (k == 0 || k == kH) ? 0.0 : buf[2 * k + 1];
        if (k == 0 || k == kH) im = 0.0;   // real for a real input
        peak = std::max(peak, std::hypot(re, im));
        err = std::max(err, std::hypot(got_re - re, got_im - im));
      }
      bool layout_ok = buf[1] == 0.0f && buf[kN + 1] == 0.0f;
      for (int i = kN + 2; i < 2 * kN; ++i)
        layout_ok = layout_ok && buf[i] == 0.0f;
      double nyquist_rel = std::fabs(buf[kN]) / peak;

      fft.transformRealInverse(buf.data());
      double rt_peak = 0.0, rt_err = 0.0;
      for (int n = 0; n < kN; ++n) {
        rt_peak = std::max(rt_peak, std::fabs(static_cast<double>(x[n])));
        rt_err = std::max(rt_err, std::fabs(static_cast<double>(buf[n]) - x[n]));
      }
      bool pass = err / peak < 1e-5 && layout_ok && nyquist_rel > 0.1 && rt_err / rt_peak < 1e-5;
      std::printf("%-34s vs direct DFT %6.1f dB  Nyquist bin %.3f of peak  layout %s  round trip %6.1f dB  %s\n",
                  "forward, random + Nyquist", db(err / peak), nyquist_rel, layout_ok ? "ok" : "WRONG",
                  db(rt_err / rt_peak), pass ? "ok" : "FAIL");
      ok = ok && pass;
    }
  }

  // Timing (host or emulator: only the ratio means anything).
  std::vector<float> d(2 * kN, 0.0f), work(2 * kN);
  for (int k = 0; k < kH; ++k) { d[2 * k] = uni(rng) / (1 + k); d[2 * k + 1] = uni(rng) / (1 + k); }
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
