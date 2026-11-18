// Arp.h - Warpline's arpeggiator: turns held notes into a tempo-synced pattern for the engine.
//
// Part of Warpline. GPLv3 (see LICENSE).
//
// Everything here runs on the audio thread and never allocates after construction.
// The plugin feeds it the notes you play (noteOn / noteOff), asks it how many samples until its next
// event (samplesToNextEvent), lets the engine render up to there, then calls fire() which sends note
// ons and offs to the engine through a callback.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

class Arp {
  public:
    enum Mode { kUp = 0, kDown, kUpDown, kDownUp, kRandom, kAsPlayed, kChord, kNumModes };
    enum Step { kStepOff = 0, kStepOn, kStepAccent, kStepTie, kNumStepStates };
    static constexpr int kMaxSteps = 16;
    static constexpr int kNumRates = 11;
    static constexpr int kMaxHeld = 16;
    static constexpr int kMaxSounding = 64;

    // Rate choices, in quarter notes per step (index = parameter choice).
    static double beatsPerStep(int rate) {
      static const double kBeats[kNumRates] = {
        1.0, 1.5, 2.0 / 3.0,           // 1/4, 1/4 dotted, 1/4 triplet
        0.5, 0.75, 1.0 / 3.0,          // 1/8, 1/8 D, 1/8 T
        0.25, 0.375, 1.0 / 6.0,        // 1/16, 1/16 D, 1/16 T
        0.125, 1.0 / 12.0,             // 1/32, 1/32 T
      };
      return kBeats[std::clamp(rate, 0, kNumRates - 1)];
    }

    struct Settings {
      bool on = false;
      int mode = kUp;
      int rate = 6;          // 1/16
      int octaves = 1;       // 1..4
      float gate = 0.5f;     // 0.05..1 of a step
      float swing = 0.0f;    // 0..0.75: off-beat steps are pushed this much of half a step later
      bool latch = false;
      int length = 16;       // 1..16 steps
      float accent = 0.5f;   // how much an Accent step raises velocity (0..1 of the headroom)
      std::array<int, kMaxSteps> steps {};
    };

    template <typename NoteFn>
    void setSettings(const Settings& s, NoteFn&& send) {
      if (s_.on && !s.on)
        stop(send);
      if (s_.latch && !s.latch && physical_ == 0) {   // latch released with no keys down: let go
        held_count_ = 0;
        stop(send);
      }
      s_ = s;
      s_.octaves = std::clamp(s_.octaves, 1, 4);
      s_.length = std::clamp(s_.length, 1, kMaxSteps);
    }
    const Settings& settings() const { return s_; }

    void setSampleRate(double sr) { sample_rate_ = sr > 0.0 ? sr : 44100.0; }

    // Called at the start of every block. When the MPC's transport runs, the step grid follows its
    // song position so the arp lines up with the sequence; otherwise it runs freely from the first note.
    void beginBlock(double bpm, bool playing, double ppq) {
      bpm_ = bpm > 1.0 ? bpm : 120.0;
      step_len_ = beatsPerStep(s_.rate) * 60.0 / bpm_ * sample_rate_;
      if (playing && running_) {
        double step_beats = beatsPerStep(s_.rate);
        double pos = ppq / step_beats;
        double whole = std::floor(pos);
        double next = pos - whole < 1e-7 ? whole : whole + 1.0;   // a boundary exactly now counts as next
        double to_boundary = (next - pos) * step_len_;
        double drift = to_boundary - grid_left_;
        drift -= std::round(drift / step_len_) * step_len_;
        // Resync only when the free-running clock has drifted (avoids double steps at block edges).
        if (!synced_ || std::fabs(drift) > 32.0) {
          grid_left_ = to_boundary;
          next_step_index_ = static_cast<int64_t>(next);
        }
        synced_ = true;
      }
      else {
        synced_ = false;
      }
    }

