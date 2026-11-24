// make_factory.cpp - builds Warpline's factory .vital patches with the engine itself.
//
// Part of Warpline (GPLv3). Not part of the plugin binary (it lives outside src/).
// Wavetables come from atsushieno/open-vital-resources (CC0 Public Domain dedication; Kimura Taro free
// wavetables, converted to .vitaltable). Every patch also opens in desktop Vital.
//
// Build (from the plugin folder):  see tools/README.md
// Run:  make_factory <open-vital-resources folder> <output Content/Patches folder>

#include "VitalCore.h"
#include "synth_parameters.h"
#include "json/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Mod { std::string src, dst; float amount; bool bipolar; };
struct PatchDef {
  std::string name, style;
  std::vector<std::string> tables;              // up to 3 .vitaltable paths (relative), "" = keep default saw
  std::vector<std::pair<std::string, float>> controls;
  std::vector<Mod> mods;
  std::vector<int> lfo_shapes;                  // per LFO: 0 tri, 1 sine, 2 square, 3 saw up, 4 saw down
};

static std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------------------------------------
// The expanded factory set. Building blocks first, then 84 patches across 11 styles.
// Times: envelope values are Vital's (seconds = value^4: 0.3 = 8 ms, 0.5 = 60 ms, 0.7 = 0.24 s,
// 0.9 = 0.66 s, 1.1 = 1.5 s, 1.3 = 2.9 s). LFO tempo: 6 = 1/1, 7 = 1/2, 8 = 1/4, 9 = 1/8, 10 = 1/16.
// Filter models: 0 Analog, 1 Dirty, 2 Ladder, 3 Digital, 4 Diode, 5 Formant, 6 Comb, 7 Phaser.
// Osc warp (distortion_type): 1 Sync, 2 Formant, 3 Quantize, 4 Bend, 5 Squeeze, 6 Pulse Width, 7 FM <- other osc.
// Spectral morph: 1 Vocode, 2 Formant Scale, 3 Harmonic Stretch, 4 Inharmonic, 5 Smear, 7 Low Pass, 9 Phase Disperse.
// ---------------------------------------------------------------------------------------------
using C = std::vector<std::pair<std::string, float>>;
using M = std::vector<Mod>;

