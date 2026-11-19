// GRAINZ's texture modes: Emilie Gillet's open-source granular processor (MIT, third_party/mutable), wrapped for
// Warpline. It runs at its native 32 kHz on 32-frame blocks, so this resamples the synth's output down to it and
// the result back up. Only the wet signal comes out: Warpline mixes dry and wet at the host rate, so the dry sound
// keeps its full bandwidth.
//
// The processor's headers need TEST defined (portable code instead of Cortex-M4 assembly), so they stay inside
// GrainzClouds.cpp; this header has none of them.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

class GrainzClouds {
 public:
  enum Mode { kParticle = 0, kStretch, kLoop, kSpectral, kNumModes };

  struct Settings {
    int mode = kParticle;
    float position = 0.0f;   // all 0..1 unless noted
    float size = 0.5f;
    float pitch = 0.0f;      // semitones
    float density = 0.5f;
    float texture = 0.5f;
    float spread = 0.5f;
    float feedback = 0.0f;
    float reverb = 0.3f;
    bool freeze = false;
  };

  GrainzClouds();
  ~GrainzClouds();

  void prepare(double sample_rate);   // allocates; not on the audio thread
  void reset();
  void setSettings(const Settings& s) { settings_ = s; }
  // A trigger (sync grid): in particle mode it seeds a grain.
  void trigger() { trigger_ = true; }
  // In: the synth's output. Out: the wet signal only, same length.
  void process(const float* in_l, const float* in_r, float* wet_l, float* wet_r, int frames);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Settings settings_;
  bool trigger_ = false;
};
