// Granulator.h - Warpline's granular effect, after Vital's own effects.
//
// Part of Warpline. GPLv3 (see LICENSE).
//
// The last 1.5 s of the synth's output is kept in a ring buffer. Grains are short windowed pieces read
// from it, each with its own start, length, pitch, direction and pan. "Space" runs the grains through
// a small allpass diffuser, "Feedback" writes some of the grains back into the buffer, "Freeze" stops
// writing so the buffer holds what is in it now.
//
// Audio thread only, after prepare(). No allocation in process().

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

class Granulator {
  public:
    static constexpr double kBufferSeconds = 1.5;
    static constexpr int kMaxGrains = 40;
    static constexpr int kWindowSize = 1024;
    static constexpr int kNumSyncRates = 7;

    struct Settings {
      bool on = false;
      float size_ms = 120.0f;     // GRAIN: grain length, 10..500 ms
      float scatter = 0.3f;       // SCATTER: random start time (up to 400 ms) and timing jitter, 0..1
      float space = 0.3f;         // SPACE: diffusion, 0..1
      float mix = 0.5f;           // MIX, 0..1
      float density = 20.0f;      // grains per second, 1..80 (free)
      float position_ms = 80.0f;  // how far back grains start, 0..1000 ms
      float pitch = 0.0f;         // semitones, -24..24
      float pitch_rand = 0.0f;    // random pitch spread, semitones 0..24
      float spread = 0.5f;        // stereo spread, 0..1
      float reverse = 0.0f;       // chance a grain plays backwards, 0..1
      float feedback = 0.0f;      // 0..0.9
      bool freeze = false;
      int sync = 0;               // 0 = free (density), 1.. = one grain per 1/4, 1/8, 1/16, 1/32, 1/8T, 1/16T
    };

