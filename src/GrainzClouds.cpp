// GRAINZ texture modes. This file is the only one that includes the third-party processor (third_party/mutable,
// MIT licence, copyright Emilie Gillet): it builds it as one unit with TEST defined, which selects its portable code
// paths (no Cortex-M4 assembly, no hardware drivers). See GrainzClouds.h.
#define TEST 1

#include "clouds/dsp/correlator.cc"
#include "clouds/dsp/granular_processor.cc"
#include "clouds/dsp/mu_law.cc"
#include "clouds/dsp/pvoc/frame_transformation.cc"
#include "clouds/dsp/pvoc/phase_vocoder.cc"
#include "clouds/dsp/pvoc/stft.cc"
#include "clouds/resources.cc"
#include "stmlib/dsp/atan.cc"
#include "stmlib/dsp/units.cc"
#include "stmlib/utils/random.cc"

#include "GrainzClouds.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kCloudsRate = 32000.0;
constexpr int kBlock = 32;                      // the processor's block size
constexpr size_t kLargeBuffer = 118784;         // its two memory areas, as on the module
constexpr size_t kSmallBuffer = 65536 - 128;
constexpr int kFifo = 16384;                    // frames, power of two
constexpr int kFifoMask = kFifo - 1;
constexpr int kOutPrime = 48;                   // frames of silence ahead of the first output, so reads never starve

// Two biquads = 4th-order Butterworth low-pass, for the band limit around the rate changes.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void lowpass(double fc, double fs, double q) {
    double w = 2.0 * M_PI * fc / fs, c = std::cos(w), s = std::sin(w), alpha = s / (2.0 * q);
    double a0 = 1.0 + alpha;
    b0 = static_cast<float>((1.0 - c) * 0.5 / a0);
    b1 = static_cast<float>((1.0 - c) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2.0 * c / a0);
    a2 = static_cast<float>((1.0 - alpha) / a0);
  }
  float run(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void clear() { z1 = z2 = 0.0f; }
};

struct LowPass4 {
  Biquad a, b;
  void set(double fc, double fs) { a.lowpass(fc, fs, 0.5412); b.lowpass(fc, fs, 1.3066); }
  float run(float x) { return b.run(a.run(x)); }
  void clear() { a.clear(); b.clear(); }
};

inline float hermite(float xm1, float x0, float x1, float x2, float t) {
  float c1 = 0.5f * (x1 - xm1);
  float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
  float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
  return ((c3 * t + c2) * t + c1) * t + x0;
}

inline float softClip(float x) {
  x = std::max(-3.0f, std::min(3.0f, x));
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline short toShort(float x) {
  x = std::max(-1.0f, std::min(1.0f, x));
  return static_cast<short>(std::lrint(x * 32767.0f));
}

}  // namespace

struct GrainzClouds::Impl {
  clouds::GranularProcessor processor;
  std::vector<uint8_t> large, small;
  double rate = 44100.0;
  bool band_limit = true;
  LowPass4 in_lp[2], out_lp[2];

  // Host rate -> 32 kHz.
  float down_hist[2][4] = {};
  double down_t = 0.0, down_step = 1.0;
  std::vector<clouds::ShortFrame> in_fifo;
  int in_count = 0;                                 // frames waiting (always from index 0)

  // 32 kHz -> host rate.
  std::vector<float> out_l, out_r;                  // ring
  int out_read = 0, out_write = 0;
  float up_hist[2][4] = {};
  double up_t = 0.0, up_step = 1.0;

  clouds::ShortFrame block_in[kBlock], block_out[kBlock];

  int outAvailable() const { return (out_write - out_read) & kFifoMask; }
  void pushOut(float l, float r) {
    out_l[out_write] = l;
    out_r[out_write] = r;
    out_write = (out_write + 1) & kFifoMask;
  }
  void popOut(float& l, float& r) {
    if (outAvailable() == 0) { l = r = 0.0f; return; }   // never expected (primed), but stay safe
    l = out_l[out_read];
    r = out_r[out_read];
    out_read = (out_read + 1) & kFifoMask;
  }
};

GrainzClouds::GrainzClouds() = default;
GrainzClouds::~GrainzClouds() = default;

void GrainzClouds::prepare(double sample_rate) {
  if (!impl_)
    impl_ = std::make_unique<Impl>();
  Impl& m = *impl_;
  m.rate = sample_rate > 1000.0 ? sample_rate : 44100.0;
  m.large.assign(kLargeBuffer, 0);
  m.small.assign(kSmallBuffer, 0);
  m.processor.Init(m.large.data(), m.large.size(), m.small.data(), m.small.size());
  m.processor.set_num_channels(2);
  m.processor.set_low_fidelity(false);
  m.processor.set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
  m.in_fifo.assign(kFifo, clouds::ShortFrame{ 0, 0 });
  m.out_l.assign(kFifo, 0.0f);
  m.out_r.assign(kFifo, 0.0f);
  m.down_step = m.rate / kCloudsRate;
  m.up_step = kCloudsRate / m.rate;
  m.band_limit = m.rate > 30000.0;
  for (int c = 0; c < 2; ++c) {
    m.in_lp[c].set(13500.0, m.rate);
    m.out_lp[c].set(13500.0, m.rate);
  }
  reset();
}