    // A note you played. Returns true when the arp took it (it is on), false = pass it to the engine.
    template <typename NoteFn>
    bool noteOn(int note, float velocity, NoteFn&& send) {
      if (!s_.on)
        return false;
      if (s_.latch && physical_ == 0)
        held_count_ = 0;   // a new chord replaces the latched one
      physical_++;
      addHeld(note, velocity);
      if (!running_) {     // first note: start on the next sample (free run) or the grid (transport)
        running_ = true;
        if (!synced_) {
          grid_left_ = 0.0;
          next_step_index_ = 0;
        }
        note_index_ = -1;
        (void)send;
      }
      return true;
    }

    template <typename NoteFn>
    bool noteOff(int note, NoteFn&& send) {
      if (!s_.on)
        return false;
      physical_ = std::max(0, physical_ - 1);
      if (!s_.latch)
        removeHeld(note);
      if (held_count_ == 0)
        stop(send);
      return true;
    }

    // Samples (from now) until the arp's next note on or off, or a large number when idle.
    int samplesToNextEvent() const {
      double next = 1e12;
      if (running_ && held_count_ > 0)
        next = std::min(next, stepStartLeft());
      for (int i = 0; i < sounding_count_; ++i)
        next = std::min(next, sounding_[i].left);
      if (next >= 1e9)
        return 1 << 30;
      return std::max(0, static_cast<int>(std::ceil(next - 1e-6)));
    }

    // Moves the clock on by n samples (after the engine rendered them).
    void advance(int n) {
      if (running_)
        grid_left_ -= n;
      for (int i = 0; i < sounding_count_; ++i)
        sounding_[i].left -= n;
    }

    // Sends everything due now.
    template <typename NoteFn>
    void fire(NoteFn&& send) {
      for (int i = 0; i < sounding_count_;) {
        if (sounding_[i].left <= 0.5) {
          send(sounding_[i].note, 0.0f, false);
          sounding_[i] = sounding_[--sounding_count_];
        }
        else {
          ++i;
        }
      }
      while (running_ && held_count_ > 0 && stepStartLeft() <= 0.5) {
        playStep(send);
        grid_left_ += step_len_;
        next_step_index_++;
      }
    }

    template <typename NoteFn>
    void stop(NoteFn&& send) {
      for (int i = 0; i < sounding_count_; ++i)
        send(sounding_[i].note, 0.0f, false);
      sounding_count_ = 0;
      running_ = false;
      current_step_ = -1;
      note_index_ = -1;
      if (!s_.latch) {
        held_count_ = 0;
        physical_ = 0;
      }
    }

    template <typename NoteFn>
    void reset(NoteFn&& send) {
      for (int i = 0; i < sounding_count_; ++i)
        send(sounding_[i].note, 0.0f, false);
      sounding_count_ = 0;
      running_ = false;
      held_count_ = 0;
      physical_ = 0;
      current_step_ = -1;
    }

    // The step that played last (0..15), or -1 when stopped. For the screen's play marker.
    int currentStep() const { return current_step_; }
    bool running() const { return running_; }

  private:
    struct Held { int note; float velocity; };
    struct Sounding { int note; double left; };

    double stepStartLeft() const {
      double left = grid_left_;
      if (next_step_index_ % 2 == 1)
        left += s_.swing * 0.5 * step_len_;
      return left;
    }

    void addHeld(int note, float velocity) {
      for (int i = 0; i < held_count_; ++i)
        if (held_[i].note == note) {
          held_[i].velocity = velocity;
          return;
        }
      if (held_count_ < kMaxHeld)
        held_[held_count_++] = { note, velocity };
    }

    void removeHeld(int note) {
      for (int i = 0; i < held_count_; ++i)
        if (held_[i].note == note) {
          for (int k = i; k + 1 < held_count_; ++k)
            held_[k] = held_[k + 1];
          held_count_--;
          return;
        }
    }