    void prepare(double sample_rate) {
      sample_rate_ = sample_rate > 0.0 ? sample_rate : 44100.0;
      int length = static_cast<int>(kBufferSeconds * sample_rate_) + 4;
      buffer_l_.assign(length, 0.0f);
      buffer_r_.assign(length, 0.0f);
      write_ = 0;
      for (int i = 0; i < kWindowSize; ++i)
        window_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (kWindowSize - 1));
      static const double kAllpassMs[kNumAllpass] = { 4.77, 3.59, 12.73, 9.31, 7.53, 5.71 };
      for (int i = 0; i < kNumAllpass; ++i) {
        int len = std::max(2, static_cast<int>(kAllpassMs[i] * 0.001 * sample_rate_));
        allpass_[i].buffer.assign(len, 0.0f);
        allpass_[i].pos = 0;
      }
      reset();
    }

    void reset() {
      std::fill(buffer_l_.begin(), buffer_l_.end(), 0.0f);
      std::fill(buffer_r_.begin(), buffer_r_.end(), 0.0f);
      for (Grain& g : grains_) g.active = false;
      for (Allpass& a : allpass_) std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
      until_next_grain_ = 0.0;
      fb_l_ = fb_r_ = 0.0f;
      wet_gain_ = 0.0f;
      mix_smooth_ = 0.0f;
    }

    void setSettings(const Settings& s) { s_ = s; }
    const Settings& settings() const { return s_; }
    void setTempo(double bpm) { bpm_ = bpm > 1.0 ? bpm : 120.0; }

    int activeGrains() const {
      int n = 0;
      for (const Grain& g : grains_) n += g.active ? 1 : 0;
      return n;
    }

    void process(float* left, float* right, int frames) {
      if (buffer_l_.empty())
        return;
      // Stays running for a moment after it is switched off, so the tail fades instead of clicking.
      float target_mix = s_.on ? std::clamp(s_.mix, 0.0f, 1.0f) : 0.0f;
      if (!s_.on && mix_smooth_ < 1e-4f && activeGrains() == 0) {
        mix_smooth_ = 0.0f;
        writeOnly(left, right, frames);
        return;
      }
      const int length = static_cast<int>(buffer_l_.size());
      const float mix_step = 1.0f / std::max(1.0f, static_cast<float>(sample_rate_) * 0.02f);
      const float space = std::clamp(s_.space, 0.0f, 1.0f);
      const float feedback = s_.freeze ? 0.0f : std::clamp(s_.feedback, 0.0f, 0.9f);
      const double interval = grainInterval();
      const double size = std::clamp(static_cast<double>(s_.size_ms), 10.0, 500.0) * 0.001 * sample_rate_;
      // Overlapping grains add up: keep the level steady whatever the density and size.
      float overlap = static_cast<float>(size / std::max(interval, 1.0));
      float norm = 1.0f / std::sqrt(std::max(1.0f, overlap));

      for (int i = 0; i < frames; ++i) {
        float in_l = left[i], in_r = right[i];

        if (s_.on) {
          until_next_grain_ -= 1.0;
          if (until_next_grain_ <= 0.0) {
            startGrain(size, length);
            double jitter = s_.sync == 0 ? (random01() * 2.0 - 1.0) * s_.scatter * 0.5 : 0.0;
            until_next_grain_ += interval * (1.0 + jitter);
            if (until_next_grain_ < 1.0) until_next_grain_ = 1.0;
          }
        }

        float out_l = 0.0f, out_r = 0.0f;
        for (Grain& g : grains_) {
          if (!g.active) continue;
          float w = window_[std::min(kWindowSize - 1, static_cast<int>(g.age * g.window_step))];
          double p = g.pos;
          int i0 = static_cast<int>(std::floor(p));
          float frac = static_cast<float>(p - i0);
          i0 %= length; if (i0 < 0) i0 += length;
          int i1 = i0 + 1; if (i1 >= length) i1 = 0;
          float sl = buffer_l_[i0] + (buffer_l_[i1] - buffer_l_[i0]) * frac;
          float sr = buffer_r_[i0] + (buffer_r_[i1] - buffer_r_[i0]) * frac;
          out_l += (sl * g.gain_ll + sr * g.gain_rl) * w;
          out_r += (sr * g.gain_rr + sl * g.gain_lr) * w;
          g.pos += g.step;
          if (g.pos >= length) g.pos -= length;
          if (g.pos < 0.0) g.pos += length;
          if (++g.age >= g.length) g.active = false;
        }
        out_l *= norm;
        out_r *= norm;

        // Space: a short allpass diffuser blended in.
        if (space > 0.001f) {
          float dl = out_l, dr = out_r;
          for (int k = 0; k < kNumAllpass; k += 2) {
            dl = allpass_[k].process(dl, 0.5f + 0.2f * space);
            dr = allpass_[k + 1].process(dr, 0.5f + 0.2f * space);
          }
          float cross = 0.25f * space;
          float l2 = dl + (dr - dl) * cross, r2 = dr + (dl - dr) * cross;
          out_l += (l2 - out_l) * space;
          out_r += (r2 - out_r) * space;
        }

        // Write the input (plus feedback) unless frozen.
        if (!s_.freeze) {
          float wl = in_l + std::tanh(out_l * feedback);
          float wr = in_r + std::tanh(out_r * feedback);
          buffer_l_[write_] = wl;
          buffer_r_[write_] = wr;
          if (++write_ >= length) write_ = 0;
        }

        if (mix_smooth_ < target_mix) mix_smooth_ = std::min(target_mix, mix_smooth_ + mix_step);
        else if (mix_smooth_ > target_mix) mix_smooth_ = std::max(target_mix, mix_smooth_ - mix_step);
        // Equal-power crossfade between dry and grains.
        float dry = std::cos(mix_smooth_ * 1.5707963f), wet = std::sin(mix_smooth_ * 1.5707963f);
        left[i] = in_l * dry + out_l * wet;
        right[i] = in_r * dry + out_r * wet;
      }
    }

  private:
    struct Grain {
      bool active = false;
      double pos = 0.0, step = 1.0;
      int age = 0, length = 1;
      float window_step = 1.0f;
      float gain_ll = 1.0f, gain_rl = 0.0f, gain_rr = 1.0f, gain_lr = 0.0f;
    };
    static constexpr int kNumAllpass = 6;
    struct Allpass {
      std::vector<float> buffer;
      int pos = 0;
      float process(float x, float g) {
        float d = buffer[pos];
        float y = -g * x + d;
        buffer[pos] = x + g * y;
        if (++pos >= static_cast<int>(buffer.size())) pos = 0;
        return y;
      }
    };

    double grainInterval() const {
      if (s_.sync > 0) {
        static const double kBeats[kNumSyncRates] = { 0.0, 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
        return kBeats[std::clamp(s_.sync, 1, kNumSyncRates - 1)] * 60.0 / bpm_ * sample_rate_;
      }
      return sample_rate_ / std::clamp(static_cast<double>(s_.density), 1.0, 80.0);
    }

    double random01() {
      rng_ = rng_ * 1664525u + 1013904223u;
      return (rng_ >> 8) / 16777216.0;
    }

    void writeOnly(const float* left, const float* right, int frames) {
      // Off: keep the buffer filled so switching on starts with sound.
      if (s_.freeze) return;
      const int length = static_cast<int>(buffer_l_.size());
      for (int i = 0; i < frames; ++i) {
        buffer_l_[write_] = left[i];
        buffer_r_[write_] = right[i];
        if (++write_ >= length) write_ = 0;
      }
    }

    void startGrain(double size, int length) {
      Grain* g = nullptr;
      for (Grain& c : grains_)
        if (!c.active) { g = &c; break; }
      if (!g) return;

      double semis = s_.pitch + (random01() * 2.0 - 1.0) * s_.pitch_rand;
      double ratio = std::pow(2.0, semis / 12.0);
      bool reverse = random01() < s_.reverse;
      double step = reverse ? -ratio : ratio;

      // A fast grain reads more of the buffer: shorten it so it always fits.
      double span = std::max(1.0, ratio);
      double glen = std::min(size, (length * 0.45) / span);
      // Start far enough back that a forward grain never overtakes the write head.
      double back = std::clamp(static_cast<double>(s_.position_ms), 0.0, 1000.0) * 0.001 * sample_rate_;
      back += random01() * std::clamp(s_.scatter, 0.0f, 1.0f) * 0.4 * sample_rate_;
      if (!reverse) back += glen * (ratio - 1.0);
      double max_back = static_cast<double>(length) - glen * span - 8.0;
      back = std::clamp(back + 4.0, 4.0, std::max(4.0, max_back));

      g->pos = write_ - back;
      while (g->pos < 0.0) g->pos += length;
      g->step = step;
      g->length = std::max(16, static_cast<int>(glen));
      g->age = 0;
      g->window_step = static_cast<float>(kWindowSize - 1) / g->length;

      // Pan: each grain sits somewhere in the stereo field (balance law, never louder than 1).
      float pan = static_cast<float>((random01() * 2.0 - 1.0) * std::clamp(s_.spread, 0.0f, 1.0f));
      g->gain_ll = std::min(1.0f, 1.0f - pan);
      g->gain_rr = std::min(1.0f, 1.0f + pan);
      g->gain_lr = 0.0f;
      g->gain_rl = 0.0f;
      g->active = true;
    }

    Settings s_;
    double sample_rate_ = 44100.0;
    double bpm_ = 120.0;
    std::vector<float> buffer_l_, buffer_r_;
    int write_ = 0;
    std::array<Grain, kMaxGrains> grains_ {};
    std::array<float, kWindowSize> window_ {};
    std::array<Allpass, kNumAllpass> allpass_ {};
    double until_next_grain_ = 0.0;
    float fb_l_ = 0.0f, fb_r_ = 0.0f;
    float wet_gain_ = 0.0f;
    float mix_smooth_ = 0.0f;
    uint32_t rng_ = 0x9e3779b9u;
};