static C cat(std::initializer_list<C> parts) {
  C out;
  for (const C& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// Envelope shapes (amp = env_1, mod = env_2).
static const C kEnvPluck = { { "env_1_attack", 0.0f }, { "env_1_decay", 0.85f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.75f },
                             { "env_2_attack", 0.0f }, { "env_2_decay", 0.6f }, { "env_2_sustain", 0.0f } };
static const C kEnvKeys = { { "env_1_attack", 0.15f }, { "env_1_decay", 1.15f }, { "env_1_sustain", 0.35f }, { "env_1_release", 0.85f },
                            { "env_2_attack", 0.0f }, { "env_2_decay", 0.85f }, { "env_2_sustain", 0.15f } };
static const C kEnvPad = { { "env_1_attack", 1.05f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.3f },
                           { "env_2_attack", 1.2f }, { "env_2_decay", 1.4f }, { "env_2_sustain", 0.6f } };
static const C kEnvLead = { { "env_1_attack", 0.25f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 0.9f }, { "env_1_release", 0.6f },
                            { "env_2_attack", 0.0f }, { "env_2_decay", 0.9f }, { "env_2_sustain", 0.3f } };
static const C kEnvBass = { { "env_1_attack", 0.1f }, { "env_1_decay", 0.95f }, { "env_1_sustain", 0.85f }, { "env_1_release", 0.4f },
                            { "env_2_attack", 0.0f }, { "env_2_decay", 0.65f }, { "env_2_sustain", 0.0f } };
static const C kEnvSwell = { { "env_1_attack", 1.2f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.0f },
                             { "env_2_attack", 1.3f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 1.0f } };
static const C kEnvPerc = { { "env_1_attack", 0.0f }, { "env_1_decay", 0.62f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.55f },
                            { "env_2_attack", 0.0f }, { "env_2_decay", 0.45f }, { "env_2_sustain", 0.0f } };

// Effects.
static const C kDry = { { "reverb_on", 0 } };
static const C kRoom = { { "reverb_dry_wet", 0.16f }, { "reverb_size", 0.45f }, { "reverb_decay_time", -1.0f } };
static const C kHall = { { "reverb_dry_wet", 0.38f }, { "reverb_size", 0.85f }, { "reverb_decay_time", 1.2f } };
static const C kShimmer = { { "reverb_dry_wet", 0.5f }, { "reverb_size", 1.0f }, { "reverb_decay_time", 2.0f } };
static const C kDelay8 = { { "delay_on", 1 }, { "delay_tempo", 9 }, { "delay_feedback", 0.35f }, { "delay_dry_wet", 0.2f } };
static const C kDelayDot = { { "delay_on", 1 }, { "delay_sync", 2 }, { "delay_tempo", 9 }, { "delay_feedback", 0.45f }, { "delay_dry_wet", 0.24f } };
static const C kDelay16 = { { "delay_on", 1 }, { "delay_tempo", 10 }, { "delay_feedback", 0.3f }, { "delay_dry_wet", 0.16f } };
static const C kChorus = { { "chorus_on", 1 }, { "chorus_dry_wet", 0.35f } };
static const C kPhaser = { { "phaser_on", 1 }, { "phaser_dry_wet", 0.5f } };
static const C kFlanger = { { "flanger_on", 1 }, { "flanger_dry_wet", 0.4f } };
static const C kComp = { { "compressor_on", 1 } };
static C dist(int type, float drive) { return { { "distortion_type", static_cast<float>(type) }, { "distortion_drive", drive } }; }

// Voicing.
static const C kMono = { { "polyphony", 1 }, { "legato", 1 }, { "portamento_time", -5.0f } };
static const C kMonoGlide = { { "polyphony", 1 }, { "legato", 1 }, { "portamento_time", -3.0f } };
static C uni(int osc, int voices, float detune) {
  std::string o = "osc_" + std::to_string(osc) + "_";
  return { { o + "unison_voices", static_cast<float>(voices) }, { o + "unison_detune", detune } };
}
static C osc(int n, float level, float transpose = 0.0f, float frame = 0.0f) {
  std::string o = "osc_" + std::to_string(n) + "_";
  C c = { { o + "on", 1 }, { o + "level", level }, { o + "transpose", transpose }, { o + "wave_frame", frame } };
  if (n > 1) c.push_back({ o + "destination", 0 });   // through Filter 1 with osc 1
  return c;
}
static C warp(int n, int type, float amount) {
  std::string o = "osc_" + std::to_string(n) + "_";
  return { { o + "distortion_type", static_cast<float>(type) }, { o + "distortion_amount", amount } };
}
static C morph(int n, int type, float amount) {
  std::string o = "osc_" + std::to_string(n) + "_";
  return { { o + "spectral_morph_type", static_cast<float>(type) }, { o + "spectral_morph_amount", amount } };
}
static C filt(int model, int style, float cutoff, float reso, float drive = 0.0f, float keytrack = 0.0f) {
  return { { "filter_1_on", 1 }, { "filter_1_model", static_cast<float>(model) }, { "filter_1_style", static_cast<float>(style) },
           { "filter_1_cutoff", cutoff }, { "filter_1_resonance", reso }, { "filter_1_drive", drive },
           { "filter_1_keytrack", keytrack } };
}
static C lfoTempo(int lfo, int tempo, int sync = 1) {
  std::string l = "lfo_" + std::to_string(lfo) + "_";
  return { { l + "sync", static_cast<float>(sync) }, { l + "tempo", static_cast<float>(tempo) } };
}
static C lfoHz(int lfo, float log2_hz) {
  std::string l = "lfo_" + std::to_string(lfo) + "_";
  return { { l + "sync", 0 }, { l + "frequency", log2_hz } };
}
static const C kNoise = { { "sample_on", 1 }, { "sample_level", 0.35f }, { "sample_destination", 0 } };

static std::string T(const std::string& dir, int index) { return dir + "/#" + std::to_string(index); }

static void addMore(std::vector<PatchDef>& d) {
  // ---------------- BASS (13) ----------------
  d.push_back({ "Reese Pressure", "Bass", { T("AnalogFour", 2) },
    cat({ osc(1, 0.75f), uni(1, 6, 2.6f), filt(2, 1, 70, 0.3f, 4), kEnvBass, kMono, kDry, dist(0, 2), kComp, lfoHz(1, -2.5f) }),
    { { "lfo_1", "osc_1_unison_detune", 0.15f, false } } });
  d.push_back({ "Rubber Sub", "Bass", { T("Mod Sine", 2) },
    cat({ osc(1, 0.8f), filt(0, 1, 50, 0.4f), kEnvBass, kMono, kDry, kComp }),
    { { "env_2", "filter_1_cutoff", 0.25f, false }, { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Neuro Grind", "Bass", { T("Hardcore FM", 3) },
    cat({ osc(1, 0.7f), uni(1, 3, 1.5f), warp(1, 5, 0.6f), filt(1, 0, 90, 0.45f, 8), kEnvBass, kMono, kDry, dist(2, 8), lfoTempo(1, 10) }),
    { { "lfo_1", "osc_1_wave_frame", 0.7f, false }, { "lfo_1", "filter_1_cutoff", 0.18f, false } }, { 3 } });
  d.push_back({ "Saw Stack Bass", "Bass", {},
    cat({ osc(1, 0.6f), uni(1, 4, 1.8f), osc(2, 0.5f, -12), filt(0, 1, 64, 0.2f), kEnvBass, kMono, kDry, kComp }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Diode Bite", "Bass", { T("PD Square", 1) },
    cat({ osc(1, 0.7f), filt(4, 0, 58, 0.6f, 6), kEnvBass, kMonoGlide, kDry, kDelay16 }),
    { { "env_2", "filter_1_cutoff", 0.38f, false }, { "velocity", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "Wobble Classic", "Bass", { T("PPG Wave", 5) },
    cat({ osc(1, 0.7f), uni(1, 2, 1.0f), osc(2, 0.45f, -12, 0), filt(1, 1, 60, 0.5f, 6), kEnvBass, kMono, kDry, dist(0, 5), lfoTempo(1, 9) }),
    { { "lfo_1", "filter_1_cutoff", 0.35f, false } }, { 1 } });
  d.push_back({ "Plaits Thump", "Bass", { T("Mutable Instruments Plaits", 1) },
    cat({ osc(1, 0.75f), filt(0, 1, 72, 0.25f), kEnvPerc, kMono, kDry, kComp }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false }, { "env_2", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Sync Snarl", "Bass", { T("OSC Sync+PWM", 4) },
    cat({ osc(1, 0.7f), warp(1, 1, 0.4f), filt(2, 1, 80, 0.35f, 4), kEnvBass, kMono, kDry, dist(1, 3) }),
    { { "env_2", "osc_1_distortion_amount", 0.4f, false } } });
  d.push_back({ "Deep House Organ Bass", "Bass", { T("Additive Synth", 3) },
    cat({ osc(1, 0.7f), filt(0, 0, 92, 0.2f), kEnvBass, kMono, kRoom }),
    { { "velocity", "filter_1_cutoff", 0.12f, false } } });
  d.push_back({ "Formant Growler", "Bass", { T("FormantStep", 0) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.9f), filt(5, 0, 90, 0.5f), kEnvBass, kMono, kDry, dist(0, 6), lfoTempo(1, 8, 3) }),
    { { "lfo_1", "osc_1_wave_frame", 0.8f, false } }, { 4 } });
  d.push_back({ "808 Glide", "Bass", { T("Mod Sine", 0) },
    cat({ osc(1, 0.85f), { { "filter_1_on", 0 } }, { { "env_1_attack", 0.0f }, { "env_1_decay", 1.25f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.9f } },
          kMonoGlide, kDry, dist(0, 4) }),
    {} });
  d.push_back({ "Bit Bass", "Bass", { T("PPG Wave[4bit]", 7) },
    cat({ osc(1, 0.7f), filt(3, 1, 84, 0.3f), kEnvBass, kMono, kDry, dist(4, 0) }),
    { { "env_2", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Squelch 303", "Bass", { T("PD PWM", 2) },
    cat({ osc(1, 0.7f), filt(2, 1, 52, 0.8f, 3), kEnvBass, kMonoGlide, kDry, kDelay16, dist(0, 3) }),
    { { "env_2", "filter_1_cutoff", 0.45f, false }, { "velocity", "filter_1_resonance", 0.15f, false } } });

  // ---------------- LEAD (11) ----------------
  d.push_back({ "Hyper Saw", "Lead", {},
    cat({ osc(1, 0.55f), uni(1, 9, 4.0f), osc(2, 0.4f, 12), uni(2, 5, 3.0f), filt(0, 1, 118, 0.1f), kEnvLead, kDelayDot, kHall }),
    {} });
  d.push_back({ "Screamer", "Lead", { T("Folding", 2) },
    cat({ osc(1, 0.65f), uni(1, 2, 0.7f), filt(1, 0, 104, 0.4f, 8), kEnvLead, kMonoGlide, dist(2, 6), kDelay8, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false }, { "mod_wheel", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Square Hero", "Lead", { T("PD Square", 3) },
    cat({ osc(1, 0.65f), filt(0, 1, 100, 0.25f), kEnvLead, kMono, kDelay8, kRoom, lfoHz(1, 2.4f) }),
    { { "lfo_1", "osc_1_tune", 0.08f, true }, { "mod_wheel", "osc_1_wave_frame", 0.5f, false } }, { 1 } });
  d.push_back({ "OP Lead", "Lead", { T("TE OP-1", 5) },
    cat({ osc(1, 0.65f), uni(1, 2, 0.5f), filt(3, 1, 110, 0.2f), kEnvLead, kMonoGlide, kDelayDot, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Vocal Synth Lead", "Lead", { T("Vocorder", 1) },
    cat({ osc(1, 0.65f), filt(5, 0, 96, 0.4f), kEnvLead, kMono, kHall, kChorus, lfoHz(1, -0.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Sync Lead", "Lead", { T("Prophet VS Wave", 4) },
    cat({ osc(1, 0.6f), warp(1, 1, 0.3f), filt(0, 1, 108, 0.2f), kEnvLead, kMonoGlide, kDelay8, kRoom }),
    { { "env_2", "osc_1_distortion_amount", 0.5f, false }, { "mod_wheel", "osc_1_distortion_amount", 0.3f, false } } });
  d.push_back({ "Chebyshev Whistle", "Lead", { T("Chebyshev polynomials", 1) },
    cat({ osc(1, 0.6f), filt(0, 0, 120, 0.1f), kEnvLead, kMono, kShimmer, lfoHz(1, 2.6f) }),
    { { "lfo_1", "osc_1_tune", 0.06f, true }, { "velocity", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Buchla Bounce", "Lead", { T("Buchla 259e-Red", 3) },
    cat({ osc(1, 0.65f), filt(4, 0, 98, 0.35f), kEnvPluck, kDelayDot, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.7f, false } } });
  d.push_back({ "Modulo Lead", "Lead", { T("Modulo", 6) },
    cat({ osc(1, 0.6f), uni(1, 3, 1.0f), filt(3, 1, 106, 0.2f), kEnvLead, kMonoGlide, kDelay16, kRoom, lfoTempo(1, 8) }),
    { { "lfo_1", "osc_1_wave_frame", 0.35f, false } }, { 3 } });
  d.push_back({ "Soft Sine Lead", "Lead", { T("Mod Sine", 4) },
    cat({ osc(1, 0.7f), { { "filter_1_on", 0 } }, kEnvLead, kMonoGlide, kHall, kChorus, lfoHz(1, 2.3f) }),
    { { "lfo_1", "osc_1_tune", 0.05f, true }, { "mod_wheel", "osc_1_wave_frame", 0.6f, false } }, { 1 } });
  d.push_back({ "Fuzz Lead", "Lead", { T("Half Clip", 0) },
    cat({ osc(1, 0.6f), uni(1, 3, 1.2f), filt(1, 1, 96, 0.3f, 10), kEnvLead, kMono, dist(1, 12), kDelay8, kRoom }),
    { { "mod_wheel", "filter_1_cutoff", 0.25f, false } } });

  // ---------------- KEYS (10) ----------------
  d.push_back({ "EP Tines", "Keys", { T("FM", 2) },
    cat({ osc(1, 0.7f), filt(0, 0, 100, 0.1f), kEnvKeys, kChorus, kRoom, lfoHz(1, 2.2f) }),
    { { "env_2", "osc_1_wave_frame", 0.35f, false }, { "velocity", "osc_1_wave_frame", 0.3f, false }, { "lfo_1", "osc_1_level", 0.08f, true } } });
  d.push_back({ "Wurly Drive", "Keys", { T("PM[Simple]", 4) },
    cat({ osc(1, 0.7f), filt(1, 0, 96, 0.15f, 4), kEnvKeys, dist(0, 3), kPhaser, kRoom }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Organ Drawbars", "Keys", { T("Additive Synth", 3) },
    cat({ osc(1, 0.6f), osc(2, 0.35f, 12, 40), filt(0, 0, 120, 0.0f), { { "env_1_attack", 0.15f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.45f } },
          kChorus, kRoom, lfoHz(1, 2.6f) }),
    { { "lfo_1", "osc_1_level", 0.06f, true } } });
  d.push_back({ "Clav Funk", "Keys", { T("PD PWM", 4) },
    cat({ osc(1, 0.7f), filt(3, 2, 104, 0.3f), kEnvPerc, kPhaser, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.15f, false }, { "velocity", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "M1 House Piano", "Keys", { T("M1 Piano", 1) },
    cat({ osc(1, 0.75f), filt(0, 0, 112, 0.1f), kEnvKeys, kRoom, kComp }),
    { { "velocity", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Dream Rhodes", "Keys", { T("Phase", 3) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.6f), filt(0, 0, 98, 0.1f), kEnvKeys, kChorus, kHall, kDelayDot }),
    { { "env_2", "osc_1_wave_frame", 0.3f, false } } });
  d.push_back({ "Lo-Fi Keys", "Keys", { T("M1 Wavetable[Mini][2048-44.1khz-32bit]", 2) },
    cat({ osc(1, 0.7f), filt(0, 0, 90, 0.15f), kEnvKeys, dist(5, 0), { { "distortion_mix", 0.4f } }, kChorus, kRoom, lfoHz(1, 0.8f) }),
    { { "lfo_1", "osc_1_tune", 0.04f, true } } });
  d.push_back({ "Glass Harp", "Keys", { T("Korg Analog  Synth PhaseShift1", 6) },
    cat({ osc(1, 0.65f), filt(0, 0, 116, 0.1f), kEnvKeys, kShimmer, kDelay8 }),
    { { "velocity", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Digi Piano 84", "Keys", { T("Digitone", 3) },
    cat({ osc(1, 0.7f), filt(3, 0, 108, 0.1f), kEnvKeys, kChorus, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.45f, false } } });
  d.push_back({ "Stab Chord", "Keys", {},
    cat({ osc(1, 0.6f), uni(1, 5, 2.4f), filt(0, 1, 84, 0.25f), kEnvPerc, kDelayDot, kHall }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });

  // ---------------- PAD (12) ----------------
  d.push_back({ "Analog Warmth", "Pad", { T("AnalogFour", 5) },
    cat({ osc(1, 0.55f), uni(1, 5, 2.4f), osc(2, 0.35f, -12), filt(0, 1, 86, 0.15f), kEnvPad, kChorus, kHall, lfoHz(1, -2.0f) }),
    { { "lfo_1", "filter_1_cutoff", 0.06f, true } } });
  d.push_back({ "Spectral Smear", "Pad", { T("jo.Spectral Morph 1.0", 0) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.8f), morph(1, 5, 0.6f), filt(0, 1, 96, 0.1f), kEnvPad, kHall, lfoHz(1, -3.0f) }),
    { { "lfo_1", "osc_1_spectral_morph_amount", 0.4f, false }, { "lfo_1", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Granular Haze", "Pad", { T("Tone2Icarus Granular", 2) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(0, 1, 90, 0.1f), kEnvPad, kShimmer, kDelayDot, lfoHz(1, -2.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.8f, false } } });
  d.push_back({ "Choir of Glass", "Pad", { T("GMa Vowel", 0) },
    cat({ osc(1, 0.55f), uni(1, 6, 1.6f), filt(5, 0, 100, 0.3f), kEnvPad, kHall, kChorus, lfoHz(1, -2.2f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Prophet Vector", "Pad", { T("Prophet VS Wave", 8) },
    cat({ osc(1, 0.55f), uni(1, 3, 1.4f), osc(2, 0.4f, 7, 100), filt(0, 1, 92, 0.15f), kEnvPad, kChorus, kHall, lfoHz(1, -2.8f), lfoHz(2, -1.8f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.7f, false }, { "lfo_2", "osc_2_wave_frame", 0.6f, false } } });
  d.push_back({ "Phaser Strings", "Pad", { T("Alpha Juno SawPWM", 1) },
    cat({ osc(1, 0.55f), uni(1, 6, 2.2f), filt(0, 1, 98, 0.1f), kEnvPad, kPhaser, kHall }),
    { { "env_2", "filter_1_cutoff", 0.08f, false } } });
  d.push_back({ "Dark Matter", "Pad", { T("AmaizingNoises DarkSynth", 4) },
    cat({ osc(1, 0.6f), uni(1, 4, 1.4f), filt(2, 1, 88, 0.35f), kEnvPad, kHall, lfoHz(1, -3.5f) }),
    { { "lfo_1", "filter_1_cutoff", 0.15f, false }, { "lfo_1", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Warm Blanket", "Pad", { T("Mod Sine", 3) },
    cat({ osc(1, 0.6f), uni(1, 5, 1.8f), filt(0, 0, 88, 0.0f), kEnvPad, kChorus, kHall }),
    { { "env_2", "osc_1_wave_frame", 0.3f, false } } });
  d.push_back({ "Comb Shimmer", "Pad", { T("Bazille", 2) },
    cat({ osc(1, 0.5f), uni(1, 3, 1.0f), filt(6, 0, 96, 0.5f), kEnvPad, kShimmer, lfoHz(1, -2.0f) }),
    { { "lfo_1", "filter_1_cutoff", 0.08f, true } } });
  d.push_back({ "Glacier", "Pad", { T("Random Spectrum", 1) },
    cat({ osc(1, 0.5f), uni(1, 6, 2.6f), filt(0, 1, 94, 0.1f), kEnvSwell, kShimmer, kFlanger, lfoHz(1, -3.2f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.9f, false } } });
  d.push_back({ "Sunrise Sweep", "Pad", { T("Filter Sweep", 1) },
    cat({ osc(1, 0.55f), uni(1, 4, 2.0f), filt(0, 1, 104, 0.1f), kEnvSwell, kHall, kChorus }),
    { { "env_2", "osc_1_wave_frame", 1.0f, false } } });
  d.push_back({ "Harmonic Drift", "Pad", { T("ck.fourier 1.0", 1) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.4f), morph(1, 3, 0.5f), filt(0, 1, 98, 0.1f), kEnvPad, kHall, lfoHz(1, -2.6f) }),
    { { "lfo_1", "osc_1_spectral_morph_amount", 0.35f, true } } });

  // ---------------- PLUCK (8) ----------------
  d.push_back({ "Pizz Pluck", "Pluck", { T("Karplus Strong", 3) },
    cat({ osc(1, 0.75f), filt(0, 1, 100, 0.2f), kEnvPluck, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.15f, false }, { "velocity", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Future Pluck", "Pluck", {},
    cat({ osc(1, 0.6f), uni(1, 7, 3.0f), filt(0, 1, 70, 0.3f), kEnvPluck, kDelayDot, kHall }),
    { { "env_2", "filter_1_cutoff", 0.4f, false } } });
  d.push_back({ "Koto Pluck", "Pluck", { T("Karplus Strong", 6) },
    cat({ osc(1, 0.75f), filt(3, 2, 110, 0.3f), kEnvPluck, kDelay8, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Harp Glint", "Pluck", { T("Additive Synth", 6) },
    cat({ osc(1, 0.7f), filt(0, 0, 118, 0.1f), kEnvPluck, kShimmer }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Rubber Pluck", "Pluck", { T("Shaper Wave", 2) },
    cat({ osc(1, 0.7f), filt(2, 1, 66, 0.55f), kEnvPluck, kDelay16, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.35f, false } } });
  d.push_back({ "Kalimba Drop", "Pluck", { T("Melda Oscillator", 2) },
    cat({ osc(1, 0.75f), filt(0, 0, 108, 0.15f), kEnvPerc, kRoom, kDelay8 }),
    { { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Guitar Pick", "Pluck", { T("Karplus Strong", 1) },
    cat({ osc(1, 0.75f), uni(1, 2, 0.4f), filt(1, 1, 96, 0.2f, 4), kEnvPluck, dist(0, 2), kRoom }),
    { { "env_2", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Tropical Pluck", "Pluck", { T("Phase Distortion", 5) },
    cat({ osc(1, 0.7f), filt(0, 1, 92, 0.2f), kEnvPluck, kDelayDot, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false }, { "env_2", "filter_1_cutoff", 0.2f, false } } });

  // ---------------- BELL / MALLET (6) ----------------
  d.push_back({ "Crystal Bell", "Bell", { T("FM", 6) },
    cat({ osc(1, 0.7f), { { "filter_1_on", 0 } }, { { "env_1_attack", 0.0f }, { "env_1_decay", 1.35f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.25f } },
          kShimmer, kDelayDot }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Marimba Wood", "Bell", { T("Mod Sine", 6) },
    cat({ osc(1, 0.75f), filt(0, 0, 100, 0.1f), kEnvPerc, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Gamelan", "Bell", { T("RMFM", 3) },
    cat({ osc(1, 0.7f), morph(1, 4, 0.4f), filt(0, 0, 116, 0.1f),
          { { "env_1_attack", 0.0f }, { "env_1_decay", 1.25f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.15f } }, kHall }),
    { { "velocity", "osc_1_spectral_morph_amount", 0.3f, false } } });
  d.push_back({ "Music Box", "Bell", { T("Walsh Function", 6) },
    cat({ osc(1, 0.7f, 12), filt(0, 0, 120, 0.1f), kEnvPluck, kHall, kChorus }),
    {} });
  d.push_back({ "Vibes Tremolo", "Bell", { T("Mod Sine", 5) },
    cat({ osc(1, 0.75f), filt(0, 0, 110, 0.1f), kEnvKeys, kRoom, lfoHz(1, 2.5f) }),
    { { "lfo_1", "osc_1_level", 0.15f, true }, { "velocity", "osc_1_wave_frame", 0.3f, false } }, { 1 } });
  d.push_back({ "Tubular Chime", "Bell", { T("Hardcore FM", 7) },
    cat({ osc(1, 0.65f), { { "filter_1_on", 0 } }, { { "env_1_attack", 0.0f }, { "env_1_decay", 1.4f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.3f } }, kHall }),
    {} });

  // ---------------- ARP / SEQ (6) ----------------
  d.push_back({ "Gated Saws", "Arp", {},
    cat({ osc(1, 0.6f), uni(1, 5, 2.4f), filt(0, 1, 92, 0.2f), kEnvPad, lfoTempo(1, 10), kDelay8, kRoom }),
    { { "lfo_1", "osc_1_level", 0.7f, false } }, { 2 } });
  d.push_back({ "Seq Bubbles", "Arp", { T("Modulo", 10) },
    cat({ osc(1, 0.65f), filt(2, 1, 88, 0.5f), kEnvPerc, lfoTempo(1, 9), kDelayDot, kRoom }),
    { { "lfo_1", "filter_1_cutoff", 0.25f, false } }, { 3 } });
  d.push_back({ "Pulse Train", "Arp", { T("AMPWM", 2) },
    cat({ osc(1, 0.65f), filt(0, 1, 96, 0.2f), kEnvLead, lfoTempo(1, 10), kDelay16, kRoom }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } }, { 2 } });
  d.push_back({ "Random Blips", "Arp", { T("Softbe Modular", 6) },
    cat({ osc(1, 0.65f), filt(3, 0, 100, 0.3f), kEnvPerc, kDelayDot, kHall, { { "random_1_sync", 1 }, { "random_1_tempo", 10 } } }),
    { { "random_1", "osc_1_wave_frame", 0.8f, false }, { "random_1", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Trance Gate Pad", "Arp", { T("Korg Analog  Synth PhaseShift2", 4) },
    cat({ osc(1, 0.55f), uni(1, 6, 2.6f), filt(0, 1, 100, 0.1f), kEnvPad, lfoTempo(1, 10), kDelayDot, kHall }),
    { { "lfo_1", "osc_1_level", 0.8f, false } }, { 2 } });
  d.push_back({ "Acid Arp", "Arp", { T("PD PWM", 0) },
    cat({ osc(1, 0.65f), filt(2, 1, 60, 0.7f, 4), kEnvPerc, kDelay16, kRoom, dist(0, 4), lfoTempo(1, 6) }),
    { { "lfo_1", "filter_1_cutoff", 0.3f, false }, { "env_2", "filter_1_cutoff", 0.2f, false } }, { 3 } });

  // ---------------- BRASS / STRINGS (5) ----------------
  d.push_back({ "Synth Brass", "Brass", {},
    cat({ osc(1, 0.6f), uni(1, 4, 1.6f), filt(0, 1, 70, 0.15f),
          { { "env_1_attack", 0.45f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 0.85f }, { "env_1_release", 0.6f },
            { "env_2_attack", 0.55f }, { "env_2_decay", 0.95f }, { "env_2_sustain", 0.4f } }, kRoom, kChorus }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Brass Section", "Brass", { T("Alpha Juno SawPWM", 2) },
    cat({ osc(1, 0.6f), uni(1, 5, 2.0f), filt(0, 1, 74, 0.1f),
          { { "env_1_attack", 0.5f }, { "env_1_sustain", 0.9f }, { "env_1_release", 0.65f }, { "env_2_attack", 0.6f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 0.5f } }, kHall }),
    { { "env_2", "filter_1_cutoff", 0.28f, false }, { "velocity", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "String Ensemble", "Strings", { T("OSC Sync+PWM", 9) },
    cat({ osc(1, 0.55f), uni(1, 7, 2.8f), filt(0, 1, 96, 0.05f), kEnvPad, kChorus, kHall }),
    {} });
  d.push_back({ "Cello Bow", "Strings", { T("Bazille", 6) },
    cat({ osc(1, 0.6f), uni(1, 2, 0.6f), filt(0, 1, 88, 0.15f),
          { { "env_1_attack", 0.75f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.8f } }, kMonoGlide, kHall, lfoHz(1, 2.4f) }),
    { { "lfo_1", "osc_1_tune", 0.06f, true } }, { 1 } });
  d.push_back({ "Solina Dream", "Strings", { T("Korg Analog  Synth PhaseShift1", 10) },
    cat({ osc(1, 0.55f), uni(1, 6, 2.4f), filt(0, 0, 100, 0.0f), kEnvPad, kChorus, kPhaser, kHall }),
    {} });

  // ---------------- VOCAL (4) ----------------
  d.push_back({ "Aah Choir", "Vocal", { T("GMa Vowel", 0) },
    cat({ osc(1, 0.55f, 0, 60), uni(1, 7, 1.8f), filt(5, 0, 98, 0.25f), kEnvPad, kHall, kChorus }),
    {} });
  d.push_back({ "Talk Box", "Vocal", { T("Vocorder", 3) },
    cat({ osc(1, 0.65f), filt(5, 0, 100, 0.5f), kEnvLead, kMono, kRoom }),
    { { "mod_wheel", "osc_1_wave_frame", 1.0f, false }, { "env_2", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "Robot Voice", "Vocal", { T("FormantStep", 0) },
    cat({ osc(1, 0.65f), morph(1, 1, 0.5f), filt(3, 0, 104, 0.2f), kEnvLead, kRoom, dist(5, 0), { { "distortion_mix", 0.5f } }, lfoTempo(1, 9) }),
    { { "lfo_1", "osc_1_wave_frame", 0.8f, false } }, { 2 } });
  d.push_back({ "Ghost Vowels", "Vocal", { T("Formant", 1) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(5, 0, 96, 0.35f), kEnvSwell, kShimmer, lfoHz(1, -1.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.9f, false } }, { 1 } });

  // ---------------- TEXTURE / FX (6) ----------------
  d.push_back({ "Wind Tunnel", "FX", { T("Noise", 1) },
    cat({ osc(1, 0.55f), filt(0, 4, 80, 0.6f), kEnvSwell, kShimmer, lfoHz(1, -2.5f) }),
    { { "lfo_1", "filter_1_cutoff", 0.25f, false } }, { 1 } });
  d.push_back({ "Riser Sweep", "FX", { T("Filter Sweep", 2) },
    cat({ osc(1, 0.55f), uni(1, 6, 3.0f), filt(0, 1, 60, 0.3f),
          { { "env_2_attack", 1.6f }, { "env_2_sustain", 1.0f }, { "env_1_attack", 0.9f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.0f } },
          kHall, kFlanger }),
    { { "env_2", "filter_1_cutoff", 0.45f, false }, { "env_2", "osc_1_wave_frame", 1.0f, false } } });
  d.push_back({ "Insect Swarm", "FX", { T("Icarus Bugs", 2) },
    cat({ osc(1, 0.55f), uni(1, 8, 5.0f), filt(3, 2, 100, 0.3f), kEnvPad, kDelayDot, kRoom, { { "random_1_sync", 0 }, { "random_1_frequency", 2.0f } } }),
    { { "random_1", "osc_1_wave_frame", 0.7f, false } } });
  d.push_back({ "Dark Drone", "FX", { T("K-Devicees Terra", 3) },
    cat({ osc(1, 0.6f), uni(1, 4, 0.8f), osc(2, 0.4f, -12, 50), filt(2, 1, 66, 0.5f), kEnvSwell, kHall, lfoHz(1, -4.0f), lfoHz(2, -3.0f) }),
    { { "lfo_1", "filter_1_cutoff", 0.2f, false }, { "lfo_2", "osc_1_wave_frame", 0.8f, false } } });
  d.push_back({ "Radio Static", "FX", { T("RandomFFT", 1) },
    cat({ osc(1, 0.55f), filt(3, 2, 96, 0.4f), kEnvPad, dist(4, 0), kRoom, { { "random_1_sync", 0 }, { "random_1_frequency", 3.0f } } }),
    { { "random_1", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Space Bloom", "FX", { T("Combination of various synthesis methods", 4) },
    cat({ osc(1, 0.55f), uni(1, 5, 2.0f), morph(1, 9, 0.6f), filt(0, 1, 100, 0.1f), kEnvSwell, kShimmer, kDelayDot }),
    { { "env_2", "osc_1_spectral_morph_amount", 0.4f, false } } });

  // ---------------- PERC (3) ----------------
  d.push_back({ "Kaoss Kit Hit", "Perc", { T("MiniDrum[Kaossilator_", 1) },
    cat({ osc(1, 0.8f), filt(0, 1, 110, 0.2f), kEnvPerc, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Synth Tom", "Perc", { T("Mod Sine", 1) },
    cat({ osc(1, 0.85f), { { "filter_1_on", 0 } }, kEnvPerc, kRoom }),
    { { "env_2", "osc_1_transpose", 0.15f, false } } });
  d.push_back({ "Metal Hit", "Perc", { T("RMFM", 6) },
    cat({ osc(1, 0.7f), morph(1, 4, 0.5f), filt(3, 2, 112, 0.3f), kEnvPerc, kHall }),
    { { "velocity", "osc_1_spectral_morph_amount", 0.3f, false } } });
}

// ---------------------------------------------------------------------------------------------
// Expansion 2: keys, leads, plucks, pads, dreamy / lush sounds and synth Rhodes.
// ---------------------------------------------------------------------------------------------
static const C kEnvRhodes = { { "env_1_attack", 0.05f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 0.2f }, { "env_1_release", 0.9f },
                              { "env_2_attack", 0.0f }, { "env_2_decay", 0.9f }, { "env_2_sustain", 0.05f } };
static const C kEnvDream = { { "env_1_attack", 1.15f }, { "env_1_decay", 1.4f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.45f },
                             { "env_2_attack", 1.35f }, { "env_2_decay", 1.6f }, { "env_2_sustain", 0.7f } };
static const C kEnvSoftLead = { { "env_1_attack", 0.55f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 0.95f }, { "env_1_release", 0.8f },
                                { "env_2_attack", 0.4f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 0.5f } };
static const C kEnvLongPluck = { { "env_1_attack", 0.0f }, { "env_1_decay", 1.15f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.05f },
                                 { "env_2_attack", 0.0f }, { "env_2_decay", 0.75f }, { "env_2_sustain", 0.0f } };
static const C kVast = { { "reverb_dry_wet", 0.55f }, { "reverb_size", 1.0f }, { "reverb_decay_time", 2.6f } };
static const C kDelayQuarter = { { "delay_on", 1 }, { "delay_tempo", 8 }, { "delay_feedback", 0.5f }, { "delay_dry_wet", 0.26f } };

static void addMore2(std::vector<PatchDef>& d) {
  // ---------------- SYNTH RHODES (8) ----------------
  d.push_back({ "Rhodes Classic", "Rhodes", { T("FM", 1) },
    cat({ osc(1, 0.7f), filt(0, 0, 98, 0.1f), kEnvRhodes, kChorus, kRoom, lfoHz(1, 2.3f) }),
    { { "velocity", "osc_1_wave_frame", 0.45f, false }, { "env_2", "osc_1_wave_frame", 0.25f, false }, { "lfo_1", "osc_1_level", 0.1f, true } }, { 1 } });
  d.push_back({ "Rhodes Suitcase", "Rhodes", { T("PM[Simple]", 2) },
    cat({ osc(1, 0.7f), filt(0, 0, 96, 0.1f), kEnvRhodes, kPhaser, kRoom, lfoHz(1, 2.6f) }),
    { { "velocity", "osc_1_wave_frame", 0.5f, false }, { "lfo_1", "osc_1_level", 0.22f, true } }, { 1 } });
  d.push_back({ "Rhodes Neo Soul", "Rhodes", { T("Phase", 5) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.4f), filt(0, 0, 94, 0.1f), kEnvRhodes, kChorus, kHall, dist(0, 3) }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false }, { "env_2", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "Rhodes Lofi Tape", "Rhodes", { T("FM", 4) },
    cat({ osc(1, 0.7f), filt(0, 0, 86, 0.1f), kEnvRhodes, dist(5, 0), { { "distortion_mix", 0.3f } }, kChorus, kRoom, lfoHz(1, 0.6f) }),
    { { "velocity", "osc_1_wave_frame", 0.35f, false }, { "lfo_1", "osc_1_tune", 0.05f, true } } });
  d.push_back({ "Rhodes Bell Tines", "Rhodes", { T("Digitone", 1), T("Mod Sine", 5) },
    cat({ osc(1, 0.6f), osc(2, 0.35f, 12), filt(0, 0, 104, 0.1f), kEnvRhodes, kChorus, kHall }),
    { { "velocity", "osc_1_wave_frame", 0.5f, false }, { "env_2", "osc_2_level", 0.3f, false } } });
  d.push_back({ "Rhodes Dream", "Rhodes", { T("Korg Analog  Synth PhaseShift1", 3) },
    cat({ osc(1, 0.65f), uni(1, 3, 0.8f), filt(0, 0, 96, 0.1f), kEnvRhodes, kChorus, kVast, kDelayDot }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Rhodes Mark Drive", "Rhodes", { T("PM[Simple]", 7) },
    cat({ osc(1, 0.7f), filt(1, 0, 100, 0.15f, 5), kEnvRhodes, dist(0, 7), kPhaser, kRoom, lfoHz(1, 2.4f) }),
    { { "velocity", "osc_1_wave_frame", 0.55f, false }, { "velocity", "distortion_drive", 0.12f, false }, { "lfo_1", "osc_1_level", 0.12f, true } }, { 1 } });
  d.push_back({ "Rhodes Wurli Hybrid", "Rhodes", { T("PM[Simple]", 11) },
    cat({ osc(1, 0.7f), filt(0, 0, 100, 0.15f), kEnvRhodes, kChorus, kRoom }),
    { { "velocity", "osc_1_wave_frame", 0.45f, false }, { "env_2", "osc_1_wave_frame", 0.2f, false } } });

  // ---------------- KEYS (10) ----------------
  d.push_back({ "Velvet Keys", "Keys", { T("Mod Sine", 7) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.5f), filt(0, 0, 100, 0.1f), kEnvKeys, kChorus, kHall }),
    { { "velocity", "osc_1_wave_frame", 0.35f, false } } });
  d.push_back({ "Poly Synth Keys", "Keys", {},
    cat({ osc(1, 0.6f), uni(1, 3, 1.2f), osc(2, 0.4f, 0, 0), warp(2, 6, 0.4f), filt(0, 1, 84, 0.2f), kEnvKeys, kChorus, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.22f, false }, { "velocity", "filter_1_cutoff", 0.12f, false } } });
  d.push_back({ "OP Keys", "Keys", { T("TE OP-1", 12) },
    cat({ osc(1, 0.7f), filt(3, 0, 106, 0.1f), kEnvKeys, kRoom, kDelay8 }),
    { { "env_2", "osc_1_wave_frame", 0.35f, false } } });
  d.push_back({ "Analog Piano", "Keys", { T("AnalogFour", 7) },
    cat({ osc(1, 0.7f), filt(0, 0, 92, 0.15f), kEnvKeys, kRoom, kComp }),
    { { "velocity", "filter_1_cutoff", 0.2f, false }, { "env_2", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Glass Organ", "Keys", { T("Walsh Function", 1) },
    cat({ osc(1, 0.6f), osc(2, 0.3f, 12, 80), filt(0, 0, 116, 0.0f), { { "env_1_attack", 0.2f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.6f } },
          kChorus, kHall, lfoHz(1, 2.5f) }),
    { { "lfo_1", "osc_1_level", 0.08f, true } }, { 1 } });
  d.push_back({ "Hammer Dulcimer", "Keys", { T("Karplus Strong", 4) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.5f), filt(0, 0, 110, 0.15f), kEnvLongPluck, kHall }),
    { { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Soft Stab Keys", "Keys", { T("Alpha Juno SawPWM", 3) },
    cat({ osc(1, 0.6f), uni(1, 4, 1.4f), filt(0, 1, 80, 0.2f), kEnvPerc, kDelayDot, kHall }),
    { { "env_2", "filter_1_cutoff", 0.32f, false } } });
  d.push_back({ "Vector Keys", "Keys", { T("Prophet VS Wave", 2) },
    cat({ osc(1, 0.7f), filt(0, 0, 100, 0.1f), kEnvKeys, kChorus, kRoom, lfoHz(1, -1.0f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.3f, false }, { "velocity", "osc_1_wave_frame", 0.2f, false } } });
  d.push_back({ "Celeste", "Keys", { T("Mod Sine", 1) },
    cat({ osc(1, 0.7f, 12), filt(0, 0, 118, 0.0f), kEnvLongPluck, kHall, kChorus }),
    {} });
  d.push_back({ "House Organ Stab", "Keys", { T("Additive Synth", 8) },
    cat({ osc(1, 0.7f), filt(0, 0, 104, 0.1f), kEnvPerc, kRoom, kDelay8 }),
    { { "velocity", "filter_1_cutoff", 0.15f, false } } });

  // ---------------- LEADS (10) ----------------
  d.push_back({ "Silk Lead", "Lead", { T("Mod Sine", 6) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.4f), filt(0, 1, 110, 0.1f), kEnvSoftLead, kMonoGlide, kDelayDot, kHall, lfoHz(1, 2.4f) }),
    { { "lfo_1", "osc_1_tune", 0.05f, true }, { "mod_wheel", "osc_1_wave_frame", 0.6f, false } }, { 1 } });
  d.push_back({ "Bright Saw Lead", "Lead", {},
    cat({ osc(1, 0.6f), uni(1, 5, 2.0f), filt(0, 1, 112, 0.2f), kEnvLead, kMono, kDelay8, kRoom }),
    { { "mod_wheel", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Portamento Sine", "Lead", { T("Chebyshev polynomials", 0) },
    cat({ osc(1, 0.7f), { { "filter_1_on", 0 } }, kEnvSoftLead, kMonoGlide, { { "portamento_time", -2.0f } }, kVast, kDelayQuarter }),
    { { "mod_wheel", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Nasal Reed", "Lead", { T("PD PWM", 5) },
    cat({ osc(1, 0.65f), filt(5, 0, 96, 0.3f), kEnvLead, kMonoGlide, kRoom, lfoHz(1, 2.5f) }),
    { { "lfo_1", "osc_1_tune", 0.06f, true }, { "env_2", "osc_1_wave_frame", 0.3f, false } }, { 1 } });
  d.push_back({ "Glide Saw 80s", "Lead", { T("Alpha Juno SawPWM", 0) },
    cat({ osc(1, 0.6f), uni(1, 3, 1.0f), filt(0, 1, 104, 0.2f), kEnvLead, kMonoGlide, kChorus, kDelayDot, kRoom }),
    { { "mod_wheel", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Sync Scream", "Lead", { T("OSC Sync+PWM", 12) },
    cat({ osc(1, 0.6f), warp(1, 1, 0.5f), filt(1, 1, 100, 0.3f, 6), kEnvLead, kMonoGlide, dist(1, 6), kDelay16, kRoom }),
    { { "env_2", "osc_1_distortion_amount", 0.45f, false }, { "mod_wheel", "osc_1_distortion_amount", 0.4f, false } } });
  d.push_back({ "Flute Synth", "Lead", { T("Mod Sine", 3) },
    cat({ osc(1, 0.7f), filt(0, 0, 104, 0.1f), kEnvSoftLead, kMono, kHall, kNoise, { { "sample_level", 0.15f } }, lfoHz(1, 2.3f) }),
    { { "lfo_1", "osc_1_tune", 0.04f, true } }, { 1 } });
  d.push_back({ "Hoover Lead", "Lead", { T("AnalogFour", 9) },
    cat({ osc(1, 0.55f), uni(1, 8, 4.5f), filt(0, 1, 104, 0.2f), kEnvLead, kMonoGlide, { { "portamento_time", -2.5f } }, kChorus, kRoom }),
    { { "env_2", "osc_1_tune", -0.1f, false } } });
  d.push_back({ "Analog Brass Lead", "Lead", { T("AnalogFour", 1) },
    cat({ osc(1, 0.65f), uni(1, 2, 0.6f), filt(2, 1, 82, 0.25f, 3),
          { { "env_1_attack", 0.4f }, { "env_1_sustain", 0.9f }, { "env_1_release", 0.55f }, { "env_2_attack", 0.45f }, { "env_2_decay", 0.95f }, { "env_2_sustain", 0.4f } },
          kMono, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Crystal Lead", "Lead", { T("Additive Synth", 4) },
    cat({ osc(1, 0.65f), uni(1, 3, 0.8f), filt(0, 0, 118, 0.1f), kEnvSoftLead, kMonoGlide, kDelayDot, kVast }),
    { { "mod_wheel", "osc_1_wave_frame", 0.5f, false } } });

  // ---------------- PLUCKS (10) ----------------
  d.push_back({ "Lush Pluck", "Pluck", {},
    cat({ osc(1, 0.55f), uni(1, 7, 3.2f), filt(0, 1, 74, 0.25f), kEnvLongPluck, kChorus, kDelayDot, kVast }),
    { { "env_2", "filter_1_cutoff", 0.38f, false } } });
  d.push_back({ "Bell Pluck", "Pluck", { T("FM", 8) },
    cat({ osc(1, 0.7f), filt(0, 0, 110, 0.1f), kEnvPluck, kDelayDot, kHall }),
    { { "env_2", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Water Drop", "Pluck", { T("Mod Sine", 2) },
    cat({ osc(1, 0.75f), filt(0, 0, 112, 0.1f), kEnvPerc, kDelayQuarter, kHall }),
    { { "env_2", "osc_1_transpose", 0.08f, false }, { "env_2", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Nylon Pluck", "Pluck", { T("Karplus Strong", 0) },
    cat({ osc(1, 0.75f), filt(0, 1, 96, 0.15f), kEnvLongPluck, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.18f, false }, { "velocity", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Synth Harp", "Pluck", { T("Additive Synth", 2) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.4f), filt(0, 0, 112, 0.1f), kEnvLongPluck, kVast, kChorus }),
    { { "env_2", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Pluck Arp Glass", "Pluck", { T("Korg Analog  Synth PhaseShift2", 8) },
    cat({ osc(1, 0.65f), filt(3, 1, 90, 0.3f), kEnvPluck, kDelay16, kHall }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Mallet Pluck", "Pluck", { T("Melda Oscillator", 5) },
    cat({ osc(1, 0.75f), filt(0, 0, 104, 0.1f), kEnvPerc, kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Funky Wah Pluck", "Pluck", { T("PD Square", 5) },
    cat({ osc(1, 0.7f), filt(0, 2, 70, 0.6f), kEnvPluck, kRoom, kDelay16 }),
    { { "env_2", "filter_1_cutoff", 0.4f, false }, { "velocity", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Chime Pluck", "Pluck", { T("Buchla 259e-Red", 6) },
    cat({ osc(1, 0.7f), filt(0, 0, 116, 0.1f), kEnvLongPluck, kVast, kDelayDot }),
    { { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Deep Pluck", "Pluck", { T("Mod Sine", 4) },
    cat({ osc(1, 0.7f), osc(2, 0.4f, -12), filt(2, 1, 68, 0.35f), kEnvPluck, kRoom, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.35f, false } } });

  // ---------------- PADS (10) ----------------
  d.push_back({ "Silk Pad", "Pad", { T("Mod Sine", 3), T("Additive Synth", 1) },
    cat({ osc(1, 0.5f), uni(1, 5, 1.6f), osc(2, 0.35f, 12), uni(2, 3, 1.0f), filt(0, 1, 94, 0.1f), kEnvPad, kChorus, kHall }),
    { { "env_2", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "80s Poly Pad", "Pad", { T("Alpha Juno SawPWM", 2) },
    cat({ osc(1, 0.55f), uni(1, 6, 2.4f), filt(0, 1, 88, 0.15f), kEnvPad, kChorus, kHall, lfoHz(1, -1.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Breathing Pad", "Pad", { T("GMa Vowel", 0) },
    cat({ osc(1, 0.55f), uni(1, 5, 1.4f), filt(0, 1, 86, 0.2f), kEnvPad, kHall, kNoise, { { "sample_level", 0.12f } }, lfoHz(1, -2.6f) }),
    { { "lfo_1", "filter_1_cutoff", 0.12f, false }, { "lfo_1", "osc_1_wave_frame", 0.5f, false } }, { 1 } });
  d.push_back({ "Analog Strings Pad", "Pad", { T("OSC Sync+PWM", 2) },
    cat({ osc(1, 0.55f), uni(1, 7, 2.6f), filt(0, 1, 92, 0.1f), kEnvPad, kChorus, kPhaser, kHall }),
    {} });
  d.push_back({ "Glass Choir Pad", "Pad", { T("Vocorder", 2) },
    cat({ osc(1, 0.55f), uni(1, 6, 1.6f), filt(5, 0, 98, 0.3f), kEnvPad, kVast, kChorus, lfoHz(1, -2.4f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Evolving Spectra", "Pad", { T("Combination of various synthesis methods", 8) },
    cat({ osc(1, 0.5f), uni(1, 4, 1.4f), morph(1, 3, 0.4f), filt(0, 1, 96, 0.1f), kEnvPad, kHall, lfoHz(1, -3.2f), lfoHz(2, -2.4f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.8f, false }, { "lfo_2", "osc_1_spectral_morph_amount", 0.4f, true } } });
  d.push_back({ "Warm Analog Pad", "Pad", { T("AnalogFour", 3) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.8f), osc(2, 0.35f, -12), filt(2, 1, 80, 0.2f), kEnvPad, kChorus, kHall }),
    { { "env_2", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Pulse Width Pad", "Pad", { T("PD PWM", 1) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.4f), filt(0, 1, 92, 0.1f), kEnvPad, kChorus, kHall, lfoHz(1, -1.8f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.7f, false } } });
  d.push_back({ "Granular Pad", "Pad", { T("Tone2Icarus Granular", 5) },
    cat({ osc(1, 0.5f), uni(1, 5, 1.6f), filt(0, 1, 94, 0.1f), kEnvPad, kVast, kDelayDot, lfoHz(1, -2.8f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.9f, false } } });
  d.push_back({ "Night Pad", "Pad", { T("Softbe Modular", 3) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(2, 1, 78, 0.3f), kEnvPad, kHall, lfoHz(1, -3.4f) }),
    { { "lfo_1", "filter_1_cutoff", 0.15f, false } } });

  // ---------------- DREAMY / LUSH (12) ----------------
  d.push_back({ "Cloud Nine", "Dreamy", { T("Additive Synth", 5), T("Mod Sine", 3) },
    cat({ osc(1, 0.5f), uni(1, 6, 2.0f), osc(2, 0.35f, 12), uni(2, 4, 1.4f), filt(0, 1, 100, 0.1f), kEnvDream, kChorus, kVast, kDelayDot, lfoHz(1, -2.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Aurora", "Dreamy", { T("Korg Analog  Synth PhaseShift1", 12) },
    cat({ osc(1, 0.5f), uni(1, 7, 2.4f), filt(0, 1, 98, 0.1f), kEnvDream, kFlanger, kVast, lfoHz(1, -3.0f), lfoHz(2, -2.0f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.8f, false }, { "lfo_2", "filter_1_cutoff", 0.08f, true } } });
  d.push_back({ "Underwater Dream", "Dreamy", { T("Mod Sine", 5) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.4f), filt(2, 1, 70, 0.35f), kEnvDream, kChorus, kVast, kDelayQuarter, lfoHz(1, -1.5f) }),
    { { "lfo_1", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Starlight Keys", "Dreamy", { T("FM", 5) },
    cat({ osc(1, 0.6f), uni(1, 3, 0.8f), filt(0, 0, 108, 0.1f), kEnvLongPluck, kChorus, kVast, kDelayDot }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false }, { "env_2", "osc_1_wave_frame", 0.3f, false } } });
  d.push_back({ "Hazy Memories", "Dreamy", { T("M1 Wavetable[Mini][2048-44.1khz-32bit]", 5) },
    cat({ osc(1, 0.55f), uni(1, 5, 1.8f), filt(0, 1, 88, 0.1f), kEnvDream, dist(5, 0), { { "distortion_mix", 0.25f } }, kChorus, kVast, lfoHz(1, 0.4f) }),
    { { "lfo_1", "osc_1_tune", 0.04f, true } } });
  d.push_back({ "Ethereal Voices", "Dreamy", { T("GMa Vowel", 0), T("Vocorder", 0) },
    cat({ osc(1, 0.45f), uni(1, 6, 1.6f), osc(2, 0.35f, 12), uni(2, 4, 1.2f), filt(5, 0, 100, 0.25f), kEnvDream, kVast, kChorus, lfoHz(1, -2.6f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.7f, false }, { "lfo_1", "osc_2_wave_frame", 0.5f, false } } });
  d.push_back({ "Lush Supersaw Pad", "Dreamy", {},
    cat({ osc(1, 0.5f), uni(1, 8, 3.4f), osc(2, 0.4f, 12), uni(2, 6, 2.8f), filt(0, 1, 96, 0.1f), kEnvDream, kChorus, kVast, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.12f, false } } });
  d.push_back({ "Soft Focus", "Dreamy", { T("Prophet VS Wave", 6) },
    cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(0, 1, 90, 0.1f), kEnvDream, kPhaser, kVast, lfoHz(1, -2.8f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Moonlit Bells", "Dreamy", { T("Digitone", 4) },
    cat({ osc(1, 0.6f), filt(0, 0, 112, 0.1f), kEnvLongPluck, kChorus, kVast, kDelayQuarter }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Daydream Swell", "Dreamy", { T("Filter Sweep", 0) },
    cat({ osc(1, 0.5f), uni(1, 6, 2.2f), filt(0, 1, 100, 0.1f), kEnvSwell, kChorus, kVast }),
    { { "env_2", "osc_1_wave_frame", 0.9f, false } } });
  d.push_back({ "Shimmer Strings", "Dreamy", { T("OSC Sync+PWM", 9) },
    cat({ osc(1, 0.5f), uni(1, 7, 2.6f), osc(2, 0.3f, 12, 60), filt(0, 1, 100, 0.05f), kEnvDream, kChorus, kVast }),
    {} });
  d.push_back({ "Weightless", "Dreamy", { T("Random Spectrum", 2) },
    cat({ osc(1, 0.5f), uni(1, 5, 2.0f), morph(1, 5, 0.5f), filt(0, 1, 96, 0.1f), kEnvDream, kVast, kDelayDot, lfoHz(1, -3.5f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.9f, false } } });
}

// ---------------------------------------------------------------------------------------------
// Expansion 3: ten more for every type. Compact helper: P(name, style, tables, controls, mods, lfo shapes).
// ---------------------------------------------------------------------------------------------
static PatchDef P(const std::string& n, const std::string& st, std::vector<std::string> t, C c, M m = {}, std::vector<int> l = {}) {
  return { n, st, std::move(t), std::move(c), std::move(m), std::move(l) };
}
static const C kEnvBrass = { { "env_1_attack", 0.45f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 0.85f }, { "env_1_release", 0.6f },
                             { "env_2_attack", 0.5f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 0.45f } };
static const C kEnvBow = { { "env_1_attack", 0.8f }, { "env_1_decay", 1.2f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.95f },
                           { "env_2_attack", 0.9f }, { "env_2_decay", 1.2f }, { "env_2_sustain", 0.6f } };
static const C kEnvArp = { { "env_1_attack", 0.05f }, { "env_1_decay", 0.75f }, { "env_1_sustain", 0.1f }, { "env_1_release", 0.5f },
                           { "env_2_attack", 0.0f }, { "env_2_decay", 0.55f }, { "env_2_sustain", 0.0f } };
static const C kEnvHit = { { "env_1_attack", 0.0f }, { "env_1_decay", 0.5f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.45f },
                           { "env_2_attack", 0.0f }, { "env_2_decay", 0.35f }, { "env_2_sustain", 0.0f } };

static void addMore3(std::vector<PatchDef>& d) {
  // ---------------- ARP (10) ----------------
  d.push_back(P("Crystal Steps", "Arp", { T("Additive Synth", 7) }, cat({ osc(1, 0.65f), filt(0, 0, 112, 0.1f), kEnvArp, kDelayDot, kHall }), { { "env_2", "osc_1_wave_frame", 0.5f, false } }));
  d.push_back(P("Saw Sequence", "Arp", {}, cat({ osc(1, 0.6f), uni(1, 3, 1.2f), filt(2, 1, 64, 0.55f, 3), kEnvArp, kDelay16, kRoom }), { { "env_2", "filter_1_cutoff", 0.4f, false }, { "velocity", "filter_1_cutoff", 0.15f, false } }));
  d.push_back(P("Ping Pong Bells", "Arp", { T("FM", 9) }, cat({ osc(1, 0.65f), { { "filter_1_on", 0 } }, kEnvArp, kDelayDot, { { "delay_style", 1 } }, kHall }), {}));
  d.push_back(P("Gated Choir", "Arp", { T("GMa Vowel", 0) }, cat({ osc(1, 0.55f), uni(1, 5, 1.4f), filt(5, 0, 98, 0.3f), kEnvPad, lfoTempo(1, 10), kHall }), { { "lfo_1", "osc_1_level", 0.8f, false } }, { 2 }));
  d.push_back(P("Retro Arcade", "Arp", { T("PPG Wave[4bit]", 15) }, cat({ osc(1, 0.6f), { { "filter_1_on", 0 } }, kEnvArp, dist(4, 0), kDelay16 }), { { "env_2", "osc_1_wave_frame", 0.6f, false } }));
  d.push_back(P("Sidechain Stabs", "Arp", { T("AnalogFour", 4) }, cat({ osc(1, 0.6f), uni(1, 5, 2.2f), filt(0, 1, 90, 0.2f), kEnvPad, lfoTempo(1, 8), kRoom }), { { "lfo_1", "osc_1_level", 0.85f, false } }, { 3 }));
  d.push_back(P("Triplet Motion", "Arp", { T("Modulo", 3) }, cat({ osc(1, 0.6f), filt(3, 1, 88, 0.35f), kEnvArp, lfoTempo(1, 9, 3), kDelay8, kRoom }), { { "lfo_1", "filter_1_cutoff", 0.3f, false } }, { 1 }));
  d.push_back(P("Morph Steps", "Arp", { T("TE OP-1", 22) }, cat({ osc(1, 0.6f), filt(0, 1, 96, 0.2f), kEnvArp, kDelayDot, kRoom, { { "random_1_sync", 1 }, { "random_1_tempo", 10 } } }), { { "random_1", "osc_1_wave_frame", 0.9f, false } }));
  d.push_back(P("Dark Pulse Seq", "Arp", { T("PD Square", 6) }, cat({ osc(1, 0.65f), filt(4, 1, 60, 0.6f, 4), kEnvArp, kDelay16, dist(0, 4) }), { { "env_2", "filter_1_cutoff", 0.35f, false } }));
  d.push_back(P("Harp Cascade", "Arp", { T("Karplus Strong", 7) }, cat({ osc(1, 0.7f), filt(0, 0, 110, 0.1f), kEnvLongPluck, kDelayDot, kVast }), { { "env_2", "osc_1_wave_frame", 0.4f, false } }));

  // ---------------- BASS (10) ----------------
  d.push_back(P("Moog Thump", "Bass", { T("Audacity", 2) }, cat({ osc(1, 0.75f), filt(2, 1, 56, 0.45f, 4), kEnvBass, kMono, kDry, kComp }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Garage Organ Bass", "Bass", { T("Walsh Function", 5) }, cat({ osc(1, 0.75f), filt(0, 0, 88, 0.15f), kEnvBass, kMono, kDry, dist(0, 3) }), { { "velocity", "filter_1_cutoff", 0.12f, false } }));
  d.push_back(P("Fat Unison Bass", "Bass", {}, cat({ osc(1, 0.6f), uni(1, 6, 2.0f), osc(2, 0.5f, -12, 0), filt(0, 1, 70, 0.2f), kEnvBass, kMono, kDry, kComp }), { { "env_2", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Liquid Sub", "Bass", { T("Mod Sine", 0) }, cat({ osc(1, 0.85f), osc(2, 0.25f, 12, 0), filt(0, 0, 70, 0.1f), kEnvBass, kMonoGlide, kDry }), { { "velocity", "osc_2_level", 0.3f, false } }));
  d.push_back(P("CZ Resonance Bass", "Bass", { T("CZ Reso", 2) }, cat({ osc(1, 0.7f), filt(0, 1, 90, 0.2f), kEnvBass, kMono, kDry }), { { "env_2", "osc_1_wave_frame", 0.7f, false } }));
  d.push_back(P("Talking Bass", "Bass", { T("Vocorder", 0) }, cat({ osc(1, 0.7f), filt(5, 0, 84, 0.45f), kEnvBass, kMono, kDry, dist(0, 6), lfoTempo(1, 9) }), { { "lfo_1", "osc_1_wave_frame", 0.8f, false } }, { 1 }));
  d.push_back(P("Detuned Square Bass", "Bass", { T("PD Square", 2) }, cat({ osc(1, 0.6f), uni(1, 3, 1.2f), filt(2, 1, 66, 0.35f), kEnvBass, kMonoGlide, kDry }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Foldback Bass", "Bass", { T("Waveshaper", 1) }, cat({ osc(1, 0.7f), filt(1, 1, 92, 0.3f, 6), kEnvBass, kMono, kDry, dist(3, 4) }), { { "env_2", "osc_1_wave_frame", 0.6f, false }, { "velocity", "distortion_drive", 0.1f, false } }));
  d.push_back(P("Pluck Bass", "Bass", { T("Karplus Strong", 2) }, cat({ osc(1, 0.8f), filt(0, 1, 76, 0.25f), kEnvPerc, kMono, kDry, kComp }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Dubstep Yoi", "Bass", { T("Formant", 0) }, cat({ osc(1, 0.7f), uni(1, 3, 1.4f), filt(5, 0, 96, 0.5f), kEnvBass, kMono, kDry, dist(2, 8), lfoTempo(1, 8, 3) }), { { "lfo_1", "osc_1_wave_frame", 1.0f, false }, { "lfo_1", "filter_1_cutoff", 0.2f, false } }, { 3 }));

  // ---------------- BELL (10) ----------------
  d.push_back(P("Glass Chimes", "Bell", { T("Additive Synth", 9) }, cat({ osc(1, 0.65f), { { "filter_1_on", 0 } }, kEnvLongPluck, kVast, kDelayDot }), {}));
  d.push_back(P("Church Bell", "Bell", { T("RMFM", 1) }, cat({ osc(1, 0.6f, -12), { { "filter_1_on", 0 } }, { { "env_1_attack", 0.0f }, { "env_1_decay", 1.5f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.4f } }, kHall }), {}));
  d.push_back(P("Toy Piano", "Bell", { T("Mod Sine", 5) }, cat({ osc(1, 0.8f, 12), { { "filter_1_on", 0 } }, kEnvPerc, kRoom }), { { "env_2", "osc_1_wave_frame", 0.5f, false } }));
  d.push_back(P("Steel Drum", "Bell", { T("Mod Sine", 6) }, cat({ osc(1, 0.75f), morph(1, 4, 0.3f), filt(0, 0, 108, 0.1f), kEnvPerc, kRoom }), { { "env_2", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Glockenspiel", "Bell", { T("FM", 11) }, cat({ osc(1, 0.65f, 24), { { "filter_1_on", 0 } }, kEnvLongPluck, kHall }), { { "velocity", "osc_1_wave_frame", 0.3f, false } }));
  d.push_back(P("FM Bell Pad", "Bell", { T("FM", 7) }, cat({ osc(1, 0.6f), uni(1, 3, 0.6f), filt(0, 0, 112, 0.1f), { { "env_1_attack", 0.0f }, { "env_1_decay", 1.4f }, { "env_1_sustain", 0.45f }, { "env_1_release", 1.3f } }, kChorus, kVast }), { { "env_2", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Wind Chime", "Bell", { T("Chebyshev polynomials", 3) }, cat({ osc(1, 0.6f, 12), { { "filter_1_on", 0 } }, kEnvLongPluck, kVast, kDelayQuarter, { { "random_1_sync", 0 }, { "random_1_frequency", 1.0f } } }), { { "random_1", "osc_1_wave_frame", 0.6f, false } }));
  d.push_back(P("Vibraphone Soft", "Bell", { T("Mod Sine", 2) }, cat({ osc(1, 0.75f), filt(0, 0, 104, 0.1f), kEnvKeys, kRoom, lfoHz(1, 2.2f) }), { { "lfo_1", "osc_1_level", 0.12f, true } }, { 1 }));
  d.push_back(P("Metallic Ring", "Bell", { T("Hardcore FM", 1) }, cat({ osc(1, 0.6f), morph(1, 4, 0.5f), filt(3, 0, 112, 0.2f), kEnvLongPluck, kHall }), { { "velocity", "osc_1_spectral_morph_amount", 0.3f, false } }));
  d.push_back(P("Bowl Resonance", "Bell", { T("Mod Sine", 7) }, cat({ osc(1, 0.65f), filt(6, 0, 100, 0.4f), kEnvSwell, kVast }), { { "env_2", "filter_1_cutoff", 0.1f, false } }));

  // ---------------- BRASS (10) ----------------
  d.push_back(P("Big Synth Horns", "Brass", {}, cat({ osc(1, 0.55f), uni(1, 6, 2.0f), filt(0, 1, 72, 0.15f), kEnvBrass, kChorus, kHall }), { { "env_2", "filter_1_cutoff", 0.32f, false } }));
  d.push_back(P("Jump Brass", "Brass", { T("Alpha Juno SawPWM", 0) }, cat({ osc(1, 0.6f), uni(1, 4, 1.6f), filt(0, 1, 78, 0.15f), kEnvBrass, kChorus, kRoom }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Muted Trumpet", "Brass", { T("PD PWM", 3) }, cat({ osc(1, 0.65f), filt(5, 0, 90, 0.35f), kEnvBrass, kMonoGlide, kRoom, lfoHz(1, 2.4f) }), { { "lfo_1", "osc_1_tune", 0.05f, true }, { "env_2", "filter_1_cutoff", 0.15f, false } }, { 1 }));
  d.push_back(P("Funk Brass Stab", "Brass", { T("AnalogFour", 6) }, cat({ osc(1, 0.6f), uni(1, 4, 1.4f), filt(0, 1, 76, 0.2f), kEnvPerc, kRoom }), { { "env_2", "filter_1_cutoff", 0.38f, false } }));
  d.push_back(P("Soft French Horn", "Brass", { T("Mod Sine", 4) }, cat({ osc(1, 0.7f), filt(0, 1, 78, 0.1f), kEnvBow, kHall }), { { "env_2", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Prophet Brass", "Brass", { T("Prophet VS Wave", 0) }, cat({ osc(1, 0.6f), uni(1, 3, 1.2f), filt(0, 1, 74, 0.2f), kEnvBrass, kPhaser, kRoom }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Tuba Low", "Brass", { T("PD Square", 4) }, cat({ osc(1, 0.75f, -12), filt(2, 1, 60, 0.2f), kEnvBrass, kMono, kRoom }), { { "env_2", "filter_1_cutoff", 0.25f, false } }));
  d.push_back(P("Swell Brass Pad", "Brass", { T("Alpha Juno SawPWM", 3) }, cat({ osc(1, 0.55f), uni(1, 6, 2.2f), filt(0, 1, 80, 0.1f), kEnvSwell, kChorus, kHall }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Sax Synth", "Brass", { T("Formant", 1) }, cat({ osc(1, 0.65f), filt(5, 0, 94, 0.3f), kEnvBrass, kMonoGlide, kRoom, kNoise, { { "sample_level", 0.1f } }, lfoHz(1, 2.5f) }), { { "lfo_1", "osc_1_tune", 0.05f, true }, { "env_2", "osc_1_wave_frame", 0.4f, false } }, { 1 }));
  d.push_back(P("Brass Fanfare", "Brass", {}, cat({ osc(1, 0.55f), uni(1, 5, 1.8f), osc(2, 0.4f, 7), uni(2, 3, 1.2f), filt(0, 1, 82, 0.15f), kEnvBrass, kHall }), { { "env_2", "filter_1_cutoff", 0.3f, false } }));

  // ---------------- DREAMY (10) ----------------
  d.push_back(P("Northern Lights", "Dreamy", { T("Korg Analog  Synth PhaseShift2", 10) }, cat({ osc(1, 0.5f), uni(1, 7, 2.2f), filt(0, 1, 98, 0.1f), kEnvDream, kFlanger, kVast, lfoHz(1, -3.2f) }), { { "lfo_1", "osc_1_wave_frame", 0.8f, false } }));
  d.push_back(P("Velvet Haze", "Dreamy", { T("Mod Sine", 5), T("Additive Synth", 3) }, cat({ osc(1, 0.5f), uni(1, 5, 1.4f), osc(2, 0.3f, 12), filt(0, 1, 92, 0.1f), kEnvDream, kChorus, kVast, kDelayDot }), {}));
  d.push_back(P("Lullaby", "Dreamy", { T("Walsh Function", 0) }, cat({ osc(1, 0.6f, 12), filt(0, 0, 104, 0.1f), kEnvLongPluck, kChorus, kVast, kDelayQuarter }), {}));
  d.push_back(P("Floating", "Dreamy", { T("Tone2Icarus Granular", 0) }, cat({ osc(1, 0.5f), uni(1, 5, 1.6f), filt(0, 1, 96, 0.1f), kEnvDream, kVast, lfoHz(1, -2.9f) }), { { "lfo_1", "osc_1_wave_frame", 0.9f, false } }));
  d.push_back(P("Sunset Glow", "Dreamy", { T("AnalogFour", 8) }, cat({ osc(1, 0.5f), uni(1, 6, 2.4f), filt(0, 1, 88, 0.1f), kEnvDream, kChorus, kPhaser, kVast }), { { "env_2", "filter_1_cutoff", 0.12f, false } }));
  d.push_back(P("Dream Piano", "Dreamy", { T("FM", 3) }, cat({ osc(1, 0.6f), uni(1, 2, 0.5f), filt(0, 0, 100, 0.1f), kEnvRhodes, kChorus, kVast, kDelayDot }), { { "velocity", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Snowfall", "Dreamy", { T("Additive Synth", 6) }, cat({ osc(1, 0.55f, 12), uni(1, 3, 0.8f), filt(0, 0, 114, 0.1f), kEnvLongPluck, kVast, kDelayDot, { { "random_1_sync", 0 }, { "random_1_frequency", 0.5f } } }), { { "random_1", "osc_1_wave_frame", 0.5f, false } }));
  d.push_back(P("Hologram", "Dreamy", { T("jo.Spectral Morph 1.0", 1) }, cat({ osc(1, 0.5f), uni(1, 5, 1.8f), morph(1, 9, 0.5f), filt(0, 1, 98, 0.1f), kEnvDream, kVast, lfoHz(1, -2.4f) }), { { "lfo_1", "osc_1_spectral_morph_amount", 0.4f, true } }));
  d.push_back(P("Tide Pool", "Dreamy", { T("Mod Sine", 3) }, cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(2, 1, 74, 0.35f), kEnvDream, kChorus, kVast, lfoHz(1, -2.0f) }), { { "lfo_1", "filter_1_cutoff", 0.18f, false } }, { 1 }));
  d.push_back(P("Stardust", "Dreamy", { T("Random Spectrum", 0) }, cat({ osc(1, 0.5f), uni(1, 6, 2.0f), filt(0, 1, 100, 0.1f), kEnvDream, kVast, kDelayQuarter, kFlanger }), { { "env_2", "osc_1_wave_frame", 0.7f, false } }));

  // ---------------- FX (10) ----------------
  d.push_back(P("Laser Zap", "FX", { T("Mod Sine", 0) }, cat({ osc(1, 0.7f, 24), { { "filter_1_on", 0 } }, kEnvHit, kRoom }), { { "env_2", "osc_1_transpose", -0.5f, false } }));
  d.push_back(P("Downlifter", "FX", { T("Noise", 2) }, cat({ osc(1, 0.6f), filt(0, 1, 120, 0.4f), { { "env_1_attack", 0.0f }, { "env_1_decay", 1.6f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.0f }, { "env_2_attack", 0.0f }, { "env_2_decay", 1.6f }, { "env_2_sustain", 0.0f } }, kHall }), { { "env_2", "filter_1_cutoff", 0.4f, false } }));
  d.push_back(P("Alarm Siren", "FX", { T("PD Square", 0) }, cat({ osc(1, 0.6f), filt(0, 1, 104, 0.2f), kEnvPad, lfoHz(1, 0.0f), kRoom }), { { "lfo_1", "osc_1_transpose", 0.1f, true } }, { 1 }));
  d.push_back(P("Glitch Machine", "FX", { T("Softbe Modular", 11) }, cat({ osc(1, 0.6f), filt(3, 2, 96, 0.4f), kEnvPad, dist(5, 0), kDelay16, { { "random_1_sync", 1 }, { "random_1_tempo", 11 } } }), { { "random_1", "osc_1_wave_frame", 1.0f, false }, { "random_1", "filter_1_cutoff", 0.3f, false } }));
  d.push_back(P("Space Drone", "FX", { T("K-Devicees Terra", 5) }, cat({ osc(1, 0.55f), uni(1, 6, 1.2f), filt(6, 0, 80, 0.4f), kEnvSwell, kVast, lfoHz(1, -4.0f) }), { { "lfo_1", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Robot Chatter", "FX", { T("Icarus Bugs", 0) }, cat({ osc(1, 0.6f), filt(5, 0, 96, 0.4f), kEnvPad, kRoom, { { "random_1_sync", 0 }, { "random_1_frequency", 3.0f } } }), { { "random_1", "osc_1_wave_frame", 0.9f, false }, { "random_1", "osc_1_transpose", 0.08f, true } }));
  d.push_back(P("Underwater Sonar", "FX", { T("Mod Sine", 1) }, cat({ osc(1, 0.7f, 12), filt(0, 0, 100, 0.2f), kEnvHit, kDelayQuarter, kVast }), {}));
  d.push_back(P("Tape Stop", "FX", { T("AnalogFour", 0) }, cat({ osc(1, 0.6f), uni(1, 3, 1.0f), filt(0, 1, 100, 0.2f), { { "env_1_attack", 0.0f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 0.0f }, { "env_2_attack", 0.0f }, { "env_2_decay", 1.3f }, { "env_2_sustain", 0.0f } }, kRoom }), { { "env_2", "osc_1_transpose", 0.4f, false } }));
  d.push_back(P("White Noise Riser", "FX", {}, cat({ { { "osc_1_on", 0 } }, kNoise, { { "sample_level", 0.8f } }, filt(0, 1, 60, 0.45f), { { "env_1_attack", 1.7f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.8f }, { "env_2_attack", 1.8f }, { "env_2_sustain", 1.0f } }, kHall, kFlanger }), { { "env_2", "filter_1_cutoff", 0.6f, false } }));
  d.push_back(P("Dial Up", "FX", { T("RandomFFT", 3) }, cat({ osc(1, 0.55f), filt(3, 2, 100, 0.4f), kEnvPad, dist(4, 0), kRoom, lfoTempo(1, 11) }), { { "lfo_1", "osc_1_wave_frame", 1.0f, false } }, { 2 }));

  // ---------------- KEYS (10) ----------------
  d.push_back(P("Upright Felt", "Keys", { T("Mod Sine", 6) }, cat({ osc(1, 0.75f), filt(0, 0, 92, 0.1f), kEnvKeys, kRoom }), { { "velocity", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Juno Keys", "Keys", { T("Alpha Juno SawPWM", 1) }, cat({ osc(1, 0.65f), uni(1, 2, 0.6f), filt(0, 1, 92, 0.15f), kEnvKeys, kChorus, kRoom }), { { "env_2", "filter_1_cutoff", 0.15f, false } }));
  d.push_back(P("Harpsichord", "Keys", { T("PD PWM", 4) }, cat({ osc(1, 0.65f), osc(2, 0.35f, 12), filt(0, 0, 112, 0.1f), kEnvLongPluck, kRoom }), {}));
  d.push_back(P("Gospel Organ", "Keys", { T("Additive Synth", 0) }, cat({ osc(1, 0.6f), osc(2, 0.3f, 12, 40), filt(0, 0, 118, 0.0f), { { "env_1_attack", 0.1f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.5f } }, kPhaser, kRoom, dist(0, 4) }), {}));
  d.push_back(P("Digital Grand", "Keys", { T("M1 Piano", 0) }, cat({ osc(1, 0.75f), filt(0, 0, 116, 0.1f), kEnvKeys, kHall, kComp }), { { "velocity", "filter_1_cutoff", 0.15f, false } }));
  d.push_back(P("Plastic Keys", "Keys", { T("TE OP-1", 28) }, cat({ osc(1, 0.7f), filt(3, 0, 104, 0.1f), kEnvKeys, kChorus, kRoom }), { { "env_2", "osc_1_wave_frame", 0.3f, false } }));
  d.push_back(P("Chord Memory", "Keys", {}, cat({ osc(1, 0.55f), uni(1, 4, 1.6f), osc(2, 0.35f, 7), filt(0, 1, 82, 0.2f), kEnvPerc, kDelayDot, kHall }), { { "env_2", "filter_1_cutoff", 0.35f, false } }));
  d.push_back(P("Toy Keys", "Keys", { T("PPG Wave", 20) }, cat({ osc(1, 0.7f), filt(0, 0, 110, 0.1f), kEnvKeys, kChorus, kRoom }), {}));
  d.push_back(P("Clavinet Pop", "Keys", { T("PD PWM", 2) }, cat({ osc(1, 0.7f), filt(3, 2, 108, 0.3f), kEnvPerc, kRoom, kPhaser }), { { "velocity", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Warm EP Pad Keys", "Keys", { T("Phase", 2) }, cat({ osc(1, 0.65f), uni(1, 3, 0.8f), filt(0, 0, 96, 0.1f), { { "env_1_attack", 0.3f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 0.6f }, { "env_1_release", 1.0f } }, kChorus, kHall }), { { "velocity", "osc_1_wave_frame", 0.3f, false } }));

  // ---------------- LEAD (10) ----------------
  d.push_back(P("Whistle Lead", "Lead", { T("Mod Sine", 0) }, cat({ osc(1, 0.7f, 12), { { "filter_1_on", 0 } }, kEnvSoftLead, kMonoGlide, kHall, lfoHz(1, 2.5f) }), { { "lfo_1", "osc_1_tune", 0.05f, true } }, { 1 }));
  d.push_back(P("G-Funk Lead", "Lead", { T("Mod Sine", 4) }, cat({ osc(1, 0.7f), filt(0, 1, 100, 0.1f), kEnvSoftLead, kMonoGlide, { { "portamento_time", -2.0f } }, kRoom, lfoHz(1, 2.3f) }), { { "lfo_1", "osc_1_tune", 0.08f, true } }, { 1 }));
  d.push_back(P("Acid Lead", "Lead", {}, cat({ osc(1, 0.65f), filt(2, 1, 72, 0.7f, 4), kEnvLead, kMonoGlide, dist(0, 5), kDelay16, kRoom }), { { "env_2", "filter_1_cutoff", 0.4f, false } }));
  d.push_back(P("Trance Lead", "Lead", {}, cat({ osc(1, 0.55f), uni(1, 7, 3.0f), osc(2, 0.35f, 12), uni(2, 5, 2.4f), filt(0, 1, 116, 0.1f), kEnvLead, kDelayDot, kHall }), {}));
  d.push_back(P("Formant Lead", "Lead", { T("Formant", 0) }, cat({ osc(1, 0.65f), filt(5, 0, 100, 0.35f), kEnvLead, kMonoGlide, kDelay8, kRoom }), { { "mod_wheel", "osc_1_wave_frame", 0.8f, false }, { "env_2", "osc_1_wave_frame", 0.3f, false } }));
  d.push_back(P("Chip Lead", "Lead", { T("PPG Wave[4bit]", 3) }, cat({ osc(1, 0.6f), { { "filter_1_on", 0 } }, kEnvLead, kMono, dist(4, 0), kDelay16, lfoHz(1, 2.8f) }), { { "lfo_1", "osc_1_tune", 0.1f, true } }, { 2 }));
  d.push_back(P("Distorted Saw Lead", "Lead", {}, cat({ osc(1, 0.6f), uni(1, 3, 1.2f), filt(1, 1, 98, 0.3f, 8), kEnvLead, kMono, dist(1, 10), kDelay8, kRoom }), { { "mod_wheel", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Ocarina", "Lead", { T("Mod Sine", 2) }, cat({ osc(1, 0.7f), filt(0, 0, 100, 0.1f), kEnvSoftLead, kMono, kNoise, { { "sample_level", 0.08f } }, kHall, lfoHz(1, 2.2f) }), { { "lfo_1", "osc_1_tune", 0.04f, true } }, { 1 }));
  d.push_back(P("Plaits Lead", "Lead", { T("Mutable Instruments Plaits", 2) }, cat({ osc(1, 0.65f), filt(0, 1, 104, 0.2f), kEnvLead, kMonoGlide, kDelayDot, kRoom }), { { "env_2", "osc_1_wave_frame", 0.5f, false }, { "mod_wheel", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Wah Lead", "Lead", { T("AnalogFour", 2) }, cat({ osc(1, 0.65f), filt(0, 2, 80, 0.6f), kEnvLead, kMono, kRoom, lfoTempo(1, 9) }), { { "lfo_1", "filter_1_cutoff", 0.3f, false } }, { 1 }));

  // ---------------- PAD (10) ----------------
  d.push_back(P("Cathedral", "Pad", { T("Additive Synth", 0) }, cat({ osc(1, 0.55f), uni(1, 5, 1.2f), osc(2, 0.3f, 12, 40), filt(0, 0, 100, 0.0f), kEnvPad, kVast }), {}));
  d.push_back(P("Ice Field", "Pad", { T("Korg Analog  Synth PhaseShift1", 14) }, cat({ osc(1, 0.5f), uni(1, 6, 2.4f), filt(3, 1, 104, 0.1f), kEnvPad, kFlanger, kVast }), { { "env_2", "osc_1_wave_frame", 0.5f, false } }));
  d.push_back(P("Desert Wind Pad", "Pad", { T("Noise", 3) }, cat({ osc(1, 0.5f), filt(0, 4, 84, 0.5f), kEnvPad, kHall, lfoHz(1, -2.0f) }), { { "lfo_1", "filter_1_cutoff", 0.15f, false } }, { 1 }));
  d.push_back(P("Retro Sci-Fi Pad", "Pad", { T("PPG Wave", 11) }, cat({ osc(1, 0.55f), uni(1, 3, 1.0f), filt(0, 1, 94, 0.15f), kEnvPad, kHall, lfoHz(1, -2.2f) }), { { "lfo_1", "osc_1_wave_frame", 0.7f, false } }));
  d.push_back(P("Choral Pad", "Pad", { T("Vocorder", 1) }, cat({ osc(1, 0.5f), uni(1, 7, 1.8f), filt(5, 0, 96, 0.25f), kEnvPad, kChorus, kHall }), {}));
  d.push_back(P("Dark Strings Pad", "Pad", { T("OSC Sync+PWM", 6) }, cat({ osc(1, 0.55f), uni(1, 6, 2.2f), filt(2, 1, 74, 0.2f), kEnvPad, kHall }), { { "env_2", "filter_1_cutoff", 0.15f, false } }));
  d.push_back(P("Shimmer Pad", "Pad", { T("Additive Synth", 4) }, cat({ osc(1, 0.5f), uni(1, 5, 1.6f), osc(2, 0.3f, 19), filt(0, 1, 104, 0.1f), kEnvPad, kVast, kChorus }), {}));
  d.push_back(P("Phase Pad", "Pad", { T("Phase", 7) }, cat({ osc(1, 0.55f), uni(1, 4, 1.4f), filt(7, 0, 96, 0.3f), kEnvPad, kHall, lfoHz(1, -2.5f) }), { { "lfo_1", "filter_1_cutoff", 0.15f, false } }));
  d.push_back(P("Bright Poly Pad", "Pad", {}, cat({ osc(1, 0.5f), uni(1, 6, 2.6f), filt(0, 1, 104, 0.1f), kEnvPad, kChorus, kHall }), { { "env_2", "filter_1_cutoff", 0.08f, false } }));
  d.push_back(P("Organ Pad", "Pad", { T("Walsh Function", 4) }, cat({ osc(1, 0.55f), uni(1, 3, 1.0f), filt(0, 0, 102, 0.0f), kEnvPad, kPhaser, kHall }), {}));

  // ---------------- PERC (10) ----------------
  d.push_back(P("Synth Kick", "Perc", { T("Mod Sine", 0) }, cat({ osc(1, 0.9f, -12), { { "filter_1_on", 0 } }, { { "env_1_attack", 0.0f }, { "env_1_decay", 0.75f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.6f }, { "env_2_attack", 0.0f }, { "env_2_decay", 0.42f }, { "env_2_sustain", 0.0f } }, kDry, dist(0, 4) }), { { "env_2", "osc_1_transpose", 0.4f, false } }));
  d.push_back(P("Snare Crack", "Perc", { T("Noise", 1) }, cat({ osc(1, 0.7f), filt(0, 2, 96, 0.3f), kEnvHit, kRoom, kComp }), {}));
  d.push_back(P("Closed Hat", "Perc", { T("Noise", 0) }, cat({ osc(1, 0.6f, 24), filt(3, 2, 120, 0.2f), { { "env_1_attack", 0.0f }, { "env_1_decay", 0.42f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.35f } }, kDry }), {}));
  d.push_back(P("Clap Room", "Perc", { T("Noise", 4) }, cat({ osc(1, 0.65f), filt(0, 4, 92, 0.4f), kEnvHit, kHall }), {}));
  d.push_back(P("Conga Tone", "Perc", { T("Mod Sine", 1) }, cat({ osc(1, 0.8f), filt(0, 0, 96, 0.2f), kEnvHit, kRoom }), { { "env_2", "osc_1_transpose", 0.1f, false } }));
  d.push_back(P("Rim Click", "Perc", { T("Walsh Function", 7) }, cat({ osc(1, 0.7f, 12), filt(3, 2, 108, 0.3f), { { "env_1_attack", 0.0f }, { "env_1_decay", 0.38f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.3f } }, kRoom }), {}));
  d.push_back(P("Cowbell 80s", "Perc", { T("PD Square", 7) }, cat({ osc(1, 0.6f), osc(2, 0.45f, 7), filt(3, 2, 100, 0.3f), kEnvHit, kRoom }), {}));
  d.push_back(P("Tabla Bend", "Perc", { T("Mod Sine", 3) }, cat({ osc(1, 0.8f), filt(0, 0, 100, 0.2f), kEnvHit, kRoom }), { { "env_2", "osc_1_transpose", -0.12f, false } }));
  d.push_back(P("Glitch Hit", "Perc", { T("Softbe Modular", 14) }, cat({ osc(1, 0.65f), filt(3, 0, 110, 0.2f), kEnvHit, dist(4, 0), kRoom }), { { "env_2", "osc_1_wave_frame", 0.8f, false } }));
  d.push_back(P("Tom Electro", "Perc", { T("Mod Sine", 0) }, cat({ osc(1, 0.85f), { { "filter_1_on", 0 } }, kEnvPerc, kHall }), { { "env_2", "osc_1_transpose", 0.25f, false } }));

  // ---------------- PLUCK (10) ----------------
  d.push_back(P("Marimba Pluck", "Pluck", { T("Mod Sine", 5) }, cat({ osc(1, 0.75f), filt(0, 0, 104, 0.1f), kEnvPerc, kRoom }), { { "env_2", "osc_1_wave_frame", 0.5f, false } }));
  d.push_back(P("Zither", "Pluck", { T("Karplus Strong", 8) }, cat({ osc(1, 0.7f), uni(1, 2, 0.5f), filt(0, 0, 110, 0.1f), kEnvLongPluck, kHall, kChorus }), {}));
  d.push_back(P("Bright Saw Pluck", "Pluck", {}, cat({ osc(1, 0.6f), uni(1, 5, 2.0f), filt(0, 1, 76, 0.3f), kEnvPluck, kDelay8, kRoom }), { { "env_2", "filter_1_cutoff", 0.42f, false } }));
  d.push_back(P("Muted Guitar", "Pluck", { T("Karplus Strong", 5) }, cat({ osc(1, 0.75f), filt(0, 1, 86, 0.2f), kEnvHit, kRoom }), { { "velocity", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Shamisen", "Pluck", { T("PD PWM", 1) }, cat({ osc(1, 0.7f), filt(3, 2, 104, 0.4f), kEnvPluck, kRoom }), { { "env_2", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Pizzicato Strings", "Pluck", { T("OSC Sync+PWM", 1) }, cat({ osc(1, 0.65f), uni(1, 4, 1.4f), filt(0, 1, 90, 0.15f), kEnvPerc, kHall }), { { "env_2", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Bubble Pluck", "Pluck", { T("Modulo", 12) }, cat({ osc(1, 0.7f), filt(2, 1, 66, 0.6f), kEnvPluck, kDelayDot, kRoom }), { { "env_2", "filter_1_cutoff", 0.45f, false } }));
  d.push_back(P("Sitar Pluck", "Pluck", { T("Karplus Strong", 3) }, cat({ osc(1, 0.7f), warp(1, 4, 0.6f), filt(6, 0, 100, 0.4f), kEnvLongPluck, kRoom }), { { "env_2", "osc_1_distortion_amount", 0.3f, false } }));
  d.push_back(P("Glass Pluck", "Pluck", { T("FM", 12) }, cat({ osc(1, 0.65f), filt(0, 0, 116, 0.1f), kEnvPluck, kVast, kDelayDot }), { { "env_2", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Ukulele", "Pluck", { T("Karplus Strong", 6) }, cat({ osc(1, 0.75f, 12), filt(0, 1, 104, 0.15f), kEnvPluck, kRoom }), {}));

  // ---------------- RHODES (10) ----------------
  d.push_back(P("Rhodes Stage 73", "Rhodes", { T("FM", 2) }, cat({ osc(1, 0.7f), filt(0, 0, 100, 0.1f), kEnvRhodes, kRoom, lfoHz(1, 2.3f) }), { { "velocity", "osc_1_wave_frame", 0.5f, false }, { "lfo_1", "osc_1_level", 0.12f, true } }, { 1 }));
  d.push_back(P("Rhodes Phase Ballad", "Rhodes", { T("PM[Simple]", 5) }, cat({ osc(1, 0.7f), filt(0, 0, 96, 0.1f), kEnvRhodes, kPhaser, kHall }), { { "velocity", "osc_1_wave_frame", 0.45f, false } }));
  d.push_back(P("Rhodes Bright Bark", "Rhodes", { T("PM[Simple]", 9) }, cat({ osc(1, 0.7f), filt(1, 0, 104, 0.15f, 4), kEnvRhodes, dist(0, 6), kRoom }), { { "velocity", "osc_1_wave_frame", 0.6f, false }, { "velocity", "distortion_drive", 0.15f, false } }));
  d.push_back(P("Rhodes Mellow Jazz", "Rhodes", { T("Mod Sine", 6) }, cat({ osc(1, 0.75f), filt(0, 0, 90, 0.1f), kEnvRhodes, kRoom }), { { "velocity", "osc_1_wave_frame", 0.35f, false } }));
  d.push_back(P("Rhodes Chorus Wide", "Rhodes", { T("FM", 10) }, cat({ osc(1, 0.65f), uni(1, 2, 0.6f), filt(0, 0, 98, 0.1f), kEnvRhodes, kChorus, { { "chorus_dry_wet", 0.6f } }, kHall }), { { "velocity", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Rhodes Vintage Trem", "Rhodes", { T("PM[Simple]", 3) }, cat({ osc(1, 0.7f), filt(0, 0, 94, 0.1f), kEnvRhodes, kRoom, lfoHz(1, 2.7f) }), { { "velocity", "osc_1_wave_frame", 0.45f, false }, { "lfo_1", "osc_1_level", 0.3f, true } }, { 1 }));
  d.push_back(P("Rhodes Hip Hop", "Rhodes", { T("FM", 1) }, cat({ osc(1, 0.7f), filt(0, 0, 84, 0.1f), kEnvRhodes, dist(5, 0), { { "distortion_mix", 0.35f } }, kChorus, kRoom }), { { "velocity", "osc_1_wave_frame", 0.35f, false } }));
  d.push_back(P("Rhodes Echo Room", "Rhodes", { T("Phase", 4) }, cat({ osc(1, 0.7f), filt(0, 0, 98, 0.1f), kEnvRhodes, kDelayDot, kHall }), { { "velocity", "osc_1_wave_frame", 0.4f, false } }));
  d.push_back(P("Rhodes Bell Mix", "Rhodes", { T("PM[Simple]", 0), T("FM", 8) }, cat({ osc(1, 0.6f), osc(2, 0.3f, 12), filt(0, 0, 106, 0.1f), kEnvRhodes, kChorus, kHall }), { { "velocity", "osc_2_level", 0.35f, false } }));
  d.push_back(P("Rhodes Dark Velvet", "Rhodes", { T("PM[Simple]", 13) }, cat({ osc(1, 0.7f), filt(2, 0, 82, 0.15f), kEnvRhodes, kPhaser, kRoom }), { { "velocity", "filter_1_cutoff", 0.2f, false } }));

  // ---------------- STRINGS (10) ----------------
  d.push_back(P("Violin Solo", "Strings", { T("OSC Sync+PWM", 3) }, cat({ osc(1, 0.65f), uni(1, 2, 0.4f), filt(0, 1, 98, 0.15f), kEnvBow, kMonoGlide, kHall, lfoHz(1, 2.5f) }), { { "lfo_1", "osc_1_tune", 0.06f, true } }, { 1 }));
  d.push_back(P("Chamber Strings", "Strings", { T("Alpha Juno SawPWM", 1) }, cat({ osc(1, 0.55f), uni(1, 7, 2.2f), filt(0, 1, 94, 0.1f), kEnvBow, kHall }), {}));
  d.push_back(P("Tremolo Strings", "Strings", { T("OSC Sync+PWM", 9) }, cat({ osc(1, 0.55f), uni(1, 6, 2.0f), filt(0, 1, 96, 0.1f), kEnvBow, kHall, lfoTempo(1, 10) }), { { "lfo_1", "osc_1_level", 0.4f, false } }, { 1 }));
  d.push_back(P("Low Strings", "Strings", { T("Bazille", 6) }, cat({ osc(1, 0.6f, -12), uni(1, 5, 1.6f), filt(0, 1, 82, 0.1f), kEnvBow, kHall }), {}));
  d.push_back(P("Spiccato", "Strings", { T("OSC Sync+PWM", 2) }, cat({ osc(1, 0.6f), uni(1, 5, 1.8f), filt(0, 1, 92, 0.1f), kEnvHit, kHall }), {}));
  d.push_back(P("Analog String Machine", "Strings", { T("Korg Analog  Synth PhaseShift2", 2) }, cat({ osc(1, 0.55f), uni(1, 7, 2.6f), filt(0, 0, 100, 0.0f), kEnvPad, kChorus, kPhaser, kHall }), {}));
  d.push_back(P("Cinematic Swell", "Strings", { T("Alpha Juno SawPWM", 2) }, cat({ osc(1, 0.55f), uni(1, 7, 2.4f), filt(0, 1, 90, 0.1f), kEnvSwell, kVast }), { { "env_2", "filter_1_cutoff", 0.2f, false } }));
  d.push_back(P("Viola Warm", "Strings", { T("Bazille", 3) }, cat({ osc(1, 0.65f), uni(1, 2, 0.5f), filt(0, 1, 90, 0.15f), kEnvBow, kMono, kHall, lfoHz(1, 2.4f) }), { { "lfo_1", "osc_1_tune", 0.05f, true } }, { 1 }));
  d.push_back(P("Ensemble 78", "Strings", { T("PD PWM", 0) }, cat({ osc(1, 0.55f), uni(1, 6, 2.2f), filt(0, 0, 98, 0.0f), kEnvPad, kChorus, { { "chorus_dry_wet", 0.6f } }, kHall }), {}));
  d.push_back(P("Glass Strings", "Strings", { T("Additive Synth", 8) }, cat({ osc(1, 0.55f), uni(1, 6, 1.8f), filt(0, 1, 104, 0.1f), kEnvBow, kVast }), {}));

  // ---------------- VOCAL (10) ----------------
  d.push_back(P("Ooh Choir", "Vocal", { T("GMa Vowel", 0) }, cat({ osc(1, 0.55f, 0, 120), uni(1, 7, 1.6f), filt(5, 0, 92, 0.25f), kEnvPad, kHall, kChorus }), {}));
  d.push_back(P("Eeh Choir", "Vocal", { T("GMa Vowel", 0) }, cat({ osc(1, 0.55f, 0, 220), uni(1, 7, 1.6f), filt(5, 0, 104, 0.25f), kEnvPad, kHall, kChorus }), {}));
  d.push_back(P("Vocoder Lead", "Vocal", { T("Vocorder", 0) }, cat({ osc(1, 0.8f, 0, 128), uni(1, 3, 0.6f), filt(0, 0, 118, 0.1f), kEnvLead, kMono, kRoom }), { { "mod_wheel", "osc_1_wave_frame", 1.0f, false } }));
  d.push_back(P("Whisper Pad", "Vocal", { T("Formant", 1) }, cat({ osc(1, 0.5f), uni(1, 5, 1.4f), filt(5, 0, 100, 0.3f), kEnvDream, kNoise, { { "sample_level", 0.2f } }, kVast, lfoHz(1, -2.0f) }), { { "lfo_1", "osc_1_wave_frame", 0.8f, false } }));
  d.push_back(P("Monk Chant", "Vocal", { T("GMa Vowel", 0) }, cat({ osc(1, 0.6f, -12, 40), uni(1, 4, 0.8f), filt(5, 0, 84, 0.3f), kEnvPad, kVast }), {}));
  d.push_back(P("Vowel Pluck", "Vocal", { T("FormantStep", 0) }, cat({ osc(1, 0.65f), filt(5, 0, 98, 0.3f), kEnvPluck, kDelayDot, kRoom }), { { "env_2", "osc_1_wave_frame", 0.8f, false } }));
  d.push_back(P("Choir Swell", "Vocal", { T("GMa Vowel", 0) }, cat({ osc(1, 0.6f, 0, 160), uni(1, 7, 1.8f), filt(0, 1, 104, 0.1f), kEnvPad, kVast, kChorus }), { { "env_2", "osc_1_wave_frame", 0.3f, false } }));
  d.push_back(P("Yah Stab", "Vocal", { T("Formant", 0) }, cat({ osc(1, 0.65f), uni(1, 3, 1.0f), filt(5, 0, 100, 0.3f), kEnvPerc, kRoom }), { { "env_2", "osc_1_wave_frame", 0.7f, false } }));
  d.push_back(P("Siren Voice", "Vocal", { T("GMa Vowel", 0) }, cat({ osc(1, 0.6f), filt(5, 0, 100, 0.3f), kEnvSoftLead, kMonoGlide, { { "portamento_time", -2.0f } }, kVast, lfoHz(1, 2.3f) }), { { "lfo_1", "osc_1_tune", 0.05f, true }, { "mod_wheel", "osc_1_wave_frame", 0.8f, false } }, { 1 }));
  d.push_back(P("Formant Sweep Voice", "Vocal", { T("FormantStep", 0) }, cat({ osc(1, 0.55f), uni(1, 4, 1.2f), filt(5, 0, 96, 0.3f), kEnvPad, kHall, lfoHz(1, -1.0f) }), { { "lfo_1", "osc_1_wave_frame", 1.0f, false } }));
}


// ---------------- 1.2: RAGE, EDM and R&B sets (5 per sound) ----------------
// "@n" as a table = Warpline's own built-in table n (VitalCore::Table: 2 sine, 3 triangle, 4 saw, 5 square, 6 PWM, ...).
static const C kEnv808 = { { "env_1_attack", 0.0f }, { "env_1_decay", 1.35f }, { "env_1_sustain", 0.55f }, { "env_1_release", 0.55f },
                           { "env_2_attack", 0.0f }, { "env_2_decay", 0.32f }, { "env_2_sustain", 0.0f } };
static const C kEnvBellLong = { { "env_1_attack", 0.0f }, { "env_1_decay", 1.45f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.3f },
                                { "env_2_attack", 0.0f }, { "env_2_decay", 0.8f }, { "env_2_sustain", 0.0f } };
static const C kEnvChoir = { { "env_1_attack", 0.9f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.35f },
                             { "env_2_attack", 1.0f }, { "env_2_decay", 1.4f }, { "env_2_sustain", 0.7f } };
static const C kEnvFestival = { { "env_1_attack", 0.05f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.7f },
                                { "env_2_attack", 0.0f }, { "env_2_decay", 0.75f }, { "env_2_sustain", 0.2f } };
static const C kSlide808 = { { "polyphony", 1 }, { "legato", 0 }, { "portamento_time", -3.5f }, { "portamento_slope", 0.0f } };
static const Mod kDrop808 = { "env_2", "osc_1_transpose", 0.11f, false };   // the "knock": about an octave down at the start
// Rage sound: OTT-style multiband compression (Vital's compressor defaults, fully wet, a bit harder), wide unison,
// poly for chords, an instant envelope that holds, and a big but short-ish reverb.
static const C kRageBus = { { "compressor_on", 1 }, { "compressor_mix", 1.0f }, { "compressor_band_gain", 12.0f },
                            { "compressor_high_gain", 12.0f } };
static const C kRageSpread = { { "osc_1_stereo_spread", 1.0f }, { "osc_2_stereo_spread", 1.0f }, { "osc_1_unison_blend", 0.9f },
                               { "osc_2_unison_blend", 0.9f } };
static const C kRagePoly = { { "polyphony", 8 }, { "legato", 0 }, { "portamento_time", -10.0f } };
static const C kEnvRageChord = { { "env_1_attack", 0.0f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.55f },
                                 { "env_2_attack", 0.0f }, { "env_2_decay", 0.8f }, { "env_2_sustain", 0.4f } };
static const C kRageVerb = { { "reverb_dry_wet", 0.3f }, { "reverb_size", 0.8f }, { "reverb_decay_time", 0.9f } };
// Punch: env 2 snaps the filter open and the pitch up a little for the first few milliseconds.
static const C kEnvPunch = { { "env_1_attack", 0.0f }, { "env_1_decay", 0.9f }, { "env_1_sustain", 0.85f }, { "env_1_release", 0.5f },
                             { "env_2_attack", 0.0f }, { "env_2_decay", 0.36f }, { "env_2_sustain", 0.0f } };
static const C kEnvPunchPluck = { { "env_1_attack", 0.0f }, { "env_1_decay", 0.72f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.5f },
                                  { "env_2_attack", 0.0f }, { "env_2_decay", 0.42f }, { "env_2_sustain", 0.0f } };
static const Mod kPunchFilter = { "env_2", "filter_1_cutoff", 0.18f, false };
// 1.3: a clean sine sub two octaves down on OSC 3, straight to the output (no filter, clip or OTT), so leads and
// chords carry a heavy low end; and "coins": white noise through a resonant comb on Filter 2, chopped by a random
// sample-and-hold at 1/32 and re-pitched every step, so it clinks and jingles behind the sound.
static const C kSub = { { "osc_3_on", 1 }, { "osc_3_level", 0.5f }, { "osc_3_transpose", -24.0f }, { "osc_3_destination", 4 },
                        { "osc_3_unison_voices", 1 } };
static const C kSubLight = { { "osc_3_on", 1 }, { "osc_3_level", 0.34f }, { "osc_3_transpose", -24.0f }, { "osc_3_destination", 4 },
                             { "osc_3_unison_voices", 1 } };
static const C kCoins = { { "sample_on", 1 }, { "sample_level", 0.0f }, { "sample_destination", 1 }, { "sample_random_phase", 1 },
                          { "filter_2_on", 1 }, { "filter_2_model", 6 }, { "filter_2_style", 0 }, { "filter_2_cutoff", 110.0f },
                          { "filter_2_resonance", 0.8f }, { "filter_2_mix", 1.0f },
                          { "random_1_style", 1 }, { "random_1_sync", 1 }, { "random_1_tempo", 11 },
                          { "random_2_style", 1 }, { "random_2_sync", 1 }, { "random_2_tempo", 11 } };
static const Mod kCoinsLevel = { "random_1", "sample_level", 0.36f, false };
static const Mod kCoinsPitch = { "random_2", "filter_2_cutoff", 0.16f, false };
static const Mod kPunchSnap = { "env_2", "osc_1_transpose", 0.04f, false };   // about 4 semitones down into the note

static void addMore4(std::vector<PatchDef>& d) {
  // ---- RAGE (1.3 rework): wide detuned saws through soft clip and OTT-style multiband compression (kRageBus),
  //      poly so they play chords; 808s clipped hard; bells and pads distorted. Plus the supersaw chord set.
  // ---- RAGE: supersaw chords (8)
  d.push_back({ "Rage Supersaw Chords", "Pad", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 5.8f), osc(2, 0.25f, 12), uni(2, 7, 4.8f), kRageSpread, filt(0, 0, 128, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb }), {} });
  d.push_back({ "Opium Chords", "Pad", { "@4", "@5", "@2" },
    cat({ kSub, kCoins, osc(1, 0.55f), uni(1, 9, 6.4f), osc(2, 0.3f, 12), uni(2, 7, 5.4f), kRageSpread, filt(0, 0, 116, 0.15f, 6), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 13), kChorus, kVast }), { kCoinsLevel, kCoinsPitch } });
  d.push_back({ "Distorted Chord Stab", "Pluck", { "@4", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 6.0f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 112, 0.1f),
          { { "env_1_attack", 0.0f }, { "env_1_decay", 0.85f }, { "env_1_sustain", 0.15f }, { "env_1_release", 0.45f },
            { "env_2_attack", 0.0f }, { "env_2_decay", 0.55f }, { "env_2_sustain", 0.0f } },
          kRagePoly, kRageBus, dist(0, 14), kRageVerb, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.12f, false } } });
  d.push_back({ "Rage Gate Chords", "Pad", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 5.6f), osc(2, 0.25f, 12), uni(2, 7, 4.6f), kRageSpread, filt(0, 0, 120, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb, lfoTempo(1, 10) }),
    { { "lfo_1", "osc_1_level", 0.55f, false }, { "lfo_1", "osc_2_level", 0.35f, false } }, { 4 } });
  d.push_back({ "Hyper Detune Chords", "Pad", { "@4", "@9", "@2" },
    cat({ kSub, kCoins, osc(1, 0.55f), uni(1, 9, 8.2f), osc(2, 0.3f, 12, 90), uni(2, 7, 6.5f), kRageSpread, filt(0, 0, 124, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 11), kRageVerb }), { kCoinsLevel, kCoinsPitch } });
  d.push_back({ "Bitcrush Chords", "Pad", { "@4", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 6.0f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 118, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(4, 0), { { "distortion_mix", 0.3f } }, kRageVerb }), {} });
  d.push_back({ "Cloud Rage Chords", "Pad", { "@4", "@4", "@2" },
    cat({ kSubLight, kCoins, osc(1, 0.55f), uni(1, 9, 6.2f), osc(2, 0.25f, 12), uni(2, 7, 5.2f), kRageSpread, filt(0, 0, 116, 0.0f),
          { { "env_1_attack", 0.45f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.0f },
            { "env_2_attack", 0.0f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 1.0f } },
          kRagePoly, kRageBus, dist(0, 10), kShimmer }), { kCoinsLevel, kCoinsPitch } });
  d.push_back({ "Rage Chord Pluck", "Pluck", { "@4", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 5.8f), osc(2, 0.25f, 12), uni(2, 5, 3.2f), kRageSpread, filt(0, 0, 112, 0.15f), kEnvPluck,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  // ---- RAGE / SUPERSAW: leads with punch (6) - a quick pitch snap and filter hit on the attack, then wide and loud
  d.push_back({ "Rage Anthem Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 6.2f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 118, 0.1f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb, kDelay16 }), { kPunchFilter, kPunchSnap } });
  d.push_back({ "Hyper Detune Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 8.0f), osc(2, 0.25f, 12), uni(2, 7, 6.5f), kRageSpread, filt(0, 0, 112, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb }), { kPunchFilter } });
  d.push_back({ "Clipped Saw Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 5.8f), osc(2, 0.3f, -12), uni(2, 5, 3.0f), kRageSpread, filt(0, 0, 120, 0.15f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 16), kRoom, kDelay16 }), { kPunchFilter, kPunchSnap } });
  d.push_back({ "Square Rage Lead", "Lead", { "@5", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 5.5f), osc(2, 0.3f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 116, 0.2f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb }), { kPunchFilter } });
  d.push_back({ "Pitch Snap Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 6.0f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 114, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb, kDelayDot }),
    { { "env_2", "osc_1_transpose", 0.13f, false }, { "env_2", "osc_2_transpose", 0.13f, false } } });
  d.push_back({ "Distorted Brass Lead", "Lead", { "@4", "@6", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 7, 5.0f), osc(2, 0.35f, 0, 80), uni(2, 5, 4.0f), kRageSpread, filt(0, 1, 96, 0.25f, 8), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb }),
    { { "env_2", "filter_1_cutoff", 0.4f, false }, kPunchSnap } });
  // ---- RAGE / SUPERSAW: plucks with punch (6)
  d.push_back({ "Punch Saw Pluck", "Pluck", { "@4", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 6.0f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 100, 0.2f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.4f, false }, kPunchSnap } });
  d.push_back({ "Rage Pluck", "Pluck", { "@4", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 6.6f), osc(2, 0.25f, 12), uni(2, 7, 5.4f), kRageSpread, filt(0, 0, 108, 0.1f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 14), kRageVerb, kDelay16 }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Snap Pluck", "Pluck", { "@4", "@5" },
    cat({ osc(1, 0.55f), uni(1, 7, 5.0f), osc(2, 0.3f, 12), uni(2, 5, 4.0f), kRageSpread, filt(0, 0, 96, 0.25f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 12), kRoom, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.45f, false }, { "env_2", "osc_1_transpose", 0.13f, false } } });
  d.push_back({ "Clipped Pluck", "Pluck", { "@4", "@4" },
    cat({ osc(1, 0.55f), uni(1, 9, 5.6f), osc(2, 0.3f, -12), filt(0, 0, 104, 0.15f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 16), kRoom }),
    { { "env_2", "filter_1_cutoff", 0.35f, false }, kPunchSnap } });
  d.push_back({ "Detuned Stab Pluck", "Pluck", { "@4", "@4" },
    cat({ osc(1, 0.55f), uni(1, 9, 7.5f), osc(2, 0.25f, 12), uni(2, 7, 6.0f), kRageSpread, filt(0, 0, 112, 0.1f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 13), kRageVerb }),
    { { "env_2", "filter_1_cutoff", 0.25f, false } } });
  d.push_back({ "Hyper Pluck", "Pluck", { "@9", "@4" },
    cat({ osc(1, 0.55f), uni(1, 7, 5.0f), osc(2, 0.25f, 12), uni(2, 7, 5.0f), kRageSpread, filt(0, 0, 118, 0.0f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb, kDelayDot }),
    { { "env_2", "osc_1_wave_frame", 0.6f, false }, kPunchSnap } });
  // ---- RAGE: punk / hyper / melodic styles (10)
  d.push_back({ "Vamp Organ", "Keys", { T("Walsh Function", 2), "@2" },
    cat({ kSubLight, osc(1, 0.6f), uni(1, 3, 0.8f), osc(2, 0.3f, -12), filt(0, 0, 112, 0.1f), kEnvRageChord, kRagePoly, kRageBus, dist(0, 13), kVast }), {} });
  d.push_back({ "Sky Arp Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSubLight, kCoins, osc(1, 0.55f), uni(1, 9, 5.4f), osc(2, 0.25f, 12), uni(2, 7, 4.6f), kRageSpread, filt(0, 0, 118, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 11), kRageVerb, kDelayDot, lfoHz(1, 2.4f) }),
    { kCoinsLevel, kCoinsPitch, { "lfo_1", "osc_1_tune", 0.02f, true }, { "lfo_1", "osc_2_tune", 0.02f, true }, kPunchFilter } , { 1 } });
  d.push_back({ "Punk Fuzz Lead", "Lead", { "@5", "@5", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 5, 3.0f), osc(2, 0.3f, -12), filt(2, 0, 112, 0.3f, 14), kEnvPunch, kRagePoly, kRageBus, dist(2, 11), kRoom }),
    { kPunchFilter, kPunchSnap } });
  d.push_back({ "Slime Reese Bass", "Bass", { "@4" },
    cat({ osc(1, 0.7f), uni(1, 7, 3.4f), filt(2, 0, 74, 0.3f, 12), kEnvBass, kMonoGlide, kDry, kRageBus, dist(0, 12), lfoHz(1, -1.8f) }),
    { { "lfo_1", "filter_1_cutoff", 0.12f, true } } });
  d.push_back({ "Chaos Screech", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, kCoins, osc(1, 0.5f), uni(1, 9, 7.2f), osc(2, 0.22f, 24), uni(2, 7, 6.0f), kRageSpread, filt(0, 0, 124, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 16), kRageVerb }), { kCoinsLevel, kCoinsPitch, kPunchSnap } });
  d.push_back({ "Gated Hyper Saw", "Pad", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 6.4f), osc(2, 0.25f, 12), uni(2, 7, 5.4f), kRageSpread, filt(0, 0, 120, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 14), kRageVerb, lfoTempo(1, 11) }),
    { { "lfo_1", "osc_1_level", 0.6f, false }, { "lfo_1", "osc_2_level", 0.4f, false } }, { 4 } });
  d.push_back({ "Overdrive Stab", "Pluck", { "@4", "@5" },
    cat({ osc(1, 0.55f), uni(1, 9, 6.0f), osc(2, 0.3f, -12), uni(2, 3, 1.5f), kRageSpread, filt(0, 0, 110, 0.15f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 16), kRoom }),
    { { "env_2", "filter_1_cutoff", 0.35f, false }, kPunchSnap } });
  d.push_back({ "Glitch Crush Lead", "Lead", { "@4", "@5", "@2" },
    cat({ kSubLight, kCoins, osc(1, 0.55f), uni(1, 7, 5.0f), osc(2, 0.3f, 12), uni(2, 5, 4.0f), kRageSpread, filt(0, 0, 116, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(5, 0), { { "distortion_mix", 0.35f } }, kRageVerb, kDelay16 }), { kCoinsLevel, kCoinsPitch, kPunchFilter } });
  d.push_back({ "Wavy Saw Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSubLight, kCoins, osc(1, 0.55f), uni(1, 9, 5.0f), osc(2, 0.25f, 12), uni(2, 7, 4.4f), kRageSpread, filt(0, 0, 112, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 10), kChorus, kRageVerb, kDelayDot, lfoHz(1, 1.6f) }),
    { kCoinsLevel, kCoinsPitch, { "lfo_1", "osc_1_tune", 0.05f, true }, { "lfo_1", "osc_2_tune", 0.05f, true } }, { 1 } });
  d.push_back({ "Dreamy Rage Pluck", "Pluck", { "@4", "@3" },
    cat({ osc(1, 0.55f), uni(1, 9, 5.2f), osc(2, 0.25f, 12), kRageSpread, filt(0, 0, 118, 0.1f), kEnvPunchPluck,
          kRagePoly, kRageBus, dist(0, 10), kShimmer, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  // ---- RAGE: experimental sub + coins (4)
  d.push_back({ "Coin Rain Chords", "Pad", { "@4", "@4", "@2" },
    cat({ kSub, kCoins, osc(1, 0.5f), uni(1, 9, 5.8f), osc(2, 0.3f, 12), uni(2, 7, 4.8f), kRageSpread, filt(0, 0, 120, 0.1f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 11), kShimmer, { { "sample_level", 0.08f } } }),
    { { "random_1", "sample_level", 0.45f, false }, kCoinsPitch } });
  d.push_back({ "Money Jingle Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, kCoins, osc(1, 0.55f), uni(1, 9, 5.4f), osc(2, 0.25f, 12), uni(2, 7, 4.6f), kRageSpread, filt(0, 0, 116, 0.0f), kEnvPunch,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb, kDelayDot, { { "random_1_tempo", 12 }, { "filter_2_cutoff", 124.0f } } }),
    { kCoinsLevel, kCoinsPitch, kPunchFilter } });
  d.push_back({ "Sub Quake Chords", "Pad", { "@4", "@2", "@2" },
    cat({ kSub, osc(1, 0.5f), uni(1, 9, 6.0f), osc(2, 0.45f, -12), { { "osc_2_destination", 4 } }, kRageSpread, filt(0, 0, 118, 0.1f),
          kEnvRageChord, kRagePoly, kRageBus, dist(0, 13), kRageVerb, { { "osc_3_level", 0.7f } } }), {} });
  d.push_back({ "Vault Chords", "Pad", { "@4", "@5", "@2" },
    cat({ kSub, kCoins, osc(1, 0.5f), uni(1, 9, 6.4f), osc(2, 0.3f, 12), uni(2, 7, 5.2f), kRageSpread, filt(0, 0, 104, 0.2f, 6),
          kEnvRageChord, kRagePoly, kRageBus, dist(0, 13), kVast, { { "filter_2_cutoff", 110.0f }, { "random_1_tempo", 10 } } }),
    { kCoinsLevel, kCoinsPitch } });
  // ---- RAGE: bells (5)
  d.push_back({ "Rage Bell", "Bell", { T("Additive Synth", 0) },
    cat({ osc(1, 0.6f), uni(1, 3, 0.6f), filt(0, 0, 128, 0.0f), kEnvBellLong, kRagePoly, kRageBus, dist(0, 11), kRageVerb, kDelayDot }),
    { { "env_2", "osc_1_wave_frame", 0.35f, false } } });
  d.push_back({ "Tonka Chime", "Bell", { T("Digitone", 1) },
    cat({ osc(1, 0.6f, 12), uni(1, 3, 0.8f), filt(0, 0, 128, 0.0f), kEnvBellLong, kRagePoly, kRageBus, dist(0, 10), kShimmer, kDelay8 }),
    { { "env_2", "osc_1_wave_frame", 0.45f, false } } });
  d.push_back({ "Lux Toll", "Bell", { T("PM[Simple]", 4) },
    cat({ osc(1, 0.6f), osc(2, 0.3f, 19), filt(0, 0, 114, 0.1f), kEnvBellLong, kRagePoly, kRageBus, dist(0, 12), kVast }),
    { { "env_2", "osc_2_level", 0.3f, false }, { "lfo_1", "osc_1_tune", 0.03f, true } } });
  d.push_back({ "Ice Toll", "Bell", { T("FM", 12) },
    cat({ osc(1, 0.6f, 12), uni(1, 3, 0.5f), filt(0, 0, 118, 0.0f), kEnvBellLong, kRagePoly, kRageBus, dist(4, 0), { { "distortion_mix", 0.4f } },
          kRageVerb, kDelayDot }),
    { { "env_2", "osc_1_wave_frame", 0.3f, false } } });
  d.push_back({ "Wraith Bell", "Bell", { T("Additive Synth", 7) },
    cat({ osc(1, 0.6f), uni(1, 5, 1.4f), kRageSpread, filt(0, 0, 112, 0.0f), kEnvBellLong, kRagePoly, kRageBus, dist(3, 6), kVast, lfoHz(1, 1.2f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.1f, true } }, { 1 } });
  // ---- RAGE: 808s (5): clipped hard, with the knock
  d.push_back({ "Rage 808", "Bass", { "@2" },
    cat({ osc(1, 0.8f), filt(0, 0, 120, 0.0f), kEnv808, kSlide808, kDry, kRageBus, dist(0, 14) }), { kDrop808 } });
  d.push_back({ "Clip 808", "Bass", { "@2" },
    cat({ osc(1, 0.8f), filt(0, 0, 112, 0.0f), kEnv808, kSlide808, kDry, kRageBus, dist(1, 16) }), { kDrop808 } });
  d.push_back({ "Fold 808", "Bass", { "@2" },
    cat({ osc(1, 0.8f), warp(1, 6, 0.35f), filt(0, 0, 110, 0.1f), kEnv808, kSlide808, kDry, kRageBus, dist(3, 8) }),
    { kDrop808, { "velocity", "osc_1_distortion_amount", 0.2f, false } } });
  d.push_back({ "Sub Knock 808", "Bass", { "@2" },
    cat({ osc(1, 0.85f), filt(0, 0, 100, 0.0f), kEnv808, kSlide808, kDry, kRageBus, dist(0, 9) }),
    { { "env_2", "osc_1_transpose", 0.16f, false } } });
  d.push_back({ "Growl 808", "Bass", { "@2", "@4" },
    cat({ osc(1, 0.75f), osc(2, 0.3f, 12), uni(2, 3, 1.0f), filt(2, 0, 92, 0.3f, 10), kEnv808, kSlide808, kDry, kRageBus, dist(0, 13) }),
    { kDrop808, { "env_2", "filter_1_cutoff", 0.3f, false } } });
  // ---- RAGE: leads (5): poly supersaws (play them as chords too), the two glide leads stay mono
  d.push_back({ "Rage Saw Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 4.4f), osc(2, 0.25f, 12), uni(2, 7, 3.6f), kRageSpread, filt(0, 0, 128, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb, kDelay16 }), {} });
  d.push_back({ "Siren Wail", "Lead", { "@6", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.6f, 0, 120), uni(1, 7, 3.4f), osc(2, 0.3f, 12), uni(2, 5, 2.8f), kRageSpread, filt(0, 0, 118, 0.0f), kEnvLead,
          kMonoGlide, kRageBus, dist(0, 12), kRageVerb, lfoHz(1, 2.2f) }),
    { { "lfo_1", "osc_1_tune", 0.06f, true }, { "env_2", "osc_1_wave_frame", 0.3f, false } }, { 1 } });
  d.push_back({ "Dive Lead", "Lead", { "@4", "@4", "@2" },
    cat({ kSub, osc(1, 0.55f), uni(1, 9, 4.8f), osc(2, 0.25f, 12), uni(2, 7, 4.0f), kRageSpread, filt(0, 0, 120, 0.0f), kEnvRageChord,
          kRagePoly, kRageBus, dist(0, 12), kRageVerb }),
    { { "env_2", "osc_1_transpose", 0.06f, false }, { "env_2", "osc_2_transpose", 0.06f, false } } });
  d.push_back({ "Fuzz Synth Lead", "Lead", { "@5", "@4", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 7, 4.0f), osc(2, 0.25f, 12), uni(2, 7, 3.6f), kRageSpread, filt(2, 0, 120, 0.2f, 10), kEnvRageChord,
          kRagePoly, kRageBus, dist(2, 9), kRageVerb, kDelay16 }), {} });
  d.push_back({ "Hyper Glide Lead", "Lead", { "@4", "@9", "@2" },
    cat({ kSubLight, osc(1, 0.55f), uni(1, 9, 5.0f), osc(2, 0.35f, 12, 60), uni(2, 5, 3.0f), kRageSpread, filt(0, 0, 128, 0.0f), kEnvLead,
          kMonoGlide, kRageBus, dist(0, 12), kRageVerb }),
    { { "env_2", "osc_2_wave_frame", 0.5f, false } } });
  // ---- RAGE: pads (5)
  d.push_back({ "Dark Choir", "Pad", { T("GMa Vowel", 0) },
    cat({ osc(1, 0.6f, 0, 60), uni(1, 7, 2.4f), kRageSpread, filt(0, 0, 96, 0.15f), kEnvChoir, kRageBus, dist(0, 9), kVast, lfoHz(1, -2.0f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.35f, false } } });
  d.push_back({ "Void Organ", "Pad", { T("Walsh Function", 2) },
    cat({ osc(1, 0.6f), uni(1, 3, 0.6f), osc(2, 0.3f, -12), filt(0, 0, 102, 0.1f), kEnvChoir, kRageBus, dist(0, 10), kVast }), {} });
  d.push_back({ "Ghost Strings", "Pad", { "@4", "@4" },
    cat({ osc(1, 0.55f), uni(1, 9, 4.0f), osc(2, 0.3f, 12), uni(2, 5, 3.0f), kRageSpread, filt(0, 0, 100, 0.1f), kEnvChoir,
          kRageBus, dist(0, 9), kChorus, kVast, lfoHz(1, -1.6f) }),
    { { "lfo_1", "filter_1_cutoff", 0.08f, true } } });
  d.push_back({ "Haze Vox Pad", "Pad", { T("Formant", 2) },
    cat({ osc(1, 0.6f), uni(1, 7, 2.8f), kRageSpread, filt(5, 0, 104, 0.2f), kEnvChoir, kRageBus, dist(5, 0), { { "distortion_mix", 0.35f } },
          kVast, lfoHz(1, -2.4f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.6f, false } } });
  d.push_back({ "Night Swell", "Pad", { "@7", "@4" },
    cat({ osc(1, 0.55f), uni(1, 7, 3.4f), osc(2, 0.3f, 12), uni(2, 5, 3.0f), kRageSpread, filt(0, 0, 110, 0.15f), kEnvSwell,
          kRageBus, dist(0, 10), kVast }),
    { { "env_2", "osc_1_wave_frame", 0.7f, false } } });

  // ---- EDM: leads (5)
  d.push_back({ "Festival Supersaw", "Lead", { "@4", "@4" },
    cat({ osc(1, 0.55f), uni(1, 9, 3.8f), osc(2, 0.35f, 12), uni(2, 5, 3.0f), filt(0, 1, 116, 0.1f), kEnvFestival, kHall, kDelayDot }), {} });
  d.push_back({ "Big Room Lead", "Lead", { "@5", "@4" },
    cat({ osc(1, 0.5f), uni(1, 7, 2.6f), osc(2, 0.4f, -12), uni(2, 4, 2.0f), filt(0, 1, 110, 0.2f), kEnvFestival, dist(0, 6), kRoom }),
    { { "env_2", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Uplift Trance Lead", "Lead", { "@4" },
    cat({ osc(1, 0.6f), uni(1, 7, 3.0f), filt(0, 1, 92, 0.25f), kEnvLead, kHall, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.3f, false } } });
  d.push_back({ "Future Bass Saw", "Lead", { "@4" },
    cat({ osc(1, 0.6f), uni(1, 8, 3.4f), filt(0, 1, 104, 0.15f), kEnvFestival, kChorus, kHall, lfoTempo(1, 7) }),
    { { "lfo_1", "osc_1_level", 0.3f, false } }, { 4 } });
  d.push_back({ "Hardstyle Screech", "Lead", { "@9" },
    cat({ osc(1, 0.6f, 0, 140), uni(1, 5, 2.4f), filt(0, 1, 100, 0.3f), kEnvFestival, kMonoGlide, dist(1, 14), kRoom }),
    { { "env_2", "osc_1_wave_frame", 0.25f, false } } });
  // ---- EDM: basses (5)
  d.push_back({ "Riddim Growl", "Bass", { T("Hardcore FM", 1) },
    cat({ osc(1, 0.7f), filt(1, 0, 84, 0.45f, 8), kEnvBass, kMono, kDry, dist(2, 10), lfoTempo(1, 10) }),
    { { "lfo_1", "osc_1_wave_frame", 0.7f, false }, { "lfo_1", "filter_1_cutoff", 0.25f, false } }, { 3 } });
  d.push_back({ "Bounce Bass", "Bass", { "@5" },
    cat({ osc(1, 0.7f), osc(2, 0.4f, -12), filt(0, 0, 70, 0.3f), kEnvPluck, kMono, kDry, kComp }),
    { { "env_2", "filter_1_cutoff", 0.4f, false } } });
  d.push_back({ "Electro Reese", "Bass", { "@4" },
    cat({ osc(1, 0.65f), uni(1, 6, 2.2f), filt(2, 1, 74, 0.25f, 6), kEnvBass, kMono, kDry, dist(0, 6), lfoHz(1, -1.8f) }),
    { { "lfo_1", "filter_1_cutoff", 0.15f, true } } });
  d.push_back({ "Dub Wobble", "Bass", { T("FormantStep", 1) },
    cat({ osc(1, 0.7f), osc(2, 0.4f, -12), filt(1, 1, 70, 0.5f, 6), kEnvBass, kMono, kDry, dist(0, 7), lfoTempo(1, 9) }),
    { { "lfo_1", "filter_1_cutoff", 0.4f, false }, { "lfo_1", "osc_1_wave_frame", 0.5f, false } }, { 1 } });
  d.push_back({ "House Pluck Bass", "Bass", { "@8" },
    cat({ osc(1, 0.7f, 0, 80), filt(0, 0, 76, 0.25f), kEnvPluck, kMono, kRoom }),
    { { "env_2", "filter_1_cutoff", 0.35f, false }, { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  // ---- EDM: plucks (5)
  d.push_back({ "Drop Pluck", "Pluck", { "@4" },
    cat({ osc(1, 0.6f), uni(1, 6, 2.6f), filt(0, 0, 70, 0.2f), kEnvPluck, kChorus, kHall, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.45f, false } } });
  d.push_back({ "Island Pluck", "Pluck", { T("Mod Sine", 3) },
    cat({ osc(1, 0.7f, 12), filt(0, 0, 100, 0.1f), kEnvPluck, kRoom, kDelay8 }),
    { { "env_2", "osc_1_wave_frame", 0.4f, false } } });
  d.push_back({ "Progressive Pluck", "Pluck", { "@5", "@4" },
    cat({ osc(1, 0.55f), osc(2, 0.4f, 12), uni(2, 3, 1.2f), filt(0, 0, 66, 0.3f), kEnvPluck, kHall, kDelayDot }),
    { { "env_2", "filter_1_cutoff", 0.5f, false } } });
  d.push_back({ "Chord Stab", "Pluck", { "@4" },
    cat({ osc(1, 0.6f), uni(1, 7, 3.0f), filt(0, 0, 80, 0.15f), { { "env_1_attack", 0.0f }, { "env_1_decay", 0.7f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.5f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.5f }, { "env_2_sustain", 0.0f } }, kHall, kComp }),
    { { "env_2", "filter_1_cutoff", 0.35f, false } } });
  d.push_back({ "Sync Pluck", "Pluck", { "@9" },
    cat({ osc(1, 0.6f), uni(1, 3, 1.4f), filt(0, 0, 108, 0.1f), kEnvPluck, kRoom, kDelay16 }),
    { { "env_2", "osc_1_wave_frame", 0.8f, false } } });
  // ---- EDM: pads (5)
  d.push_back({ "Euphoric Pad", "Pad", { "@4", "@4" },
    cat({ osc(1, 0.5f), uni(1, 8, 3.2f), osc(2, 0.35f, 12), uni(2, 5, 2.6f), filt(0, 0, 96, 0.1f), kEnvPad, kChorus, kVast }), {} });
  d.push_back({ "Sidechain Pad", "Pad", { "@4" },
    cat({ osc(1, 0.55f), uni(1, 7, 3.0f), filt(0, 0, 92, 0.1f), kEnvPad, kHall, lfoTempo(1, 7) }),
    { { "lfo_1", "osc_1_level", 0.55f, false } }, { 4 } });
  d.push_back({ "Breakdown Pad", "Pad", { "@7" },
    cat({ osc(1, 0.55f, 0, 160), uni(1, 6, 2.4f), filt(0, 0, 84, 0.1f), kEnvSwell, kShimmer }),
    { { "env_2", "filter_1_cutoff", 0.25f, false } } });
  d.push_back({ "PWM Air Pad", "Pad", { "@6" },
    cat({ osc(1, 0.6f), uni(1, 4, 1.8f), filt(0, 0, 90, 0.1f), kEnvPad, kChorus, kVast, lfoHz(1, -1.4f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.5f, false } }, { 1 } });
  d.push_back({ "Filter Rise Pad", "Pad", { "@4" },
    cat({ osc(1, 0.55f), uni(1, 7, 3.0f), filt(0, 0, 50, 0.3f), kEnvSwell, kHall }),
    { { "env_2", "filter_1_cutoff", 0.6f, false } } });

  // ---- R&B: Rhodes (5)
  d.push_back({ "Silk Rhodes", "Rhodes", { T("FM", 1) },
    cat({ osc(1, 0.7f), filt(0, 0, 92, 0.1f), kEnvRhodes, kChorus, kRoom, lfoHz(1, 2.2f) }),
    { { "velocity", "osc_1_wave_frame", 0.4f, false }, { "lfo_1", "osc_1_level", 0.12f, true } }, { 1 } });
  d.push_back({ "Late Night Rhodes", "Rhodes", { T("PM[Simple]", 2) },
    cat({ osc(1, 0.7f), filt(0, 0, 84, 0.1f), kEnvRhodes, kPhaser, kHall, dist(0, 2) }),
    { { "velocity", "osc_1_wave_frame", 0.45f, false } } });
  d.push_back({ "Slow Jam Rhodes", "Rhodes", { T("Phase", 5) },
    cat({ osc(1, 0.7f), uni(1, 2, 0.3f), filt(0, 0, 88, 0.1f), kEnvRhodes, kChorus, kHall, lfoHz(1, 2.6f) }),
    { { "velocity", "osc_1_wave_frame", 0.35f, false }, { "lfo_1", "osc_1_level", 0.2f, true } }, { 1 } });
  d.push_back({ "Velvet Tines", "Rhodes", { T("Digitone", 1), "@2" },
    cat({ osc(1, 0.55f), osc(2, 0.35f), filt(0, 0, 96, 0.1f), kEnvRhodes, kChorus, kRoom }),
    { { "velocity", "osc_1_wave_frame", 0.5f, false } } });
  d.push_back({ "Tape Soul Rhodes", "Rhodes", { T("FM", 4) },
    cat({ osc(1, 0.7f), filt(0, 0, 80, 0.1f), kEnvRhodes, dist(5, 0), { { "distortion_mix", 0.25f } }, kChorus, kRoom, lfoHz(1, 0.5f) }),
    { { "velocity", "osc_1_wave_frame", 0.35f, false }, { "lfo_1", "osc_1_tune", 0.04f, true } } });
  // ---- R&B: pads (5)
  d.push_back({ "Satin Pad", "Pad", { "@3" },
    cat({ osc(1, 0.6f), uni(1, 4, 1.4f), filt(0, 0, 86, 0.1f), kEnvPad, kChorus, kHall }), {} });
  d.push_back({ "Quiet Storm Pad", "Pad", { "@6" },
    cat({ osc(1, 0.55f), uni(1, 5, 1.8f), filt(0, 0, 78, 0.1f), kEnvPad, kChorus, kVast, lfoHz(1, -2.2f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.4f, false } }, { 1 } });
  d.push_back({ "Neo Soul Strings", "Pad", { "@4" },
    cat({ osc(1, 0.55f), uni(1, 6, 2.0f), filt(0, 0, 76, 0.1f), kEnvPad, kChorus, kHall }),
    { { "velocity", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "Warm Vocal Pad", "Pad", { T("GMa Vowel", 0) },
    cat({ osc(1, 0.55f, 0, 30), uni(1, 3, 1.0f), filt(0, 0, 90, 0.1f), kEnvPad, kChorus, kHall, lfoHz(1, -2.6f) }),
    { { "lfo_1", "osc_1_wave_frame", 0.25f, false } } });
  d.push_back({ "Harmonic Bloom", "Pad", { "@7" },
    cat({ osc(1, 0.55f), uni(1, 4, 1.4f), filt(0, 0, 88, 0.1f), kEnvSwell, kChorus, kVast }),
    { { "env_2", "osc_1_wave_frame", 0.55f, false } } });
  // ---- R&B: leads (5)
  d.push_back({ "Falsetto Whistle", "Lead", { "@2" },
    cat({ osc(1, 0.7f, 12), filt(0, 0, 120, 0.0f), kEnvSoftLead, kMonoGlide, kHall, kDelayDot, lfoHz(1, 2.4f) }),
    { { "lfo_1", "osc_1_tune", 0.03f, true } }, { 1 } });
  d.push_back({ "Smooth Sine Lead", "Lead", { "@2", "@3" },
    cat({ osc(1, 0.6f), osc(2, 0.25f, 12), filt(0, 0, 110, 0.0f), kEnvSoftLead, kMonoGlide, kChorus, kHall }), {} });
  d.push_back({ "Talk Box Lead", "Lead", { T("Formant", 0) },
    cat({ osc(1, 0.65f), filt(5, 0, 92, 0.3f), kEnvSoftLead, kMonoGlide, kRoom }),
    { { "mod_wheel", "osc_1_wave_frame", 0.7f, false }, { "env_2", "osc_1_wave_frame", 0.3f, false } } });
  d.push_back({ "Soft Square Lead", "Lead", { "@5" },
    cat({ osc(1, 0.6f), filt(0, 0, 76, 0.1f), kEnvSoftLead, kMonoGlide, kChorus, kHall }),
    { { "velocity", "filter_1_cutoff", 0.15f, false } } });
  d.push_back({ "Glide Pulse Lead", "Lead", { "@6" },
    cat({ osc(1, 0.6f, 0, 90), uni(1, 2, 0.4f), filt(0, 0, 90, 0.1f), kEnvSoftLead, kMonoGlide, kHall, kDelay8 }),
    { { "lfo_1", "osc_1_wave_frame", 0.2f, true } }, { 1 } });
  // ---- R&B: basses (5)
  d.push_back({ "Smooth Sub", "Bass", { "@2" },
    cat({ osc(1, 0.85f), filt(0, 0, 70, 0.0f), kEnvBass, kMonoGlide, kDry, dist(0, 2) }), {} });
  d.push_back({ "Funk Synth Bass", "Bass", { "@5" },
    cat({ osc(1, 0.7f), filt(0, 0, 62, 0.4f), kEnvPluck, kMono, kDry, kComp }),
    { { "env_2", "filter_1_cutoff", 0.35f, false }, { "velocity", "filter_1_cutoff", 0.1f, false } } });
  d.push_back({ "Round Moog Bass", "Bass", { "@4" },
    cat({ osc(1, 0.7f), osc(2, 0.4f, -12), filt(0, 0, 58, 0.25f, 3), kEnvBass, kMonoGlide, kDry }),
    { { "env_2", "filter_1_cutoff", 0.2f, false } } });
  d.push_back({ "Neo Soul Sub", "Bass", { "@3" },
    cat({ osc(1, 0.8f), filt(0, 0, 80, 0.0f), kEnvBass, kMonoGlide, kDry, dist(0, 3) }),
    { { "velocity", "osc_1_level", 0.15f, false } } });
  d.push_back({ "Slap Synth Bass", "Bass", { "@6" },
    cat({ osc(1, 0.7f, 0, 120), filt(0, 0, 70, 0.3f), kEnvPluck, kMono, kDry, kComp }),
    { { "env_2", "filter_1_cutoff", 0.45f, false }, { "env_2", "osc_1_wave_frame", 0.3f, false } } });
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: make_factory <open-vital-resources> <out Patches folder>\n");
    return 1;
  }
  fs::path res = fs::path(argv[1]) / "KimuraTaroFreeWavetables" / "Wavetables";
  fs::path out = argv[2];

  // Shared "house style": the 4 macros always mean TONE, MOTION, GRIT, SPACE.
  const std::vector<Mod> kMacros = {
    { "macro_control_1", "filter_1_cutoff", 0.30f, false },
    { "macro_control_2", "osc_1_wave_frame", 0.60f, false },
    { "macro_control_3", "distortion_drive", 0.45f, false },
    { "macro_control_4", "reverb_dry_wet", 0.50f, false },
  };
  const std::vector<std::pair<std::string, float>> kBase = {
    { "distortion_on", 1.0f }, { "distortion_drive", 0.0f }, { "distortion_mix", 1.0f },
    { "reverb_on", 1.0f }, { "reverb_dry_wet", 0.12f }, { "reverb_size", 0.6f },
    { "filter_1_on", 1.0f }, { "filter_1_model", 0.0f }, { "filter_1_style", 1.0f },
  };

  std::vector<PatchDef> defs = {
    { "Glass Keys", "Keys", { "Additive Synth/Add Bell.vitaltable" },
      { { "osc_1_wave_frame", 40 }, { "osc_1_unison_voices", 2 }, { "osc_1_unison_detune", 1.5f },
        { "filter_1_cutoff", 96 }, { "filter_1_resonance", 0.2f }, { "filter_1_style", 0 },
        { "env_1_attack", 0.1f }, { "env_1_decay", 1.1f }, { "env_1_sustain", 0.35f }, { "env_1_release", 0.9f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.8f }, { "env_2_sustain", 0.0f },
        { "chorus_on", 1 }, { "chorus_dry_wet", 0.3f }, { "reverb_dry_wet", 0.25f } },
      { { "env_2", "filter_1_cutoff", 0.15f, false }, { "velocity", "osc_1_level", 0.2f, false } } },

    { "Anthem Supersaw", "Lead", {},
      { { "osc_1_unison_voices", 7 }, { "osc_1_unison_detune", 3.6f }, { "osc_1_level", 0.6f },
        { "osc_2_on", 1 }, { "osc_2_unison_voices", 5 }, { "osc_2_unison_detune", 3.0f }, { "osc_2_transpose", -12 },
        { "osc_2_level", 0.45f }, { "filter_1_cutoff", 112 }, { "filter_1_resonance", 0.15f },
        { "osc_2_destination", 0 },
        { "env_1_attack", 0.15f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.75f },
        { "delay_on", 1 }, { "delay_tempo", 9 }, { "delay_feedback", 0.35f }, { "delay_dry_wet", 0.18f },
        { "reverb_dry_wet", 0.2f } },
      {} },

    { "Sub Pressure", "Bass", { "Mod Sine/" },
      { { "osc_1_wave_frame", 0 }, { "osc_1_level", 0.8f },
        { "osc_2_on", 1 }, { "osc_2_level", 0.3f }, { "osc_2_destination", 0 },
        { "filter_1_cutoff", 58 }, { "filter_1_resonance", 0.25f },
        { "env_1_attack", 0.05f }, { "env_1_decay", 1.0f }, { "env_1_sustain", 1.0f }, { "env_1_release", 0.35f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.62f }, { "env_2_sustain", 0.0f },
        { "polyphony", 1 }, { "legato", 1 }, { "portamento_time", -5.0f }, { "reverb_on", 0 } },
      { { "env_2", "filter_1_cutoff", 0.22f, false } } },

    { "Formant Wobble", "Bass", { "Formant/Formant1.vitaltable" },
      { { "osc_1_unison_voices", 3 }, { "osc_1_unison_detune", 1.2f }, { "osc_1_level", 0.75f },
        { "filter_1_cutoff", 100 }, { "filter_1_resonance", 0.35f },
        { "lfo_1_sync", 1 }, { "lfo_1_tempo", 9 },
        { "distortion_drive", 6.0f }, { "env_1_release", 0.3f }, { "reverb_on", 0 },
        { "polyphony", 2 } },
      { { "lfo_1", "osc_1_wave_frame", 0.75f, false }, { "lfo_1", "filter_1_cutoff", 0.12f, false } } },

    { "PWM Strings", "Pad", { "OSC Sync+PWM/PWM1.vitaltable" },
      { { "osc_1_unison_voices", 4 }, { "osc_1_unison_detune", 2.2f }, { "osc_1_level", 0.6f },
        { "filter_1_cutoff", 92 }, { "filter_1_resonance", 0.1f },
        { "lfo_1_sync", 0 }, { "lfo_1_frequency", -2.3f },
        { "env_1_attack", 1.0f }, { "env_1_decay", 1.2f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.25f },
        { "chorus_on", 1 }, { "chorus_dry_wet", 0.45f }, { "reverb_dry_wet", 0.35f }, { "reverb_size", 0.8f } },
      { { "lfo_1", "osc_1_wave_frame", 0.5f, false } } },

    { "Digital Bells", "Pluck", { "Digitone/DN Harm.vitaltable" },
      { { "osc_1_wave_frame", 20 }, { "osc_1_level", 0.7f }, { "filter_1_cutoff", 120 },
        { "env_1_attack", 0.0f }, { "env_1_decay", 1.15f }, { "env_1_sustain", 0.0f }, { "env_1_release", 1.1f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.9f }, { "env_2_sustain", 0.0f },
        { "delay_on", 1 }, { "delay_tempo", 9 }, { "delay_feedback", 0.4f }, { "delay_dry_wet", 0.22f },
        { "reverb_dry_wet", 0.3f } },
      { { "env_2", "osc_1_wave_frame", 0.5f, false } } },

    { "String Pluck", "Pluck", { "Karplus Strong/Karplus BP.vitaltable" },
      { { "osc_1_level", 0.75f }, { "osc_1_unison_voices", 2 }, { "osc_1_unison_detune", 0.8f },
        { "filter_1_cutoff", 104 }, { "filter_1_resonance", 0.2f },
        { "env_1_attack", 0.0f }, { "env_1_decay", 0.9f }, { "env_1_sustain", 0.0f }, { "env_1_release", 0.7f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.55f }, { "env_2_sustain", 0.0f },
        { "reverb_dry_wet", 0.18f } },
      { { "env_2", "osc_1_wave_frame", 0.6f, false }, { "env_2", "filter_1_cutoff", 0.1f, false } } },

    { "FM Growl", "Bass", { "FM Feedback/" },
      { { "osc_1_level", 0.75f }, { "filter_1_model", 1 }, { "filter_1_cutoff", 88 }, { "filter_1_resonance", 0.4f },
        { "lfo_1_sync", 1 }, { "lfo_1_tempo", 10 }, { "distortion_drive", 10.0f }, { "reverb_on", 0 },
        { "polyphony", 2 }, { "env_1_release", 0.3f } },
      { { "lfo_1", "osc_1_wave_frame", 0.6f, false }, { "lfo_1", "filter_1_cutoff", 0.15f, false } } },

    { "Folded Lead", "Lead", { "DSP Wavefolder/DSPWF.vitaltable" },
      { { "osc_1_level", 0.7f }, { "osc_1_unison_voices", 3 }, { "osc_1_unison_detune", 1.0f },
        { "filter_1_cutoff", 108 }, { "env_2_attack", 0.3f }, { "env_2_decay", 1.0f }, { "env_2_sustain", 0.4f },
        { "polyphony", 1 }, { "legato", 1 }, { "portamento_time", -4.0f },
        { "delay_on", 1 }, { "delay_tempo", 8 }, { "delay_feedback", 0.3f }, { "delay_dry_wet", 0.15f } },
      { { "env_2", "osc_1_wave_frame", 0.7f, false }, { "mod_wheel", "osc_1_unison_detune", 0.3f, false } } },

    { "Piano Air", "Keys", { "M1 Piano/M1 PianoI.vitaltable" },
      { { "osc_1_level", 0.75f }, { "filter_1_cutoff", 110 }, { "filter_1_style", 0 },
        { "env_1_attack", 0.0f }, { "env_1_decay", 1.3f }, { "env_1_sustain", 0.25f }, { "env_1_release", 0.85f },
        { "reverb_dry_wet", 0.22f } },
      { { "velocity", "filter_1_cutoff", 0.15f, false } } },

    { "Vowel Pad", "Pad", { "GMa Vowel/" },
      { { "osc_1_unison_voices", 4 }, { "osc_1_unison_detune", 1.8f }, { "osc_1_level", 0.6f },
        { "filter_1_cutoff", 100 }, { "lfo_1_sync", 0 }, { "lfo_1_frequency", -3.0f },
        { "env_1_attack", 1.05f }, { "env_1_decay", 1.2f }, { "env_1_sustain", 1.0f }, { "env_1_release", 1.3f },
        { "chorus_on", 1 }, { "chorus_dry_wet", 0.35f }, { "reverb_dry_wet", 0.4f }, { "reverb_size", 0.85f } },
      { { "lfo_1", "osc_1_wave_frame", 0.9f, false } } },

    { "PPG Sweep", "Pad", { "PPG Wave/" },
      { { "osc_1_unison_voices", 3 }, { "osc_1_unison_detune", 1.4f }, { "osc_1_level", 0.6f },
        { "filter_1_cutoff", 104 }, { "env_2_attack", 1.3f }, { "env_2_decay", 1.5f }, { "env_2_sustain", 1.0f },
        { "env_1_attack", 0.6f }, { "env_1_release", 1.2f }, { "reverb_dry_wet", 0.35f } },
      { { "env_2", "osc_1_wave_frame", 1.0f, false } } },

    { "Chip Arp", "Lead", { "PPG Wave[4bit]/" },
      { { "osc_1_level", 0.6f }, { "filter_1_on", 0 },
        { "env_1_attack", 0.0f }, { "env_1_decay", 0.7f }, { "env_1_sustain", 0.2f }, { "env_1_release", 0.4f },
        { "delay_on", 1 }, { "delay_tempo", 10 }, { "delay_feedback", 0.45f }, { "delay_dry_wet", 0.25f },
        { "reverb_dry_wet", 0.1f } },
      { { "env_1", "osc_1_wave_frame", 0.3f, false } } },

    { "Phase Keys", "Keys", { "Phase Distortion/" },
      { { "osc_1_level", 0.7f }, { "osc_1_unison_voices", 2 }, { "osc_1_unison_detune", 0.6f },
        { "filter_1_cutoff", 100 }, { "env_2_attack", 0.0f }, { "env_2_decay", 0.85f }, { "env_2_sustain", 0.1f },
        { "env_1_decay", 1.1f }, { "env_1_sustain", 0.5f }, { "env_1_release", 0.7f },
        { "chorus_on", 1 }, { "chorus_dry_wet", 0.25f } },
      { { "env_2", "osc_1_wave_frame", 0.55f, false } } },

    { "Acid Line", "Bass", {},
      { { "osc_1_level", 0.7f }, { "filter_1_model", 2 }, { "filter_1_cutoff", 60 }, { "filter_1_resonance", 0.75f },
        { "env_2_attack", 0.0f }, { "env_2_decay", 0.6f }, { "env_2_sustain", 0.0f },
        { "env_1_release", 0.25f }, { "distortion_drive", 4.0f }, { "reverb_on", 0 },
        { "polyphony", 1 }, { "legato", 1 }, { "portamento_time", -5.5f },
        { "delay_on", 1 }, { "delay_tempo", 10 }, { "delay_feedback", 0.3f }, { "delay_dry_wet", 0.12f } },
      { { "env_2", "filter_1_cutoff", 0.4f, false }, { "velocity", "filter_1_resonance", 0.15f, false } } },

    { "Additive Choir", "Pad", { "Additive Synth/Add Synth1.vitaltable" },
      { { "osc_1_unison_voices", 5 }, { "osc_1_unison_detune", 2.0f }, { "osc_1_level", 0.55f },
        { "filter_1_cutoff", 96 }, { "lfo_1_sync", 0 }, { "lfo_1_frequency", -1.5f },
        { "env_1_attack", 0.9f }, { "env_1_release", 1.2f },
        { "chorus_on", 1 }, { "chorus_dry_wet", 0.4f }, { "reverb_dry_wet", 0.4f }, { "reverb_size", 0.85f } },
      { { "lfo_1", "osc_1_wave_frame", 0.35f, false } } },
  };

  addMore(defs);
  addMore2(defs);
  addMore3(defs);
  addMore4(defs);
  {
    std::map<std::string, int> seen;
    for (const PatchDef& d : defs)
      if (seen[d.name]++) { std::fprintf(stderr, "duplicate patch name %s\n", d.name.c_str()); return 1; }
  }

  int made = 0;
  const char* only = std::getenv("FACTORY_ONLY");   // debugging: build just the patches whose name contains this
  for (const PatchDef& def : defs) {
    if (only && def.name.find(only) == std::string::npos) continue;
    VitalCore core;
    core.prepare(44100);
    for (const auto& c : kBase) core.setControl(c.first, c.second);

    for (size_t t = 0; t < def.tables.size() && t < 3; ++t) {
      if (def.tables[t].empty()) continue;
      if (def.tables[t][0] == '@') {   // Warpline's own generated table
        core.setWavetable(static_cast<int>(t), std::stoi(def.tables[t].substr(1)));
        continue;
      }
      fs::path p = res / def.tables[t];
      int pick = 0;
      size_t hash = def.tables[t].find("/#");
      if (hash != std::string::npos) {   // "Folder/#n": the n-th table in that folder
        pick = std::stoi(def.tables[t].substr(hash + 2));
        p = res / def.tables[t].substr(0, hash);
      }
      if (fs::is_directory(p)) {   // a folder: take table number "pick" (wraps around)
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(p))
          if (e.path().extension() == ".vitaltable") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        if (files.empty()) { std::fprintf(stderr, "no tables in %s\n", p.string().c_str()); return 1; }
        p = files[pick % files.size()];
      }
      if (!core.loadWavetable(static_cast<int>(t), readFile(p))) {
        std::fprintf(stderr, "could not load %s\n", p.string().c_str());
        return 1;
      }
    }
    for (const auto& c : def.controls) {
      if (!core.hasControl(c.first)) { std::fprintf(stderr, "%s: unknown control %s\n", def.name.c_str(), c.first.c_str()); return 1; }
      core.setControl(c.first, c.second);
    }
    for (size_t l = 0; l < def.lfo_shapes.size(); ++l)
      core.setLfoShape(static_cast<int>(l), def.lfo_shapes[l]);
    for (const Mod& m : kMacros)
      if (!core.addModulation(m.src, m.dst, m.amount, m.bipolar)) { std::fprintf(stderr, "macro mod failed\n"); return 1; }
    for (const Mod& m : def.mods)
      if (!core.addModulation(m.src, m.dst, m.amount, m.bipolar)) { std::fprintf(stderr, "%s: mod %s->%s failed\n", def.name.c_str(), m.src.c_str(), m.dst.c_str()); return 1; }

    // Velocity: how much softer a soft note plays (amp "velocity_track"), by type. Played instruments
    // follow velocity most; pads, drones and FX only a little, so they stay even. A patch can set its own.
    {
      static const std::map<std::string, std::pair<float, float>> kVel = {   // style -> { amp track, brightness }
        { "Keys", { 0.65f, 0.08f } }, { "Rhodes", { 0.75f, 0.0f } }, { "Pluck", { 0.6f, 0.08f } },
        { "Bell", { 0.6f, 0.05f } }, { "Perc", { 0.7f, 0.0f } }, { "Brass", { 0.5f, 0.1f } },
        { "Strings", { 0.4f, 0.05f } }, { "Bass", { 0.4f, 0.06f } }, { "Lead", { 0.35f, 0.05f } },
        { "Vocal", { 0.35f, 0.0f } }, { "Arp", { 0.3f, 0.0f } }, { "Pad", { 0.25f, 0.0f } },
        { "Dreamy", { 0.25f, 0.0f } }, { "FX", { 0.1f, 0.0f } } };
      bool own_track = false, has_vel_mod = false;
      for (const auto& c : def.controls) own_track |= c.first == "velocity_track";
      for (const Mod& m : def.mods) has_vel_mod |= m.src == "velocity";
      auto it = kVel.find(def.style);
      if (it != kVel.end()) {
        if (!own_track)
          core.setControl("velocity_track", it->second.first);
        // The compressor evens soft and hard notes out again, so played instruments go without it
        // (organs keep theirs: a real organ doesn't respond to velocity either).
        static const std::set<std::string> kPlayed = { "Keys", "Pluck", "Bell", "Perc", "Rhodes" };
        if (kPlayed.count(def.style) && def.name.find("Organ") == std::string::npos)
          core.setControl("compressor_on", 0.0f);
        // Harder notes also open the filter a little, unless the patch already uses velocity.
        // The base cutoff drops by the same amount at velocity 0.8, so a medium hit sounds as before.
        if (!has_vel_mod && it->second.second > 0.0f && core.getControl("filter_1_on") > 0.5f) {
          const vital::ValueDetails d = vital::Parameters::getDetails("filter_1_cutoff");
          float amount = it->second.second;
          float cutoff = core.getControl("filter_1_cutoff") - 0.8f * amount * (d.max - d.min);
          core.setControl("filter_1_cutoff", std::max(d.min, cutoff));
          if (!core.addModulation("velocity", "filter_1_cutoff", amount, false)) { std::fprintf(stderr, "%s: velocity mod failed\n", def.name.c_str()); return 1; }
        }
      }
    }

    // Loudness match: hold a chord (one note for mono patches) for 3.5 s and set "volume" so the
    // loudest 300 ms lands near -16 dBFS RMS. Patches that cannot get there with volume alone get
    // their oscillator levels raised first.
    double last_peak = 0.0;
    // 808s (and the 1.3 reese) play low: measure them on a low note, or the high-band compression makes them read loud.
    bool low_bass = def.name.find("808") != std::string::npos || def.name == "Slime Reese Bass";
    auto measure = [&core, &last_peak, low_bass]() {
      // Measure as the MPC plays it by default (Quality: Eco, unison and voices capped at 8),
      // including the release and effect tails.
      float oversampling = core.getControl("oversampling");
      float polyphony = core.getControl("polyphony");
      float unison[3] = { core.getControl("osc_1_unison_voices"), core.getControl("osc_2_unison_voices"),
                          core.getControl("osc_3_unison_voices") };
      core.prepare(44100);
      core.applyLimits(8, 8, 0, nullptr);
      bool mono = core.getControl("polyphony") < 1.5f;
      std::vector<int> notes = { 48, 55, 60, 64 };   // mono patches too: the last note wins, as when played
      if (low_bass) notes = { 33 };
      (void)mono;
      float l[128], r[128];
      double window = 0.0, best = 0.0;
      last_peak = 0.0;
      int in_window = 0;
      for (int b = 0; b < 1300; ++b) {
        if (b == 4) for (int n : notes) core.noteOn(n, 0.8f, 0, 0);
        if (b == 700) for (int n : notes) core.noteOff(n, 0.5f, 0, 0);
        core.render(l, r, 128);
        for (int i = 0; i < 128; ++i) {
          window += 0.5 * (l[i] * l[i] + r[i] * r[i]);
          last_peak = std::max(last_peak, static_cast<double>(std::max(std::fabs(l[i]), std::fabs(r[i]))));
        }
        in_window += 128;
        if (in_window >= 13230) {
          best = std::max(best, window / in_window);
          window = 0.0;
          in_window = 0;
        }
      }
      core.allSoundsOff();
      core.setControl("oversampling", oversampling);
      core.setControl("polyphony", polyphony);
      for (int o = 0; o < 3; ++o)
        core.setControl("osc_" + std::to_string(o + 1) + "_unison_voices", unison[o]);
      return 10.0 * std::log10(std::max(best, 1e-12));
    };
    {
      double rms_db = measure();
      double volume_db = std::sqrt(core.getControl("volume")) - 80.0;
      double target = volume_db + (-16.0 - rms_db);
      if (target > 6.0) {
        double extra = target - 6.0;
        float scale = static_cast<float>(std::pow(10.0, extra / 40.0));   // level is squared
        for (int o = 1; o <= 3; ++o) {
          std::string n = "osc_" + std::to_string(o) + "_level";
          if (core.getControl("osc_" + std::to_string(o) + "_on") > 0.5f)
            core.setControl(n, std::min(1.0f, core.getControl(n) * scale));
        }
        rms_db = measure();
        target = volume_db + (-16.0 - rms_db);
      }
      double short_db = target - 6.0;
      target = std::max(-30.0, std::min(6.0, target));
      core.setControl("volume", static_cast<float>((target + 80.0) * (target + 80.0)));
      if (short_db > 0.5) {   // still too quiet: make up the rest with the soft-clip drive stage
        core.setControl("distortion_on", 1.0f);
        core.setControl("distortion_drive",
                        static_cast<float>(std::min(9.0, core.getControl("distortion_drive") + short_db)));
      }
      double final_db = measure();
      // Short, punchy sounds: keep the peak of a 4-note chord at velocity 100 under -6 dBFS.
      constexpr double kMaxPeak = 0.5;   // -6 dBFS: leaves room for random unison phases and harder playing
      for (int pass = 0; pass < 3 && last_peak > kMaxPeak; ++pass) {
        target = std::max(-30.0, target - 20.0 * std::log10(last_peak / kMaxPeak) - 0.3);
        core.setControl("volume", static_cast<float>((target + 80.0) * (target + 80.0)));
        final_db = measure();
      }
      // Hand trims after listening tests (mono legato patches can stack louder when played as chords).
      static const std::map<std::string, double> kTrim = { { "Talk Box", -3.0 }, { "Deep Pluck", -3.0 } };
      if (kTrim.count(def.name)) {
        target += kTrim.at(def.name);
        core.setControl("volume", static_cast<float>((target + 80.0) * (target + 80.0)));
      }
      std::printf("  %-22s loudest %.1f dBFS, peak %.2f (volume %.1f dB)\n", def.name.c_str(), final_db, last_peak, target);
    }

    // Every patch name starts with its two-letter type code (LD lead, PD pad, KY keys, PL pluck, ...).
    // The plugin applies the same codes to your own patches, so keep this table in step with
    // kTypeCodes in src/Warpline.cpp.
    static const std::map<std::string, std::string> kCodes = {
      { "Arp", "AR" }, { "Bass", "BS" }, { "Bell", "BL" }, { "Brass", "BR" }, { "Dreamy", "DY" }, { "FX", "FX" },
      { "Keys", "KY" }, { "Lead", "LD" }, { "Pad", "PD" }, { "Perc", "PC" }, { "Pluck", "PL" }, { "Rhodes", "RH" },
      { "Strings", "ST" }, { "Vocal", "VX" } };
    if (!kCodes.count(def.style)) { std::fprintf(stderr, "%s: no type code for style %s\n", def.name.c_str(), def.style.c_str()); return 1; }
    const std::string full_name = kCodes.at(def.style) + " " + def.name;
    std::string text = core.savePatch(full_name, "CLVRKE", def.style,
                                      "Warpline factory patch. Wavetables: Kimura Taro free wavetables (CC0) via open-vital-resources.",
                                      { "TONE", "MOTION", "GRIT", "SPACE" });
    // Vital always stores its default noise sample (~120 KB). Leave it out when the sample
    // oscillator is off: the plugin (and Vital) fall back to the same default.
    if (core.getControl("sample_on") < 0.5f) {
      nlohmann::json j = nlohmann::json::parse(text);
      j["settings"].erase("sample");
      text = j.dump();
    }
    fs::path dir = out / def.style;
    fs::create_directories(dir);
    std::ofstream(dir / (full_name + ".vital"), std::ios::binary) << text;
    std::printf("%-18s %-6s %7zu bytes\n", def.name.c_str(), def.style.c_str(), text.size());
    made++;
  }
  std::printf("%d patches written to %s\n", made, out.string().c_str());
  return 0;
}