    // The notes the pattern walks through, octaves included (as played or sorted).
    int buildSequence(std::array<Held, kMaxHeld * 4 * 2>& seq) const {
      std::array<Held, kMaxHeld> base {};
      int n = held_count_;
      for (int i = 0; i < n; ++i)
        base[i] = held_[i];
      if (s_.mode != kAsPlayed)
        std::sort(base.begin(), base.begin() + n, [](const Held& a, const Held& b) { return a.note < b.note; });
      int count = 0;
      for (int o = 0; o < s_.octaves; ++o)
        for (int i = 0; i < n; ++i)
          if (base[i].note + 12 * o <= 127)
            seq[count++] = { base[i].note + 12 * o, base[i].velocity };
      if (s_.mode == kDown)
        std::reverse(seq.begin(), seq.begin() + count);
      else if ((s_.mode == kUpDown || s_.mode == kDownUp) && count > 2) {
        int up = count;
        for (int i = up - 2; i >= 1; --i)
          seq[count++] = seq[i];
        if (s_.mode == kDownUp)
          std::rotate(seq.begin(), seq.begin() + up - 1, seq.begin() + count);
      }
      else if (s_.mode == kDownUp && count == 2) {
        std::swap(seq[0], seq[1]);
      }
      return count;
    }

    template <typename NoteFn>
    void playStep(NoteFn&& send) {
      int step = static_cast<int>(next_step_index_ % s_.length);
      if (step < 0) step += s_.length;
      current_step_ = step;
      int state = s_.steps[step];
      if (state == kStepOff || state == kStepTie)
        return;   // a rest, or the earlier note carries on (its length already covers this step)

      // Length: the gate, stretched over the Tie steps that follow.
      int ties = 0;
      for (int k = 1; k < s_.length; ++k) {
        if (s_.steps[(step + k) % s_.length] != kStepTie)
          break;
        ties++;
      }
      double length = (ties + s_.gate) * step_len_;
      length = std::max(length, 32.0);

      auto velocityFor = [&](float v) {
        if (state == kStepAccent)
          v = v + s_.accent * (1.0f - v);
        return std::clamp(v, 0.05f, 1.0f);
      };

      std::array<Held, kMaxHeld * 4 * 2> seq {};
      int count = buildSequence(seq);
      if (count <= 0)
        return;

      if (s_.mode == kChord) {
        int octave = (++note_index_) % s_.octaves;
        for (int i = 0; i < held_count_; ++i) {
          int note = held_[i].note + 12 * octave;
          if (note <= 127)
            start(note, velocityFor(held_[i].velocity), length, send);
        }
        return;
      }
      int index;
      if (s_.mode == kRandom) {
        rng_ = rng_ * 1664525u + 1013904223u;
        index = static_cast<int>((rng_ >> 8) % static_cast<uint32_t>(count));
      }
      else {
        note_index_ = (note_index_ + 1) % count;
        index = note_index_;
      }
      start(seq[index].note, velocityFor(seq[index].velocity), length, send);
    }

    template <typename NoteFn>
    void start(int note, float velocity, double length, NoteFn&& send) {
      for (int i = 0; i < sounding_count_; ++i)   // the same note again: end it first
        if (sounding_[i].note == note) {
          send(note, 0.0f, false);
          sounding_[i] = sounding_[--sounding_count_];
          break;
        }
      if (sounding_count_ >= kMaxSounding)
        return;
      send(note, velocity, true);
      sounding_[sounding_count_++] = { note, length };
    }

    Settings s_;
    double sample_rate_ = 44100.0;
    double bpm_ = 120.0;
    double step_len_ = 5512.5;
    double grid_left_ = 0.0;          // samples until the next step boundary (before swing)
    int64_t next_step_index_ = 0;
    bool running_ = false;
    bool synced_ = false;
    int note_index_ = -1;
    int current_step_ = -1;
    uint32_t rng_ = 0x1234567u;

    std::array<Held, kMaxHeld> held_ {};
    int held_count_ = 0;
    int physical_ = 0;                // keys physically down (latch keeps notes after they go up)
    std::array<Sounding, kMaxSounding> sounding_ {};
    int sounding_count_ = 0;
};
