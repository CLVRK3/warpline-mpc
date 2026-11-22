// Gen.h - Warpline's generative mod sources ("Gen 1", "Gen 2"): tempo-clocked random values with a
// déjà vu memory, in the spirit of Eurorack random samplers (written for Warpline, no code from elsewhere).
//
// Part of Warpline. GPLv3 (see LICENSE).
//
// On every clock tick each output picks a new random value, shaped by SPREAD (narrow around the
// centre .. uniform .. pushed to the extremes) and BIAS (leans low or high). DÉJÀ VU decides how
// often it replays its own past instead: 0 = always new, 50% = a locked loop of LENGTH ticks,
// 100% = the same values, but in shuffled order. STEPS goes from smooth glides (0) to hard steps.
// Audio thread only, no allocation.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

class Gen {
  public:
    static constexpr int kNumOutputs = 2;
    static constexpr int kMaxLength = 16;

    struct Settings {
      int rate = 6;          // Arp::beatsPerStep index (1/16)
      float spread = 0.5f;   // 0..1
      float bias = 0.5f;     // 0..1
      float steps = 1.0f;    // 0 smooth .. 1 stepped
      float deja_vu = 0.0f;  // 0..1
      int length = 8;        // 1..16
    };

    void setSettings(const Settings& s) {
      s_ = s;
      s_.length = std::clamp(s_.length, 1, kMaxLength);
    }
    const Settings& settings() const { return s_; }
    void setSampleRate(double sr) { sample_rate_ = sr > 0.0 ? sr : 44100.0; }

    // Moves the generator on by n samples. beats_per_tick from the rate; when the transport plays the
    // ticks follow its position (ppq at the start of these samples), otherwise they run freely.
    void advance(int n, double bpm, bool playing, double ppq, double beats_per_tick) {
      double tick_len = beats_per_tick * 60.0 / (bpm > 1.0 ? bpm : 120.0) * sample_rate_;
      if (playing) {
        int64_t tick = static_cast<int64_t>(std::floor(ppq / beats_per_tick + 1e-7));
        if (tick != last_tick_) {
          last_tick_ = tick;
          clock(tick_len);
        }
        free_left_ = tick_len;
      }
      else {
        last_tick_ = INT64_MIN;
        free_left_ -= n;
        if (free_left_ <= 0.0) {
          free_left_ += tick_len;
          if (free_left_ <= 0.0) free_left_ = tick_len;
          clock(tick_len);
        }
      }
      for (int o = 0; o < kNumOutputs; ++o) {
        Out& out = out_[o];
        if (out.glide_left > 0.0) {
          double step = std::min(static_cast<double>(n), out.glide_left);
          out.value += (out.target - out.value) * static_cast<float>(step / out.glide_left);
          out.glide_left -= step;
        }
        else {
          out.value = out.target;
        }
      }
    }

    float value(int o) const { return out_[std::clamp(o, 0, kNumOutputs - 1)].value; }

    // New values start from where the outputs are now, on the next tick.
    void reset() {
      for (Out& o : out_) {
        o.value = o.target = 0.5f;
        o.glide_left = 0.0;
        o.history.fill(0.5f);
        o.filled = 0;
      }
      pos_ = 0;
      free_left_ = 0.0;
      last_tick_ = INT64_MIN;
    }

  private:
    struct Out {
      float value = 0.5f, target = 0.5f;
      double glide_left = 0.0;
      std::array<float, kMaxLength> history {};
      int filled = 0;
    };

    float uniform() {
      rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
      return static_cast<float>(rng_ >> 8) / 16777216.0f;
    }

    float shape(float u) const {
      float c = u - 0.5f;
      float v;
      if (s_.spread < 0.5f)            // narrow: towards the centre
        v = 0.5f + c * (s_.spread * 2.0f);
      else {                           // wide: towards the ends
        float a = 1.0f / (1.0f + (s_.spread - 0.5f) * 8.0f);
        float m = std::pow(std::fabs(c) * 2.0f, a) * 0.5f;
        v = 0.5f + (c < 0.0f ? -m : m);
      }
      float gamma = std::pow(2.0f, (0.5f - s_.bias) * 4.0f);   // bias 0.5 = straight
      return std::clamp(std::pow(std::clamp(v, 0.0f, 1.0f), gamma), 0.0f, 1.0f);
    }

    void clock(double tick_len) {
      int len = s_.length;
      float dv = s_.deja_vu;
      // déjà vu: below half, the chance of replaying; above half, the loop is locked and the extra
      // amount is the chance of jumping to another place in it.
      bool replay = dv >= 0.5f || uniform() < dv * 2.0f;
      int read = pos_;
      if (dv > 0.5f && uniform() < (dv - 0.5f) * 2.0f)
        read = static_cast<int>(uniform() * len) % len;
      for (int o = 0; o < kNumOutputs; ++o) {
        Out& out = out_[o];
        float u;
        if (replay && out.filled >= len)
          u = out.history[read];
        else {
          u = uniform();
          out.history[pos_] = u;
          out.filled = std::min(kMaxLength, out.filled + 1);
        }
        out.target = shape(u);
        out.glide_left = (1.0f - s_.steps) * tick_len;
      }
      pos_ = (pos_ + 1) % len;
    }

    Settings s_;
    double sample_rate_ = 44100.0;
    double free_left_ = 0.0;
    int64_t last_tick_ = INT64_MIN;
    int pos_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    std::array<Out, kNumOutputs> out_ {};
};