void GrainzClouds::reset() {
  if (!impl_)
    return;
  Impl& m = *impl_;
  for (int c = 0; c < 2; ++c) {
    m.in_lp[c].clear();
    m.out_lp[c].clear();
    for (float& v : m.down_hist[c]) v = 0.0f;
    for (float& v : m.up_hist[c]) v = 0.0f;
  }
  m.down_t = 0.0;
  m.up_t = 0.0;
  m.in_count = 0;
  m.out_read = m.out_write = 0;
  for (int i = 0; i < kOutPrime; ++i)
    m.pushOut(0.0f, 0.0f);
}

void GrainzClouds::process(const float* in_l, const float* in_r, float* wet_l, float* wet_r, int frames) {
  if (!impl_) {
    std::fill(wet_l, wet_l + frames, 0.0f);
    std::fill(wet_r, wet_r + frames, 0.0f);
    return;
  }
  Impl& m = *impl_;

  static const clouds::PlaybackMode kModes[kNumModes] = {
    clouds::PLAYBACK_MODE_GRANULAR, clouds::PLAYBACK_MODE_STRETCH, clouds::PLAYBACK_MODE_LOOPING_DELAY,
    clouds::PLAYBACK_MODE_SPECTRAL
  };
  m.processor.set_playback_mode(kModes[std::clamp(settings_.mode, 0, kNumModes - 1)]);
  // The processor's lookup tables are read as Interpolate(table, x, size): x = 1.0 reads one past the end
  // (ASan, offline 2026-11-19), so 0..1 controls stop just short of 1.
  constexpr float kTop = 0.9999f;
  clouds::Parameters* p = m.processor.mutable_parameters();
  p->position = std::clamp(settings_.position, 0.0f, kTop);
  p->size = std::clamp(settings_.size, 0.0f, kTop);
  p->pitch = std::clamp(settings_.pitch, -48.0f, 48.0f);
  p->density = std::clamp(settings_.density, 0.0f, kTop);
  p->texture = std::clamp(settings_.texture, 0.0f, kTop);
  p->dry_wet = kTop;                       // wet only: Warpline mixes
  p->stereo_spread = std::clamp(settings_.spread, 0.0f, kTop);
  p->feedback = std::clamp(settings_.feedback, 0.0f, kTop);
  p->reverb = std::clamp(settings_.reverb, 0.0f, kTop);
  p->freeze = settings_.freeze;
  p->gate = false;

  // 1. Down to 32 kHz.
  for (int i = 0; i < frames; ++i) {
    float x[2] = { in_l[i], in_r[i] };
    for (int c = 0; c < 2; ++c) {
      float v = m.band_limit ? m.in_lp[c].run(x[c]) : x[c];
      float* h = m.down_hist[c];
      h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = v;
    }
    while (m.down_t < 1.0) {
      float t = static_cast<float>(m.down_t);
      if (m.in_count < kFifo) {
        clouds::ShortFrame& f = m.in_fifo[m.in_count++];
        f.l = toShort(hermite(m.down_hist[0][0], m.down_hist[0][1], m.down_hist[0][2], m.down_hist[0][3], t));
        f.r = toShort(hermite(m.down_hist[1][0], m.down_hist[1][1], m.down_hist[1][2], m.down_hist[1][3], t));
      }
      m.down_t += m.down_step;
    }
    m.down_t -= 1.0;
  }

  // 2. Run the processor on every whole block.
  int done = 0;
  while (m.in_count - done >= kBlock) {
    std::copy(&m.in_fifo[done], &m.in_fifo[done] + kBlock, m.block_in);
    p->trigger = trigger_;
    trigger_ = false;
    m.processor.Prepare();
    m.processor.Process(m.block_in, m.block_out, kBlock);
    for (int i = 0; i < kBlock; ++i)
      m.pushOut(m.block_out[i].l / 32768.0f, m.block_out[i].r / 32768.0f);
    done += kBlock;
  }
  if (done > 0) {
    std::copy(&m.in_fifo[done], &m.in_fifo[m.in_count], &m.in_fifo[0]);
    m.in_count -= done;
  }

  // 3. Back up to the host rate.
  for (int i = 0; i < frames; ++i) {
    while (m.up_t >= 1.0) {
      float l, r;
      m.popOut(l, r);
      float* hl = m.up_hist[0];
      float* hr = m.up_hist[1];
      hl[0] = hl[1]; hl[1] = hl[2]; hl[2] = hl[3]; hl[3] = l;
      hr[0] = hr[1]; hr[1] = hr[2]; hr[2] = hr[3]; hr[3] = r;
      m.up_t -= 1.0;
    }
    float t = static_cast<float>(m.up_t);
    float l = hermite(m.up_hist[0][0], m.up_hist[0][1], m.up_hist[0][2], m.up_hist[0][3], t);
    float r = hermite(m.up_hist[1][0], m.up_hist[1][1], m.up_hist[1][2], m.up_hist[1][3], t);
    l = m.band_limit ? m.out_lp[0].run(l) : l;
    r = m.band_limit ? m.out_lp[1].run(r) : r;
    // Interpolation and the band-limit filter can overshoot a full-scale wet signal: keep it under 0 dBFS.
    wet_l[i] = softClip(l);
    wet_r[i] = softClip(r);
    m.up_t += m.up_step;
  }
}
