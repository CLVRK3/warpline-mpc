// Warpline.cpp - the MPC plugin: wraps the Vital sound engine (VitalCore) in the SDK's mpc::Plugin.
//
// Warpline (MPC). GPLv3 (see LICENSE). Engine: Vital by Matt Tytel (https://github.com/mtytel/vital).
//
// How it fits together
//   * VitalCore owns one Vital SoundEngine. Every Vital control has a name ("filter_1_cutoff").
//   * About 80 of those controls are exposed as MPC parameters (plain value = Vital's own value), laid
//     out over six pages. The rest of a patch (modulation matrix, wavetables, LFO shapes, other
//     effects) comes from the loaded .vital patch and is kept as-is.
//   * Parameter 0 "Patch" picks a .vital file. Patches are found in Content/Patches (factory) and in
//     a "Warpline/Patches" folder at the root of the drive the plugin is on (your own). Every patch is
//     also a built-in program, so the MPC's own preset list works as a browser.
//   * Loading a patch takes 50-900 ms, so it never happens on the audio thread. A worker thread
//     pauses the audio (process() outputs silence while paused, exactly like Vital pauses its own
//     processing during a load), loads the patch into the engine, then resumes.
//   * Parameter changes from the screen, Q-Links and automation are queued as "dirty" flags and applied
//     on the audio thread at the start of the next block.

#include "QuietVital.h"
#include "mpc_plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Arp.h"
#include "Granulator.h"
#include "VitalCore.h"
#include "synth_constants.h"
#include "synth_parameters.h"

namespace fs = std::filesystem;

namespace {

// One exposed Vital control.
struct Exposed {
  const char* vital;   // Vital's control name
  const char* label;   // name on the MPC screen (about 12 characters)
};

// Order = parameter order after the plugin's own parameters. Never reorder or remove entries
// (the framework saves by id, so adding new ones at the end is safe).
const Exposed kExposed[] = {
  // F1 MAIN
  { "osc_1_wave_frame", "Osc1 WT Pos" }, { "osc_1_unison_voices", "Osc1 Unison" },
  { "osc_1_unison_detune", "Osc1 Detune" }, { "osc_1_level", "Osc1 Level" },
  { "osc_2_wave_frame", "Osc2 WT Pos" }, { "osc_2_unison_voices", "Osc2 Unison" },
  { "osc_2_unison_detune", "Osc2 Detune" }, { "osc_2_level", "Osc2 Level" },
  { "osc_3_wave_frame", "Osc3 WT Pos" }, { "osc_3_unison_voices", "Osc3 Unison" },
  { "osc_3_unison_detune", "Osc3 Detune" }, { "osc_3_level", "Osc3 Level" },
  { "filter_1_cutoff", "F1 Cutoff" }, { "filter_1_resonance", "F1 Reso" },
  { "filter_1_drive", "F1 Drive" }, { "filter_1_blend", "F1 Blend" },
  // F2 OSC
  { "osc_1_transpose", "Osc1 Transp" }, { "osc_1_tune", "Osc1 Fine" },
  { "osc_1_distortion_amount", "Osc1 Warp" }, { "osc_1_spectral_morph_amount", "Osc1 Morph" },
  { "osc_2_transpose", "Osc2 Transp" }, { "osc_2_tune", "Osc2 Fine" },
  { "osc_2_distortion_amount", "Osc2 Warp" }, { "osc_2_spectral_morph_amount", "Osc2 Morph" },
  { "osc_3_transpose", "Osc3 Transp" }, { "osc_3_tune", "Osc3 Fine" },
  { "osc_3_distortion_amount", "Osc3 Warp" }, { "osc_3_spectral_morph_amount", "Osc3 Morph" },
  { "osc_1_on", "Osc1 On" }, { "osc_2_on", "Osc2 On" }, { "osc_3_on", "Osc3 On" }, { "sample_on", "Noise/Smp On" },
  // F3 FILTER
  { "filter_1_model", "F1 Model" }, { "filter_1_style", "F1 Style" },
  { "filter_1_keytrack", "F1 Keytrack" }, { "filter_1_mix", "F1 Mix" },
  { "filter_2_cutoff", "F2 Cutoff" }, { "filter_2_resonance", "F2 Reso" },
  { "filter_2_drive", "F2 Drive" }, { "filter_2_blend", "F2 Blend" },
  { "filter_2_model", "F2 Model" }, { "filter_2_style", "F2 Style" },
  { "filter_2_keytrack", "F2 Keytrack" }, { "filter_2_mix", "F2 Mix" },
  { "filter_1_on", "F1 On" }, { "filter_2_on", "F2 On" }, { "portamento_time", "Glide" }, { "legato", "Legato" },
  // F4 ENV / MACRO
  { "env_1_attack", "Amp Attack" }, { "env_1_decay", "Amp Decay" },
  { "env_1_sustain", "Amp Sustain" }, { "env_1_release", "Amp Release" },
  { "env_2_attack", "Env2 Attack" }, { "env_2_decay", "Env2 Decay" },
  { "env_2_sustain", "Env2 Sustain" }, { "env_2_release", "Env2 Release" },
  { "env_3_attack", "Env3 Attack" }, { "env_3_decay", "Env3 Decay" },
  { "env_3_sustain", "Env3 Sustain" }, { "env_3_release", "Env3 Release" },
  { "macro_control_1", "Macro 1" }, { "macro_control_2", "Macro 2" },
  { "macro_control_3", "Macro 3" }, { "macro_control_4", "Macro 4" },
  // F5 FX
  { "distortion_on", "Dist On" }, { "distortion_type", "Dist Type" },
  { "distortion_drive", "Dist Drive" }, { "distortion_mix", "Dist Mix" },
  { "delay_on", "Delay On" }, { "delay_tempo", "Delay Time" },
  { "delay_feedback", "Delay Fdbk" }, { "delay_dry_wet", "Delay Mix" },
  { "reverb_on", "Reverb On" }, { "reverb_size", "Reverb Size" },
  { "reverb_decay_time", "Reverb Decay" }, { "reverb_dry_wet", "Reverb Mix" },
  { "chorus_on", "Chorus On" }, { "chorus_dry_wet", "Chorus Mix" },
  { "phaser_on", "Phaser On" }, { "phaser_dry_wet", "Phaser Mix" },
  // F6 PATCH (with the plugin's own parameters)
  { "volume", "Volume" },
  // Added for the custom skin (shown in the oscillator panel headers).
  { "osc_1_distortion_type", "Osc1 Warp Mode" }, { "osc_2_distortion_type", "Osc2 Warp Mode" },
  { "osc_3_distortion_type", "Osc3 Warp Mode" },
  // How much the amp follows note velocity (0 = every note the same, 100% = soft notes quiet).
  { "velocity_track", "Vel Sens" },
};
// Display parameters for Live View: the picture follows this Vital control including its modulation.
struct LiveDisplay { const char* id; const char* label; const char* vital; };
const LiveDisplay kLive[] = {
  { "osc1_live", "Osc1 WT Live", "osc_1_wave_frame" }, { "osc2_live", "Osc2 WT Live", "osc_2_wave_frame" },
  { "osc3_live", "Osc3 WT Live", "osc_3_wave_frame" }, { "f1_live", "F1 Cut Live", "filter_1_cutoff" },
  { "f2_live", "F2 Cut Live", "filter_2_cutoff" },
};
constexpr int kNumLive = sizeof(kLive) / sizeof(kLive[0]);
constexpr int kListRows = 12;      // rows of the scrollable patch list (the browser shows 12, the PATCH page the first 9)
constexpr int kListRowsV1 = 9;     // rows that existed before the browser (parameter order is permanent)
constexpr int kListPageRows = 9;   // rows the PATCH page shows; scrolling stops when its last row holds the last patch

// ---- 1.1 additions (all their parameters come after "Browser", so older projects keep their order) ----

// Vital controls added in 1.1: the mod matrix's own LFOs. The matrix calls them LFO 1-4; in the engine
// they are Vital's LFO 5-8, so they do not disturb the LFOs a patch already uses. Their settings belong
// to the matrix, so a patch load does not change them.
const Exposed kExposed2[] = {
  { "lfo_5_sync", "LFO1 Sync" }, { "lfo_5_frequency", "LFO1 Rate" }, { "lfo_5_tempo", "LFO1 Tempo" }, { "lfo_5_sync_type", "LFO1 Mode" },
  { "lfo_6_sync", "LFO2 Sync" }, { "lfo_6_frequency", "LFO2 Rate" }, { "lfo_6_tempo", "LFO2 Tempo" }, { "lfo_6_sync_type", "LFO2 Mode" },
  { "lfo_7_sync", "LFO3 Sync" }, { "lfo_7_frequency", "LFO3 Rate" }, { "lfo_7_tempo", "LFO3 Tempo" }, { "lfo_7_sync_type", "LFO3 Mode" },
  { "lfo_8_sync", "LFO4 Sync" }, { "lfo_8_frequency", "LFO4 Rate" }, { "lfo_8_tempo", "LFO4 Tempo" }, { "lfo_8_sync_type", "LFO4 Mode" },
};
constexpr int kNumExposed2 = sizeof(kExposed2) / sizeof(kExposed2[0]);
constexpr int kNumMatrixLfos = 4;
constexpr int kFirstMatrixLfo = 4;   // Vital LFO index (0-based) of the matrix's LFO 1

// Mod matrix sources and destinations: screen name, Vital name.
struct Named { const char* label; const char* vital; };
const Named kModSources[] = {
  { "Off", "" }, { "Env 1 (Amp)", "env_1" }, { "Env 2", "env_2" }, { "Env 3", "env_3" },
  { "LFO 1", "lfo_5" }, { "LFO 2", "lfo_6" }, { "LFO 3", "lfo_7" }, { "LFO 4", "lfo_8" },
  { "Velocity", "velocity" }, { "Mod Wheel", "mod_wheel" }, { "Aftertouch", "aftertouch" }, { "Random", "random" },
  { "Macro 1", "macro_control_1" }, { "Macro 2", "macro_control_2" }, { "Macro 3", "macro_control_3" }, { "Macro 4", "macro_control_4" },
};
constexpr int kNumModSources = sizeof(kModSources) / sizeof(kModSources[0]);
const Named kModDestinations[] = {
  { "Off", "" },
  { "Osc1 WT Pos", "osc_1_wave_frame" }, { "Osc1 Level", "osc_1_level" }, { "Osc1 Detune", "osc_1_unison_detune" },
  { "Osc1 Warp", "osc_1_distortion_amount" }, { "Osc1 Morph", "osc_1_spectral_morph_amount" }, { "Osc1 Pitch", "osc_1_transpose" },
  { "Osc1 Pan", "osc_1_pan" },
  { "Osc2 WT Pos", "osc_2_wave_frame" }, { "Osc2 Level", "osc_2_level" }, { "Osc2 Detune", "osc_2_unison_detune" },
  { "Osc2 Warp", "osc_2_distortion_amount" }, { "Osc2 Morph", "osc_2_spectral_morph_amount" }, { "Osc2 Pitch", "osc_2_transpose" },
  { "Osc2 Pan", "osc_2_pan" },
  { "Osc3 WT Pos", "osc_3_wave_frame" }, { "Osc3 Level", "osc_3_level" }, { "Osc3 Detune", "osc_3_unison_detune" },
  { "Osc3 Warp", "osc_3_distortion_amount" }, { "Osc3 Morph", "osc_3_spectral_morph_amount" }, { "Osc3 Pitch", "osc_3_transpose" },
  { "Osc3 Pan", "osc_3_pan" },
  { "F1 Cutoff", "filter_1_cutoff" }, { "F1 Reso", "filter_1_resonance" }, { "F1 Drive", "filter_1_drive" },
  { "F1 Blend", "filter_1_blend" }, { "F1 Mix", "filter_1_mix" },
  { "F2 Cutoff", "filter_2_cutoff" }, { "F2 Reso", "filter_2_resonance" }, { "F2 Drive", "filter_2_drive" },
  { "F2 Blend", "filter_2_blend" }, { "F2 Mix", "filter_2_mix" },
  { "Dist Drive", "distortion_drive" }, { "Dist Mix", "distortion_mix" },
  { "Chorus Mix", "chorus_dry_wet" }, { "Phaser Mix", "phaser_dry_wet" },
  { "Delay Fdbk", "delay_feedback" }, { "Delay Mix", "delay_dry_wet" },
  { "Reverb Size", "reverb_size" }, { "Reverb Mix", "reverb_dry_wet" },
  { "LFO1 Rate", "lfo_5_frequency" }, { "LFO2 Rate", "lfo_6_frequency" }, { "LFO3 Rate", "lfo_7_frequency" },
  { "LFO4 Rate", "lfo_8_frequency" },
  { "Volume", "volume" },
};
constexpr int kNumModDestinations = sizeof(kModDestinations) / sizeof(kModDestinations[0]);
constexpr int kNumModSlots = VitalCore::kNumUserModulations;
// Sources that swing both ways: their modulation is centred on the knob (Vital's "bipolar").
bool bipolarSource(int s) { return (s >= 4 && s <= 7) || s == 11; }

const char* const kLfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square" };
constexpr int kNumLfoShapes = 5;
const int kLfoShapeToCore[kNumLfoShapes] = { 1, 0, 3, 4, 2 };   // VitalCore::setLfoShape numbering

const char* const kArpModes[Arp::kNumModes] = { "Up", "Down", "Up/Down", "Down/Up", "Random", "As Played", "Chord" };
const char* const kArpModesShort[Arp::kNumModes] = { "UP", "DOWN", "UP/DN", "DN/UP", "RAND", "PLAYED", "CHORD" };
const char* const kArpRates[Arp::kNumRates] = { "1/4", "1/4 D", "1/4 T", "1/8", "1/8 D", "1/8 T", "1/16", "1/16 D", "1/16 T",
                                                "1/32", "1/32 T" };
const char* const kStepStates[Arp::kNumStepStates] = { "Off", "On", "Accent", "Tie" };
const char* const kGrainSync[Granulator::kNumSyncRates] = { "Free", "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T" };
// The overlay panels (Panel parameter).
const char* const kPanels[] = { "Closed", "Arp", "Granulator", "Mod 1-8", "Mod 9-16", "LFOs" };
enum PanelId { kPanelClosed = 0, kPanelArp, kPanelGrain, kPanelMod1, kPanelMod2, kPanelLfo, kNumPanels };
// The browser's search keyboard.
const char* const kKeys = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr int kNumLetterKeys = 26;
enum ExtraKey { kKeySpace = kNumLetterKeys, kKeyDelete, kKeyClear, kNumKeys };
constexpr size_t kMaxQuery = 20;

// "modulation_49_amount" ... "modulation_64_amount": the engine controls behind the matrix's amounts.
const std::vector<std::string> kModAmountControls = [] {
  std::vector<std::string> v;
  for (int s = 0; s < VitalCore::kNumUserModulations; ++s)
    v.push_back("modulation_" + std::to_string(VitalCore::kFirstUserModulation + s + 1) + "_amount");
  return v;
}();

int exposedIndex(const char* vital_name) {
  for (int i = 0; i < static_cast<int>(sizeof(kExposed) / sizeof(kExposed[0])); ++i)
    if (std::string(kExposed[i].vital) == vital_name)
      return 5 + i;   // after the plugin's own parameters (kNumOwn)
  return -1;
}

// Projects saved before "Vel Sens" was added stored this many parameters (own + 84 exposed + 3 browser).
constexpr uint32_t kParamsV1 = 92;
constexpr int kExposedEndV1 = 89;
constexpr int kNumExposed = sizeof(kExposed) / sizeof(kExposed[0]);

// Plugin-own parameters come first. "Patch" must be parameter 0 (see the debounce in the worker).
enum OwnParam { kPatch = 0, kQuality, kMaxVoices, kMaxUnison, kStatus, kNumOwn };
static_assert(kNumOwn == 5, "exposedIndex() assumes 5 own parameters");

struct PatchEntry {
  std::string name;   // shown in lists, always "XX Name" (two-letter type code first)
  std::string path;   // empty = built-in Init
  int type = -1;      // index into kTypeCodes (-1 = Init)
};

// Two-letter sound-type codes. Factory patch names already start with one (tools/make_factory.cpp uses
// the same table); your own patches get one from the patch's Vital style, its folder or its name.
const char* const kTypeCodes[] = { "AR", "BS", "BL", "BR", "DY", "FX", "KY", "LD", "PD", "PC", "PL", "RH", "ST", "VX", "SY" };
const char* const kTypeNames[] = { "Arp", "Bass", "Bell", "Brass", "Dreamy", "FX", "Keys", "Lead", "Pad", "Perc", "Pluck",
                                   "Rhodes", "Strings", "Vocal", "Other" };
constexpr int kNumTypes = sizeof(kTypeCodes) / sizeof(kTypeCodes[0]);
constexpr int kOtherType = kNumTypes - 1;
constexpr size_t kMaxPatches = 6000;   // Init + factory + your own

int typeFromCode(const std::string& name) {
  if (name.size() < 3 || name[2] != ' ')
    return -1;
  for (int t = 0; t < kNumTypes; ++t)
    if (name.compare(0, 2, kTypeCodes[t]) == 0)
      return t;
  return -1;
}

std::string lowerCase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Vital's own "preset_style" (Bass, Lead, Keys, Pad, Percussion, Sequence, Experiment, SFX, Template).
int typeFromVitalStyle(const std::string& style) {
  std::string s = lowerCase(style);
  if (s == "bass") return 1;
  if (s == "lead") return 7;
  if (s == "keys") return 6;
  if (s == "pad") return 8;
  if (s == "percussion") return 9;
  if (s == "sequence") return 0;
  if (s == "sfx" || s == "experiment") return 5;
  return -1;
}

// Keywords in the folder or file name. First match wins, so the more specific words come first.
int typeFromWords(const std::string& text) {
  static const std::pair<const char*, int> kWords[] = {
    { "rhodes", 11 }, { "e.piano", 11 }, { "epiano", 11 }, { "wurl", 11 }, { "suitcase", 11 },
    { "bass", 1 }, { "sub ", 1 }, { "808", 1 }, { "reese", 1 }, { "wub", 1 }, { "growl", 1 },
    { "pluck", 10 }, { "harp", 10 }, { "pizz", 10 },
    { "bell", 2 }, { "chime", 2 }, { "mallet", 2 }, { "marimba", 2 }, { "glock", 2 },
    { "brass", 3 }, { "horn", 3 }, { "trumpet", 3 }, { "sax", 3 },
    { "string", 12 }, { "violin", 12 }, { "cello", 12 },
    { "vox", 13 }, { "vocal", 13 }, { "choir", 13 }, { "voice", 13 },
    { "arp", 0 }, { "seq", 0 },
    { "dream", 4 }, { "ambient", 4 }, { "atmo", 4 },
    { "pad", 8 }, { "drone", 8 }, { "texture", 8 },
    { "lead", 7 }, { "solo", 7 },
    { "key", 6 }, { "piano", 6 }, { "organ", 6 }, { "clav", 6 }, { "ep ", 6 }, { "chord", 6 }, { "stab", 6 },
    { "drum", 9 }, { "kick", 9 }, { "snare", 9 }, { "hat", 9 }, { "perc", 9 }, { "clap", 9 },
    { "fx", 5 }, { "sfx", 5 }, { "riser", 5 }, { "impact", 5 }, { "sweep", 5 },
  };
  std::string s = lowerCase(text) + " ";
  for (const auto& w : kWords)
    if (s.find(w.first) != std::string::npos)
      return w.second;
  return -1;
}

// Category labels that preset packs put in their patch names ("BA Hastings", "Pack - PD Lifeline",
// "FR2 - PLK - Fuhu", "GS_BASS_Free_Chewer", "pd cosmic_cloud", "Keys - Riot"). Whole words only.
int typeFromTag(const std::string& word, bool allow_generic) {
  static const std::pair<const char*, int> kTags[] = {
    { "arp", 0 }, { "arps", 0 }, { "seq", 0 }, { "sq", 0 }, { "sequence", 0 }, { "ar", 0 },
    { "ba", 1 }, { "bs", 1 }, { "bass", 1 }, { "basses", 1 },
    { "bell", 2 }, { "bells", 2 }, { "bl", 2 },
    { "brass", 3 }, { "br", 3 },
    { "dy", 4 },
    { "fx", 5 }, { "sfx", 5 }, { "osfx", 5 }, { "exp", 5 }, { "xpr", 5 }, { "riser", 5 },
    { "key", 6 }, { "keys", 6 }, { "ky", 6 }, { "ep", 6 }, { "piano", 6 }, { "organ", 6 },
    { "ch", 6 }, { "chord", 6 }, { "chords", 6 },
    { "ld", 7 }, { "lead", 7 }, { "leads", 7 },
    { "pd", 8 }, { "pad", 8 }, { "pads", 8 }, { "drn", 8 }, { "drone", 8 }, { "at", 8 }, { "atm", 8 },
    { "txt", 8 }, { "texture", 8 }, { "scp", 8 },
    { "pc", 9 }, { "dr", 9 }, { "drum", 9 }, { "drums", 9 }, { "perc", 9 },
    { "pl", 10 }, { "plk", 10 }, { "plck", 10 }, { "pluck", 10 }, { "plucks", 10 },
    { "rh", 11 }, { "rhodes", 11 },
    { "st", 12 }, { "str", 12 }, { "strings", 12 },
    { "vx", 13 }, { "vox", 13 }, { "vocal", 13 },
  };
  std::string w = lowerCase(word);
  for (const auto& t : kTags)
    if (w == t.first)
      return t.second;
  if (allow_generic && (w == "sy" || w == "syn" || w == "synth"))
    return kOtherType;
  return -1;
}

bool isNameSeparator(char c) { return c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')'; }

// Looks for a category label among the words of a patch file name. On a match, returns the type and
// sets clean_name to the part of the name after the label when the label starts a section
// ("2K Sub Vital Pack - PD Into Lucidity" -> "Into Lucidity"); otherwise clean_name is the whole name.
int typeFromNameTag(const std::string& stem, std::string& clean_name) {
  struct Word { size_t begin, end; };
  std::vector<Word> words;
  for (size_t i = 0; i < stem.size();) {
    while (i < stem.size() && isNameSeparator(stem[i])) ++i;
    size_t b = i;
    while (i < stem.size() && !isNameSeparator(stem[i])) ++i;
    if (i > b) words.push_back({ b, i });
  }
  for (int pass = 0; pass < 2; ++pass) {   // specific labels first, then "Synth"/"SY"
    for (size_t k = 0; k < words.size(); ++k) {
      int t = typeFromTag(stem.substr(words[k].begin, words[k].end - words[k].begin), pass == 1);
      if (t < 0)
        continue;
      if (pass == 1 && t != kOtherType)
        continue;
      // A label starts a section if it is the first word or follows " - " / "_".
      bool starts_section = k == 0;
      if (!starts_section) {
        std::string gap = stem.substr(words[k - 1].end, words[k].begin - words[k - 1].end);
        starts_section = gap.find('-') != std::string::npos || gap.find('_') != std::string::npos;
      }
      // The label itself is dropped only when it is a short code or in capitals ("PD", "BASS"), or a
      // separator follows it ("Keys - Riot"); "Bass Tutorial" keeps its name.
      std::string label = stem.substr(words[k].begin, words[k].end - words[k].begin);
      bool upper = std::none_of(label.begin(), label.end(), [](unsigned char c) { return std::islower(c); });
      std::string after = k + 1 < words.size() ? stem.substr(words[k].end, words[k + 1].begin - words[k].end) : "";
      bool drop_label = label.size() <= 3 || upper || after.find('-') != std::string::npos ||
                        after.find('_') != std::string::npos;
      std::string rest = stem;
      if (starts_section && drop_label) {
        rest = stem.substr(words[k].end);
        size_t s = 0;
        while (s < rest.size() && isNameSeparator(rest[s]) && rest[s] != '(') ++s;
        rest = rest.substr(s);
        if (std::none_of(rest.begin(), rest.end(), [](unsigned char c) { return std::isalpha(c); }))
          rest = stem;   // "Keys 9" stays "Keys 9"
      }
      std::replace(rest.begin(), rest.end(), '_', ' ');
      clean_name = rest;
      return t;
    }
  }
  return -1;
}

// Reads the "preset_style" near the start of a .vital file (Vital writes its keys alphabetically,
// so it comes before the large "settings" block).
std::string readVitalStyle(const fs::path& path) {
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
    return "";
  char buf[4096];
  size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  std::string head(buf, n);
  size_t k = head.find("\"preset_style\"");
  if (k == std::string::npos)
    return "";
  size_t q1 = head.find('"', head.find(':', k) + 1);
  size_t q2 = q1 == std::string::npos ? q1 : head.find('"', q1 + 1);
  if (q1 == std::string::npos || q2 == std::string::npos)
    return "";
  return head.substr(q1 + 1, q2 - q1 - 1);
}

std::string jsonQuote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out + "\"";
}

// The MPC's double-tap list for a parameter comes from "replacementStrings" in the skin, which is
// a file. The patch list is only known at run time (your own patches), so the plugin writes it
// into its skin (Plugin Skins/TUI.json, entry "Parameter 0") when it changed. The MPC reads the
// skin after loading the plugin; if a copy is cached, the new list shows from the next load.
void writeSkinPatchList(const std::vector<std::string>& names) {
  std::error_code ec;
  if (mpc::pluginFolder().empty())
    return;
  fs::path tui = fs::path(mpc::pluginFolder()) / "Plugin Skins" / "TUI.json";
  std::string text;
  if (!mpc::readTextFile(tui.string(), text))
    return;
  size_t key = text.find("\"Parameter 0\"");
  if (key == std::string::npos)
    return;
  size_t open = text.find('[', key);
  if (open == std::string::npos)
    return;
  size_t close = std::string::npos;   // the matching ']', skipping brackets inside strings
  bool in_string = false;
  for (size_t i = open + 1; i < text.size(); ++i) {
    char c = text[i];
    if (in_string) {
      if (c == '\\') ++i;
      else if (c == '"') in_string = false;
    }
    else if (c == '"') in_string = true;
    else if (c == ']') { close = i; break; }
  }
  if (close == std::string::npos)
    return;
  std::string list = "[";
  for (size_t i = 0; i < names.size(); ++i)
    list += (i ? ", " : "") + jsonQuote(names[i]);
  list += "]";
  if (text.compare(open, close - open + 1, list) == 0)
    return;   // already current
  text.replace(open, close - open + 1, list);
  fs::path tmp = tui;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return;
    out << text;
    if (!out)
      return;
  }
  fs::rename(tmp, tui, ec);
  if (ec)
    fs::remove(tmp, ec);
}

std::string trimName(std::string s, size_t max_len) {
  if (s.size() > max_len)
    s = s.substr(0, max_len);
  return s;
}

std::string formatNumber(double v, int decimals) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  if (buf[0] == '-') {
    bool zero = true;
    for (const char* q = buf + 1; *q; ++q)
      if (*q != '0' && *q != '.') { zero = false; break; }
    if (zero) return std::string(buf + 1);
  }
  return buf;
}

int decimalsFor(double v) {
  double a = std::fabs(v);
  if (a >= 100.0) return 0;
  if (a >= 10.0) return 1;
  return 2;
}

}  // namespace

class Warpline : public mpc::Plugin {
  public:
    static mpc::PluginInfo makeInfo() {
      mpc::PluginInfo info;
      info.name = "Warpline";
      info.vendor = "CLVRKE";
      info.uniqueId = MPC_FOURCC('C', 'v', 'V', 'm');
      info.version = 1100;
      info.isInstrument = true;
      info.numInputs = 0;
      info.numOutputs = 2;
      info.tailSamples = 0;
      return info;
    }

    Warpline() : mpc::Plugin(makeInfo()) {
      scanPatches();

      std::vector<std::string> names;
      names.reserve(patches_.size());
      for (const PatchEntry& p : patches_)
        names.push_back(trimName(p.name, 24));
      writeSkinPatchList(names);

      // Plugin-own parameters.
      addParam(mpc::choiceParam("patch", "Patch", names, 0));
      addParam(mpc::choiceParam("quality", "Quality", { "Eco", "High" }, 0));
      addParam(mpc::intParam("max_voices", "Max Voices", 1, vital::kMaxPolyphony - 1, 8));
      addParam(mpc::intParam("max_unison", "Max Unison", 1, 16, 8));
      {
        mpc::Param status = mpc::floatParam("status", "Status", 0.0f, 1.0f, 0.0f);
        status.automatable = false;
        addParam(status);
      }

      // Exposed Vital controls.
      offset_.assign(kNumExposed, 0.0f);
      for (int i = 0; i < kNumExposed; ++i) {
        const vital::ValueDetails d = vital::Parameters::getDetails(kExposed[i].vital);
        std::string id = std::string("v_") + kExposed[i].vital;
        mpc::Param p;
        // Vital's name tables are sometimes shorter than the control's range (filter style names
        // depend on the model), so only trust them where the sizes are known to match.
        std::string vname = kExposed[i].vital;
        bool lookup_ok = d.string_lookup && vname.find("_style") == std::string::npos;
        if (d.value_scale == vital::ValueDetails::kIndexed && lookup_ok) {
          std::vector<std::string> choices;
          for (int c = static_cast<int>(d.min); c <= static_cast<int>(d.max); ++c)
            choices.push_back(d.string_lookup[c]);
          if (vname.size() >= 16 && vname.compare(vname.size() - 16, 16, "_distortion_type") == 0 && choices.size() == 13) {
            // Vital names these relative to the other two oscillators; spell them out so every choice is unique.
            int self = vname[4] - '0';
            int a = self == 1 ? 2 : 1, b = self == 3 ? 2 : 3;
            choices[7] = "FM <- Osc " + std::to_string(a);
            choices[8] = "FM <- Osc " + std::to_string(b);
            choices[10] = "RM <- Osc " + std::to_string(a);
            choices[11] = "RM <- Osc " + std::to_string(b);
          }
          p = mpc::choiceParam("", "", choices, static_cast<int>(d.default_value - d.min));
          offset_[i] = d.min;   // choice index 0 = Vital value d.min
        }
        else if (d.value_scale == vital::ValueDetails::kIndexed) {
          p = mpc::intParam("", "", static_cast<int>(d.min), static_cast<int>(d.max),
                            static_cast<int>(d.default_value));
        }
        else {
          p = mpc::floatParam("", "", d.min, d.max, d.default_value);
        }
        p.id = id;
        p.name = kExposed[i].label;
        if (p.kind == mpc::Param::Kind::Choice && p.choices.size() == 2 &&
            p.choices[0] == "Off" && p.choices[1] == "On") {
          p.kind = mpc::Param::Kind::Bool;
          p.choices.clear();
        }
        addParam(p);
      }

      // Browser arrows for the skin's patch bar (momentary, not automatable).
      {
        mpc::Param prev = mpc::boolParam("prev", "Prev", false);
        prev.automatable = false;
        prev_index_ = addParam(prev);
        mpc::Param next = mpc::boolParam("next", "Next", false);
        next.automatable = false;
        next_index_ = addParam(next);
        std::vector<std::string> cats = { "ALL" };
        for (int t = 0; t < kNumTypes; ++t)
          cats.push_back(std::string(kTypeCodes[t]) + " " + kTypeNames[t]);
        mpc::Param cat = mpc::choiceParam("category", "Category", cats, 0);
        // Automatable like the other menus, so the MPC's double-tap list shows the types.
        category_index_ = addParam(cat);
      }

      // Live View (experimental): the wavetable and filter pictures follow LFOs, envelopes and other
      // modulation while notes play. The plugin writes the "live" display parameters ~12 times a
      // second; they are not automatable, so the MPC never records them. Off = pictures follow the knobs.
      {
        mpc::Param lv = mpc::choiceParam("live_view", "Live View", { "Off", "On" }, 0);
        lv.automatable = false;
        live_view_index_ = addParam(lv);
        for (int i = 0; i < kNumLive; ++i) {
          mpc::Param d = mpc::floatParam(kLive[i].id, kLive[i].label, 0.0f, 1.0f, 0.0f);
          d.automatable = false;
          int index = addParam(d);
          if (i == 0) live_first_index_ = index;
          live_base_index_[i] = exposedIndex(kLive[i].vital);
        }
      }

      // Scrollable patch list (PATCH page and the browser): rows of patch names inside the TYPE filter,
      // starting at list_top_. Each row's value text is a patch name; tapping a row loads it. Built
      // because the MPC's own double-tap menu shows no text for plugin lists.
      static const char* const kRowIds[kListRows] = { "list_row_1", "list_row_2", "list_row_3", "list_row_4", "list_row_5",
                                                      "list_row_6", "list_row_7", "list_row_8", "list_row_9", "list_row_10",
                                                      "list_row_11", "list_row_12" };
      static const char* const kRowNames[kListRows] = { "List 1", "List 2", "List 3", "List 4", "List 5", "List 6",
                                                        "List 7", "List 8", "List 9", "List 10", "List 11", "List 12" };
      auto addRow = [&](int r) {
        mpc::Param row = mpc::boolParam(kRowIds[r], kRowNames[r], false);   // momentary, like the arrows
        row.automatable = false;
        list_row_index_[r] = addParam(row);
      };
      for (int r = 0; r < kListRowsV1; ++r)
        addRow(r);

      // Added for the browser (after everything older, so saved projects keep their order):
      // rows 10-12, the scroll bar, which row holds the playing patch, and the browser's open state.
      for (int r = kListRowsV1; r < kListRows; ++r)
        addRow(r);
      {
        mpc::Param sc = mpc::floatParam("list_scroll", "List Scroll", 0.0f, 1.0f, 1.0f);   // 1 = top of the list
        sc.automatable = false;
        list_scroll_index_ = addParam(sc);
        std::vector<std::string> rows;
        for (int r = 1; r <= kListRows; ++r)
          rows.push_back(std::to_string(r));
        rows.push_back("-");
        mpc::Param cur = mpc::choiceParam("list_current", "List Current", rows, kListRows);
        cur.automatable = false;
        list_current_index_ = addParam(cur);
        mpc::Param br = mpc::choiceParam("browser", "Browser", { "Closed", "Open" }, 0);
        br.automatable = false;
        browser_index_ = addParam(br);
      }

      addParamsV11();

      dirty_ = std::make_unique<std::atomic<bool>[]>(numParams());
      for (int i = 0; i < numParams(); ++i)
        dirty_[i].store(false);

      // Every patch is also a program: the MPC preset list becomes the patch browser.
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        addProgram(trimName(patches_[i].name, 24).c_str(), { static_cast<float>(i) });

      loadFavourites();
      core_ = std::make_unique<VitalCore>();
      core_->prepare(44100.0);
      granulator_.prepare(44100.0);
      arp_.setSampleRate(44100.0);
      rememberPatchLimits();
      applyEcoSettings();
      pullParamsFromCore();

      worker_ = std::thread([this] { workerLoop(); });
    }

    ~Warpline() override {
      {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        quit_ = true;
      }
      worker_cv_.notify_all();
      if (worker_.joinable())
        worker_.join();
    }

    // ----- lifecycle --------------------------------------------------------------------------
    void prepare(double sample_rate, int /*max_block*/) override {
      std::lock_guard<std::mutex> load_lock(load_mutex_);
      pauseAudio();
      core_->prepare(sample_rate);
      applyEcoSettings();
      granulator_.prepare(sample_rate);
      arp_.setSampleRate(sample_rate);
      arp_dirty_.store(true);
      grain_dirty_.store(true);
      resumeAudio();
    }

    void reset() override {
      // Audio thread (resume, or after a state load). Stops hanging notes.
      if (!paused_.load()) {
        arp_.reset([](int, float, bool) {});
        core_->allSoundsOff();
        granulator_.reset();
      }
    }

    // ----- audio ------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override {
      float* left = ctx.outputs[0];
      float* right = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];

      in_process_.store(true);
      if (paused_.load()) {
        std::memset(left, 0, sizeof(float) * ctx.numFrames);
        if (right != left)
          std::memset(right, 0, sizeof(float) * ctx.numFrames);
        in_process_.store(false);
        return;
      }

      applyDirtyParams();

      double bpm = 120.0;
      if (ctx.transport.valid && ctx.transport.tempoValid && ctx.transport.bpm > 0.0) {
        bpm = ctx.transport.bpm;
        core_->setBpm(static_cast<float>(bpm));
      }
      bool playing = ctx.transport.valid && ctx.transport.playing && ctx.transport.ppqValid && ctx.transport.bpm > 0.0;
      if (playing)
        core_->setTime(ctx.transport.ppqPos * 60.0 / ctx.transport.bpm);

      auto send = [this](int note, float velocity, bool on) {
        if (on)
          core_->noteOn(note, velocity, 0, arp_channel_);
        else
          core_->noteOff(note, 0.0f, 0, arp_channel_);
      };
      if (arp_dirty_.exchange(false))
        arp_.setSettings(arpSettings(), send);
      arp_.beginBlock(bpm, playing, playing ? ctx.transport.ppqPos : 0.0);

      // Render between MIDI events and arp steps so notes land on the right sample.
      int pos = 0;
      int e = 0;
      while (pos < ctx.numFrames) {
        while (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset <= pos)
          handleMidi(ctx.midi[e++], send);
        arp_.fire(send);
        int next = ctx.numFrames;
        if (e < ctx.numMidiEvents)
          next = std::min(next, std::max(pos + 1, ctx.midi[e].sampleOffset));
        next = std::min(next, pos + std::max(1, arp_.samplesToNextEvent()));
        core_->render(left + pos, right + pos, next - pos);
        arp_.advance(next - pos);
        pos = next;
      }
      while (e < ctx.numMidiEvents)
        handleMidi(ctx.midi[e++], send);
      arp_step_.store(arp_.running() ? arp_.currentStep() : -1);

      if (grain_dirty_.exchange(false))
        granulator_.setSettings(grainSettings());
      granulator_.setTempo(bpm);
      if (right != left)
        granulator_.process(left, right, ctx.numFrames);
      in_process_.store(false);
    }

    // ----- parameters -------------------------------------------------------------------------
    void parameterChanged(int index, float plain) override {
      if (index == kPatch) {
        if (suppress_patch_trigger_.load())
          return;
        requested_patch_.store(static_cast<int>(std::lround(plain)));
        last_patch_request_ns_.store(nowNs());
        worker_cv_.notify_all();
        return;
      }
      if (index == kQuality) {
        quality_dirty_.store(true);
        worker_cv_.notify_all();
        return;
      }
      if (index == kStatus)
        return;
      if (index == category_index_) {
        int c = static_cast<int>(std::lround(plain));
        category_.store(c);
        // Picking a type jumps to its first patch (unless the current one already is that type).
        if (c > 0 && !suppress_patch_trigger_.load()) {
          int cur = requested_patch_.load();
          if (cur < 0 || cur >= static_cast<int>(patches_.size()) || patches_[cur].type != c - 1) {
            int k = stepPatch(0, 1);
            if (k != cur)
              setNormalised(kPatch, param(kPatch).toNormalised(static_cast<float>(k)));
          }
        }
        list_follow_.store(true);     // the list changes with the filter: show the playing patch
        worker_cv_.notify_all();
        return;
      }
      if (index == browser_index_) {
        if (plain > 0.5f && panel_index_ >= 0 && getInt(panel_index_) != kPanelClosed) {
          setNormalised(panel_index_, 0.0f);
          notifyHostParameterChanged(panel_index_);
        }
        return;
      }
      if (index == list_current_index_)
        return;   // screen state only
      if (index >= first_v11_index_ && first_v11_index_ >= 0) {
        parameterChangedV11(index, plain);
        return;
      }
      if (index == list_scroll_index_) {
        if (!list_scroll_from_plugin_.load()) {
          list_top_.store(topFromScroll(plain));
          display_dirty_.store(true);
          list_marker_dirty_.store(true);
          worker_cv_.notify_all();
        }
        return;
      }
      if (index == live_view_index_) {
        live_view_.store(plain > 0.5f);
        return;
      }
      if (live_first_index_ >= 0 && index >= live_first_index_ && index < live_first_index_ + kNumLive)
        return;   // display only
      if (int r = listRowOf(index); r >= 0) {
        if (plain < 0.5f || stepping_.exchange(true))
          return;
        int k = listPatchAt(r);
        if (k >= 0)
          setNormalised(kPatch, param(kPatch).toNormalised(static_cast<float>(k)));   // triggers the load
        setNormalised(index, 0.0f);
        notifyHostParameterChanged(kPatch);
        notifyHostParameterChanged(index);
        stepping_.store(false);
        return;
      }
      if (index == prev_index_ || index == next_index_) {
        if (plain < 0.5f || stepping_.exchange(true))
          return;
        int k = stepPatch(requested_patch_.load(), index == next_index_ ? 1 : -1);
        setNormalised(kPatch, param(kPatch).toNormalised(static_cast<float>(k)));   // triggers the load
        setNormalised(index, 0.0f);
        notifyHostParameterChanged(kPatch);
        notifyHostParameterChanged(index);
        stepping_.store(false);
        return;
      }
      if (index >= 0 && index < numParams()) {
        dirty_[index].store(true);
        any_dirty_.store(true);
      }
    }

    bool formatValue(int index, float plain, std::string& out) override {
      if (index == kStatus) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        out = status_text_;
        return true;
      }
      if (int r = listRowOf(index); r >= 0) {
        int k = listPatchAt(r);
        out = k >= 0 ? trimName(patches_[k].name, 24) : "";
        return true;
      }
      if (index >= first_v11_index_ && first_v11_index_ >= 0)
        return formatValueV11(index, plain, out);
      if (index == prev_index_) { out = "<"; return true; }
      if (index == next_index_) { out = ">"; return true; }
      if (index < kNumOwn || index >= kNumOwn + kNumExposed)
        return false;
      const Exposed& x = kExposed[index - kNumOwn];
      const vital::ValueDetails d = vital::Parameters::getDetails(x.vital);
      if (d.value_scale == vital::ValueDetails::kIndexed)
        return false;   // choices and integers: the framework's text is right

      std::string name = x.vital;
      if (name.find("_wave_frame") != std::string::npos) {
        out = std::to_string(static_cast<int>(std::lround(plain)) + 1);
        return true;
      }
      if (name.find("_cutoff") != std::string::npos) {
        double hz = 440.0 * std::pow(2.0, (plain - 69.0) / 12.0);
        out = hz >= 1000.0 ? formatNumber(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + "k" : formatNumber(hz, 0) + "Hz";
        return true;
      }

      double v = plain;
      switch (d.value_scale) {
        case vital::ValueDetails::kQuadratic: v = v * v; break;
        case vital::ValueDetails::kCubic: v = v * v * v; break;
        case vital::ValueDetails::kQuartic: v = v * v; v = v * v; break;
        case vital::ValueDetails::kExponential: v = std::pow(2.0, v); break;
        case vital::ValueDetails::kSquareRoot: v = std::sqrt(std::max(v, 0.0)); break;
        default: break;
      }
      v += d.post_offset;
      if (d.display_invert && v != 0.0)
        v = 1.0 / v;
      v *= d.display_multiply;

      std::string units = d.display_units;
      if (units == " secs") {
        out = v < 1.0 ? formatNumber(v * 1000.0, 0) + "ms" : formatNumber(v, 2) + "s";
        return true;
      }
      if (units == " semitones") units = "st";
      else if (units == " cents") units = "c";
      else if (units == " dB" || units == "dB") units = "dB";
      out = formatNumber(v, decimalsFor(v)) + units;
      return true;
    }

    // ----- state ------------------------------------------------------------------------------
    // Layout: "VTM1", u32 text length, patch text, u32 path length, path, u32 n, n normalised values.
    void saveExtraState(std::vector<uint8_t>& out) override {
      std::string text, path;
      {
        std::lock_guard<std::mutex> lock(load_mutex_);
        text = current_patch_text_;
        path = current_patch_path_;
      }
      auto put32 = [&out](uint32_t v) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + 4);
      };
      out.insert(out.end(), { 'V', 'T', 'M', '1' });
      put32(static_cast<uint32_t>(text.size()));
      out.insert(out.end(), text.begin(), text.end());
      put32(static_cast<uint32_t>(path.size()));
      out.insert(out.end(), path.begin(), path.end());
      put32(static_cast<uint32_t>(numParams()));
      for (int i = 0; i < numParams(); ++i) {
        float v = getNormalised(i);
        const uint8_t* q = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), q, q + 4);
      }
    }

    void loadExtraState(const uint8_t* data, size_t size) override {
      size_t pos = 0;
      auto get32 = [&](uint32_t& v) {
        if (pos + 4 > size) return false;
        std::memcpy(&v, data + pos, 4);
        pos += 4;
        return true;
      };
      if (size < 4 || std::memcmp(data, "VTM1", 4) != 0)
        return;
      pos = 4;
      uint32_t len = 0;
      if (!get32(len) || pos + len > size) return;
      std::string text(reinterpret_cast<const char*>(data + pos), len);
      pos += len;
      if (!get32(len) || pos + len > size) return;
      std::string path(reinterpret_cast<const char*>(data + pos), len);
      pos += len;
      uint32_t n = 0;
      if (!get32(n) || pos + static_cast<size_t>(n) * 4 > size) return;
      std::vector<float> values(n);
      std::memcpy(values.data(), data + pos, static_cast<size_t>(n) * 4);

      // The Patch parameter's saved value is authoritative (a load may have been pending when the
      // project was saved); the saved patch text is what gets loaded, so nothing is re-read from disk.
      // Exception: when the saved file is still in the list, its position wins, because the list
      // grows when you add patches and an old normalised Patch value would point somewhere else.
      int index = findPatchIndex(path);
      bool found_by_path = index >= 0 && !path.empty();
      if (!found_by_path && n > static_cast<uint32_t>(kPatch))
        index = static_cast<int>(std::lround(param(kPatch).toPlain(values[kPatch])));
      {
        std::lock_guard<std::mutex> load_lock(load_mutex_);
        pauseAudio();
        PatchReport report;
        loadIntoCore(text, report);
        current_patch_text_ = text;
        current_patch_path_ = path;
        loaded_patch_ = index;
        requested_patch_.store(index);
        setStatus(report);
        resumeAudio();
      }

      live_hold_until_ns_.store(nowNs() + 2000LL * 1000 * 1000);
      suppress_patch_trigger_.store(true);
      // Older projects have fewer parameters: only their engine controls line up with today's.
      uint32_t usable = n == kParamsV1 ? static_cast<uint32_t>(kExposedEndV1) : n;
      for (uint32_t i = 0; i < usable && static_cast<int>(i) < numParams(); ++i) {
        if (static_cast<int>(i) == kPatch) continue;
        setNormalised(static_cast<int>(i), values[i]);
      }
      setNormalised(kPatch, (!found_by_path && n > static_cast<uint32_t>(kPatch)) ? values[kPatch]
                                                                                 : getParamNormalisedForPatch(index));
      suppress_patch_trigger_.store(false);
      {
        // The framework restored every parameter before this call (List Drag too): that is not a touch.
        std::lock_guard<std::mutex> lock(drag_mutex_);
        drag_ = DragGesture();
      }
      markAllDirty();
      mod_routing_dirty_.store(true);
      worker_cv_.notify_all();
    }

  private:
    // ----- patches ----------------------------------------------------------------------------
    void scanPatches() {
      patches_.clear();
      patches_.push_back({ "Init", "", -1 });

      // Finds .vital files under root, names every one "XX Name", and adds them sorted by type, then name.
      auto scan = [this](const fs::path& root) {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
          return;
        std::vector<PatchEntry> found;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
          if (!it->is_regular_file(ec))
            continue;
          const fs::path& p = it->path();
          std::string ext = lowerCase(p.extension().string());
          std::string stem = p.stem().string();
          if (ext != ".vital" || stem.empty() || stem[0] == '.')
            continue;

          PatchEntry e;
          e.path = p.generic_string();
          e.type = typeFromCode(stem);
          if (e.type >= 0) {
            size_t s = 3;   // "PL - Elektro ARP" -> "PL Elektro ARP"
            while (s < stem.size() && (stem[s] == ' ' || stem[s] == '-' || stem[s] == '_')) ++s;
            e.name = s < stem.size() ? stem.substr(0, 3) + stem.substr(s) : stem;
          }
          else {
            std::string shown = stem;
            e.type = typeFromNameTag(stem, shown);
            if (e.type < 0)
              e.type = typeFromVitalStyle(readVitalStyle(p));
            if (e.type < 0)
              e.type = typeFromWords(stem);
            if (e.type < 0) {
              // Folder names: labels first ("Bass", "Pads"), then keywords, but not "sub" ("2K Sub Pack").
              std::string folder = fs::relative(p.parent_path(), root, ec).generic_string();
              std::string ignored;
              e.type = typeFromNameTag(folder, ignored);
              if (e.type < 0) {
                std::string f = " " + lowerCase(folder) + " ";
                for (size_t k; (k = f.find(" sub ")) != std::string::npos;)
                  f.replace(k, 5, " ");
                e.type = typeFromWords(f);
              }
            }
            if (e.type < 0)
              e.type = kOtherType;
            e.name = std::string(kTypeCodes[e.type]) + " " + shown;
          }
          found.push_back(e);
          if (patches_.size() + found.size() >= kMaxPatches)
            break;
        }
        std::sort(found.begin(), found.end(), [](const PatchEntry& a, const PatchEntry& b) {
          if (a.type != b.type) return a.type < b.type;
          return lowerCase(a.name) < lowerCase(b.name);
        });
        patches_.insert(patches_.end(), found.begin(), found.end());
      };

      scan(fs::path(mpc::contentFolder()) / "Patches");
      factory_count_ = static_cast<int>(patches_.size());

      // <drive>/Warpline/Patches, where <drive> holds the folder this plugin is in: "VST2 Plugins"
      // (copied by hand) or "Synths" (installed by the Open MPC installer / Plugin Manager, e.g. /nvme/Synths).
      fs::path plugin_folder(mpc::pluginFolder());
      fs::path parent = plugin_folder.parent_path();
      if (parent.filename() == "VST2 Plugins" || parent.filename() == "Synths")
        scan(parent.parent_path() / "Warpline" / "Patches");
      if (const char* env = std::getenv("WARPLINE_USER_PATCHES"))
        scan(fs::path(env));
    }

    // Next / previous patch (dir = +1 / -1) from "from", staying inside the Category filter.
    int stepPatch(int from, int dir) const {
      int n = static_cast<int>(patches_.size());
      std::lock_guard<std::mutex> lock(filter_mutex_);
      for (int k = 1; k <= n; ++k) {
        int i = ((from + dir * k) % n + n) % n;
        if (passesFilter(i))
          return i;
      }
      return from;
    }

    // TYPE, search text and favourites. Caller holds filter_mutex_.
    bool passesFilter(int i) const {
      int filter = category_.load();   // 0 = all, else type + 1
      if (filter != 0 && patches_[i].type != filter - 1)
        return false;
      if (fav_only_.load() && favourites_.count(favKey(i)) == 0)
        return false;
      if (!query_.empty() && search_names_[i].find(query_) == std::string::npos)
        return false;
      return true;
    }

    // Favourites are kept by file name (without .vital), so they survive moving a bank to another drive.
    std::string favKey(int i) const {
      if (i < 0 || i >= static_cast<int>(patches_.size()))
        return "";
      return patches_[i].path.empty() ? std::string("Init") : fs::path(patches_[i].path).stem().string();
    }

    // Which list row a parameter is, or -1.
    int listRowOf(int index) const {
      for (int r = 0; r < kListRows; ++r)
        if (list_row_index_[r] == index)
          return r;
      return -1;
    }

    // Patches inside the TYPE filter, in list order.
    std::vector<int> filteredPatches() const {
      std::vector<int> out;
      std::lock_guard<std::mutex> lock(filter_mutex_);
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        if (passesFilter(i))
          out.push_back(i);
      return out;
    }

    int maxListTop(int count) const { return std::max(0, count - kListPageRows); }

    // Scroll bar value (1 = top) -> first visible position in the filtered list.
    int topFromScroll(float value) const {
      int top_max = maxListTop(static_cast<int>(filteredPatches().size()));
      return static_cast<int>(std::lround((1.0f - std::clamp(value, 0.0f, 1.0f)) * top_max));
    }

    // Patch index shown in list row r, or -1 for an empty row.
    int listPatchAt(int r) const {
      std::vector<int> list = filteredPatches();
      int pos = list_top_.load() + r;
      return pos >= 0 && pos < static_cast<int>(list.size()) ? list[pos] : -1;
    }

    // Worker thread: when the playing patch or the filter changed, scroll so the playing patch is
    // visible (a few rows from the top), move the scroll bar to match, and mark the playing row.
    void updateListPosition(bool follow) {
      if (list_scroll_index_ < 0)
        return;
      std::vector<int> list = filteredPatches();
      int count = static_cast<int>(list.size());
      int cur = loaded_patch_.load();
      int pos = static_cast<int>(std::find(list.begin(), list.end(), cur) - list.begin());
      int top = std::min(list_top_.load(), maxListTop(count));
      if (follow && pos < count && (pos < top || pos >= top + kListPageRows))
        top = std::clamp(pos - 3, 0, maxListTop(count));
      list_top_.store(top);
      int top_max = maxListTop(count);
      float scroll = top_max > 0 ? 1.0f - static_cast<float>(top) / top_max : 1.0f;
      if (std::fabs(scroll - getNormalised(list_scroll_index_)) > 1e-4f) {
        list_scroll_from_plugin_.store(true);
        setNormalised(list_scroll_index_, scroll);
        list_scroll_from_plugin_.store(false);
        notifyHostParameterChanged(list_scroll_index_);
      }
      int row = pos < count && pos >= top && pos < top + kListRows ? pos - top : kListRows;
      float marker = param(list_current_index_).toNormalised(static_cast<float>(row));
      if (std::fabs(marker - getNormalised(list_current_index_)) > 1e-4f) {
        setNormalised(list_current_index_, marker);
        notifyHostParameterChanged(list_current_index_);
      }
    }

    int findPatchIndex(const std::string& path) const {
      if (path.empty())
        return 0;
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        if (patches_[i].path == path)
          return i;
      // Same file name elsewhere (drive renamed or moved)?
      std::string stem = fs::path(path).stem().string();
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        if (!patches_[i].path.empty() && fs::path(patches_[i].path).stem().string() == stem)
          return i;
      return 0;
    }

    float getParamNormalisedForPatch(int index) const { return param(kPatch).toNormalised(static_cast<float>(index)); }

    // Load text into the (paused) core: a .vital patch, or Init when the text is empty.
    bool loadIntoCore(const std::string& text, PatchReport& report) {
      bool ok = true;
      if (text.empty()) {
        core_->loadInit();
        report = PatchReport();
        report.ok = true;
        report.name = "Init";
      }
      else {
        ok = core_->loadPatch(text, report);
        if (!ok)
          core_->loadInit();
      }
      rememberPatchLimits();
      applyEcoSettings(&report);
      lfo_applied_mask_ = 0;          // the patch brought its own LFO 5-8
      syncMatrixLocked();
      core_->warmUp();
      return ok;
    }

    void rememberPatchLimits() {
      patch_polyphony_ = static_cast<int>(core_->getControl("polyphony") + 0.5f);
      patch_oversampling_ = static_cast<int>(core_->getControl("oversampling") + 0.5f);
    }

    // Applies Quality, Max Voices and Max Unison to the core. Off the audio thread (or paused).
    void applyEcoSettings(PatchReport* report = nullptr) {
      int max_voices = getInt(kMaxVoices);
      int max_unison = getInt(kMaxUnison);
      int oversampling = getInt(kQuality) == 0 ? 0 : std::min(patch_oversampling_, 1);
      core_->setControl("polyphony", static_cast<float>(patch_polyphony_));
      core_->applyLimits(max_unison, max_voices, oversampling, report);
    }

    // After a patch load: copy the engine's values into the MPC parameters.
    void pullParamsFromCore() {
      for (int i = 0; i < kNumExposed; ++i) {
        int index = kNumOwn + i;
        float v = core_->getControl(kExposed[i].vital) - offset_[i];
        set(index, v);
        dirty_[index].store(false);
      }
    }

    void setStatus(const PatchReport& report) {
      std::string s;
      if (!report.ok)
        s = "Error";
      else if (report.notes.empty())
        s = "OK";
      else
        s = std::to_string(report.notes.size()) + " adj";
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_text_ = s;
      status_notes_ = report.notes;
    }

    // ----- worker thread ----------------------------------------------------------------------
    static int64_t nowNs() {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void workerLoop() {
      constexpr int64_t kDebounceNs = 80 * 1000 * 1000;   // ignores the host's param-0 nudge
      std::unique_lock<std::mutex> lock(worker_mutex_);
      int tick = 0;
      while (!quit_) {
        worker_cv_.wait_for(lock, std::chrono::milliseconds(40));
        if (quit_)
          break;
        // Just after a project load the restored screen values stay as saved for a moment (as Live View).
        bool list_hold = nowNs() < live_hold_until_ns_.load();
        bool follow = !list_hold && list_follow_.exchange(false);
        if (follow || (!list_hold && list_marker_dirty_.exchange(false))) {
          lock.unlock();
          updateListPosition(follow);
          lock.lock();
          display_dirty_.store(true);
        }
        if (display_dirty_.exchange(false)) {
          lock.unlock();
          requestDisplayUpdate();
          lock.lock();
        }
        if (++tick % 2 == 0) {
          lock.unlock();
          updateLiveDisplay();
          lock.lock();
        }
        lock.unlock();
        workerV11();
        lock.lock();

        int want = requested_patch_.load();
        bool patch_due = want != loaded_patch_ && nowNs() - last_patch_request_ns_.load() > kDebounceNs;
        bool quality_due = quality_dirty_.exchange(false);
        if (!patch_due && !quality_due)
          continue;

        lock.unlock();
        if (patch_due)
          loadPatchIndex(want);
        else if (quality_due) {
          std::lock_guard<std::mutex> load_lock(load_mutex_);
          pauseAudio();
          applyEcoSettings();
          core_->warmUp();
          resumeAudio();
        }
        lock.lock();
      }
    }

    // Writes the display parameters: modulated values while Live View is on and notes play,
    // otherwise the knob values. Only changes of at least one picture frame are sent to the host.
    void updateLiveDisplay() {
      if (live_first_index_ < 0 || nowNs() < live_hold_until_ns_.load())
        return;   // just after a project load: leave the restored values alone for a moment
      bool live = live_view_.load();
      for (int i = 0; i < kNumLive; ++i) {
        float v = getNormalised(live_base_index_[i]);
        if (live) {
          std::lock_guard<std::mutex> load_lock(load_mutex_);
          v = core_->liveNormalised(kLive[i].vital);
        }
        int index = live_first_index_ + i;
        if (std::fabs(v - getNormalised(index)) < 0.5f / 48.0f)
          continue;
        setNormalised(index, v);
        notifyHostParameterChanged(index);
      }
    }

    void loadPatchIndex(int index) {
      if (index < 0 || index >= static_cast<int>(patches_.size()))
        return;
      std::string text;
      if (!patches_[index].path.empty() && !mpc::readTextFile(patches_[index].path, text))
        text.clear();

      PatchReport report;
      {
        std::lock_guard<std::mutex> load_lock(load_mutex_);
        pauseAudio();
        loadIntoCore(text, report);
        current_patch_text_ = text;
        current_patch_path_ = patches_[index].path;
        loaded_patch_ = index;
        pullParamsFromCore();
        resumeAudio();
      }
      setStatus(report);

      // Like Velour's preset browser: one display update makes the MPC re-read every name and value.
      // (No per-parameter "automate" calls: they would be recorded as automation while recording.)
      // The worker's next pass moves the patch list to the new patch and then requests that update.
      list_follow_.store(true);
    }

    // ----- audio pause handshake (Dekker style, all seq_cst) ------------------------------------
    void pauseAudio() {
      paused_.store(true);
      for (int spins = 0; in_process_.load() && spins < 20000; ++spins)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    void resumeAudio() {
      markAllDirty();
      paused_.store(false);
    }

    void markAllDirty() {
      for (int i = kNumOwn; i < kNumOwn + kNumExposed; ++i)
        dirty_[i].store(true);
      dirty_[kMaxVoices].store(true);
      dirty_[kMaxUnison].store(true);
      for (int s = 0; s < kNumModSlots && mod_first_index_ >= 0; ++s)
        dirty_[modAmountIndex(s)].store(true);
      for (int i = 0; i < kNumExposed2 && exposed2_first_index_ >= 0; ++i)
        dirty_[exposed2_first_index_ + i].store(true);
      arp_dirty_.store(true);
      grain_dirty_.store(true);
      any_dirty_.store(true);
    }

    // Audio thread.
    void applyDirtyParams() {
      if (!any_dirty_.exchange(false))
        return;
      int max_unison = getInt(kMaxUnison);
      bool limits = dirty_[kMaxVoices].exchange(false) | dirty_[kMaxUnison].exchange(false);
      if (limits) {
        core_->setControl("polyphony", static_cast<float>(std::min(patch_polyphony_, getInt(kMaxVoices))));
        for (int o = 0; o < 3; ++o)
          dirty_[kNumOwn + 1 + 4 * o].store(true);   // the three "Osc Unison" parameters
      }
      for (int i = 0; i < kNumExposed; ++i) {
        int index = kNumOwn + i;
        if (!dirty_[index].exchange(false))
          continue;
        float v = get(index) + offset_[i];
        if (i == 1 || i == 5 || i == 9)   // unison voices
          v = std::min(v, static_cast<float>(max_unison));
        core_->setControl(kExposed[i].vital, v);
      }
      // Mod matrix amounts (Vital modulations 49-64).
      for (int s = 0; s < kNumModSlots && mod_first_index_ >= 0; ++s) {
        if (!dirty_[modAmountIndex(s)].exchange(false))
          continue;
        core_->setControl(kModAmountControls[s], get(modAmountIndex(s)) / 100.0f);
      }
      // Matrix LFO settings: only while a slot uses that LFO (otherwise the patch's LFO 5-8 stay as they are).
      int in_use = lfo_in_use_.load();
      for (int i = 0; i < kNumExposed2 && exposed2_first_index_ >= 0; ++i) {
        int index = exposed2_first_index_ + i;
        if ((in_use & (1 << (i / 4))) == 0 || !dirty_[index].exchange(false))
          continue;
        core_->setControl(kExposed2[i].vital, get(index) + offset2_[i]);
      }
    }

    template <typename NoteFn>
    void handleMidi(const mpc::MidiEvent& m, NoteFn& send) {
      int channel = m.channel();
      if (m.isNoteOn()) {
        arp_channel_ = channel;
        if (!arp_.noteOn(m.data[1], m.data[2] / 127.0f, send))
          core_->noteOn(m.data[1], m.data[2] / 127.0f, 0, channel);
      }
      else if (m.isNoteOff()) {
        if (!arp_.noteOff(m.data[1], send))
          core_->noteOff(m.data[1], m.data[2] / 127.0f, 0, channel);
      }
      else if (m.status() == 0xE0) {
        int bend = (m.data[2] << 7) | m.data[1];
        core_->pitchBend((bend - 8192) / 8192.0f);
      }
      else if (m.status() == 0xD0)
        core_->channelPressure(m.data[1] / 127.0f);
      else if (m.status() == 0xB0) {
        int cc = m.data[1];
        int value = m.data[2];
        if (cc == 1)
          core_->modWheel(value / 127.0f);
        else if (cc == 64)
          core_->sustain(value >= 64);
        else if (cc == 120) {
          arp_.reset(send);
          core_->allSoundsOff();
        }
        else if (cc == 123) {
          arp_.reset(send);
          core_->allNotesOff();
        }
      }
    }


    // ===== 1.1: favourites, search, arpeggiator, mod matrix, granulator ===========================
    void addParamsV11() {
      // Search names: lower case, without the two-letter type code.
      search_names_.clear();
      for (const PatchEntry& p : patches_)
        search_names_.push_back(lowerCase(p.name.size() > 3 && p.name[2] == ' ' ? p.name.substr(3) : p.name));

      auto screen = [this](mpc::Param p) {   // not automatable: screen state, never recorded
        p.automatable = false;
        return addParam(p);
      };
      auto withSuffix = [this](int index, const char* suffix, bool sign = false) {
        if (static_cast<int>(suffix_.size()) <= index) {
          suffix_.resize(index + 1);
          signed_.resize(index + 1, false);
        }
        suffix_[index] = suffix;
        signed_[index] = sign;
      };

      // Favourites and search.
      fav_toggle_index_ = screen(mpc::boolParam("fav_toggle", "Favourite", false));
      first_v11_index_ = fav_toggle_index_;
      fav_state_index_ = screen(mpc::choiceParam("fav_state", "Fav State", { "Off", "On" }, 0));
      fav_filter_index_ = screen(mpc::choiceParam("fav_filter", "Fav Filter", { "All", "Favourites" }, 0));
      search_index_ = screen(mpc::floatParam("search", "Search", 0.0f, 1.0f, 0.0f));
      for (int k = 0; k < kNumKeys; ++k) {
        std::string id = "key_" + std::to_string(k + 1);
        std::string name = "Key " + keyLabel(k);
        int index = screen(mpc::boolParam(id.c_str(), name.c_str(), false));
        if (k == 0) key_first_index_ = index;
      }

      // Overlay panels and the header chips.
      std::vector<std::string> panels(kPanels, kPanels + kNumPanels);
      panel_index_ = screen(mpc::choiceParam("panel", "Panel", panels, 0));
      arp_info_index_ = screen(mpc::floatParam("arp_info", "Arp Info", 0.0f, 1.0f, 0.0f));
      grain_info_index_ = screen(mpc::floatParam("grain_info", "Grain Info", 0.0f, 1.0f, 0.0f));
      mod_info_index_ = screen(mpc::floatParam("mod_info", "Mod Info", 0.0f, 1.0f, 0.0f));

      // Arpeggiator (automatable, so you can switch it per sequence).
      arp_first_index_ = addParam(mpc::boolParam("arp_on", "Arp On", false));
      addParam(mpc::choiceParam("arp_mode", "Arp Mode", std::vector<std::string>(kArpModes, kArpModes + Arp::kNumModes), 0));
      addParam(mpc::choiceParam("arp_rate", "Arp Rate", std::vector<std::string>(kArpRates, kArpRates + Arp::kNumRates), 6));
      addParam(mpc::intParam("arp_octaves", "Arp Octaves", 1, 4, 1));
      withSuffix(addParam(mpc::floatParam("arp_gate", "Arp Gate", 5.0f, 100.0f, 50.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("arp_swing", "Arp Swing", 0.0f, 75.0f, 0.0f, "", 0)), "%");
      addParam(mpc::boolParam("arp_latch", "Arp Latch", false));
      addParam(mpc::intParam("arp_length", "Arp Length", 1, Arp::kMaxSteps, Arp::kMaxSteps));
      withSuffix(addParam(mpc::floatParam("arp_accent", "Arp Accent", 0.0f, 100.0f, 50.0f, "", 0)), "%");
      std::vector<std::string> states(kStepStates, kStepStates + Arp::kNumStepStates);
      for (int k = 0; k < Arp::kMaxSteps; ++k) {
        std::string id = "arp_step_" + std::to_string(k + 1), name = "Arp Step " + std::to_string(k + 1);
        int index = addParam(mpc::choiceParam(id.c_str(), name.c_str(), states, Arp::kStepOn));
        if (k == 0) arp_step_first_index_ = index;
      }
      for (int k = 0; k < Arp::kMaxSteps; ++k) {
        std::string id = "arp_tap_" + std::to_string(k + 1), name = "Step Tap " + std::to_string(k + 1);
        int index = screen(mpc::boolParam(id.c_str(), name.c_str(), false));
        if (k == 0) arp_tap_first_index_ = index;
      }
      {
        std::vector<std::string> steps;
        for (int k = 1; k <= Arp::kMaxSteps; ++k)
          steps.push_back(std::to_string(k));
        steps.push_back("-");
        arp_pos_index_ = screen(mpc::choiceParam("arp_pos", "Arp Position", steps, Arp::kMaxSteps));
      }

      // Mod matrix: 16 slots of source, destination, amount.
      std::vector<std::string> sources, destinations;
      for (const Named& n : kModSources) sources.push_back(n.label);
      for (const Named& n : kModDestinations) destinations.push_back(n.label);
      for (int k = 0; k < kNumModSlots; ++k) {
        std::string n = std::to_string(k + 1);
        int index = addParam(mpc::choiceParam(("mod_" + n + "_src").c_str(), ("Mod " + n + " Source").c_str(), sources, 0));
        if (k == 0) mod_first_index_ = index;
        addParam(mpc::choiceParam(("mod_" + n + "_dst").c_str(), ("Mod " + n + " Dest").c_str(), destinations, 0));
        withSuffix(addParam(mpc::floatParam(("mod_" + n + "_amt").c_str(), ("Mod " + n + " Amount").c_str(),
                                            -100.0f, 100.0f, 0.0f, "", 0)), "%", true);
      }
      // The matrix's four LFOs: shape here, timing below (Vital controls of LFO 5-8).
      std::vector<std::string> shapes(kLfoShapes, kLfoShapes + kNumLfoShapes);
      for (int k = 0; k < kNumMatrixLfos; ++k) {
        std::string n = std::to_string(k + 1);
        int index = addParam(mpc::choiceParam(("mlfo_" + n + "_shape").c_str(), ("LFO" + n + " Shape").c_str(), shapes, 0));
        if (k == 0) lfo_shape_first_index_ = index;
      }
      offset2_.assign(kNumExposed2, 0.0f);
      for (int i = 0; i < kNumExposed2; ++i) {
        const vital::ValueDetails d = vital::Parameters::getDetails(kExposed2[i].vital);
        mpc::Param p;
        if (d.value_scale == vital::ValueDetails::kIndexed && d.string_lookup) {
          std::vector<std::string> choices;
          for (int c = static_cast<int>(d.min); c <= static_cast<int>(d.max); ++c)
            choices.push_back(d.string_lookup[c]);
          p = mpc::choiceParam("", "", choices, static_cast<int>(d.default_value - d.min));
          offset2_[i] = d.min;
        }
        else if (d.value_scale == vital::ValueDetails::kIndexed) {
          p = mpc::intParam("", "", static_cast<int>(d.min), static_cast<int>(d.max), static_cast<int>(d.default_value));
        }
        else {
          p = mpc::floatParam("", "", d.min, d.max, d.default_value);
        }
        p.id = std::string("v_") + kExposed2[i].vital;
        p.name = kExposed2[i].label;
        int index = addParam(p);
        if (i == 0) exposed2_first_index_ = index;
      }

      // Granulator.
      grain_first_index_ = addParam(mpc::boolParam("gr_on", "Grain On", false));
      withSuffix(addParam(mpc::floatParam("gr_size", "Grain Size", 10.0f, 500.0f, 120.0f, "", 0)), "ms");
      withSuffix(addParam(mpc::floatParam("gr_scatter", "Grain Scatter", 0.0f, 100.0f, 30.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("gr_space", "Grain Space", 0.0f, 100.0f, 30.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("gr_mix", "Grain Mix", 0.0f, 100.0f, 50.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("gr_density", "Grain Density", 1.0f, 80.0f, 20.0f, "", 0)), "/s");
      withSuffix(addParam(mpc::floatParam("gr_position", "Grain Position", 0.0f, 1000.0f, 80.0f, "", 0)), "ms");
      withSuffix(addParam(mpc::floatParam("gr_pitch", "Grain Pitch", -24.0f, 24.0f, 0.0f, "", 0)), "st", true);
      withSuffix(addParam(mpc::floatParam("gr_pitch_rand", "Grain Pitch Rnd", 0.0f, 24.0f, 0.0f, "", 1)), "st");
      withSuffix(addParam(mpc::floatParam("gr_spread", "Grain Spread", 0.0f, 100.0f, 50.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("gr_reverse", "Grain Reverse", 0.0f, 100.0f, 0.0f, "", 0)), "%");
      withSuffix(addParam(mpc::floatParam("gr_feedback", "Grain Feedback", 0.0f, 90.0f, 0.0f, "", 0)), "%");
      addParam(mpc::boolParam("gr_freeze", "Grain Freeze", false));
      grain_last_index_ = addParam(mpc::choiceParam("gr_sync", "Grain Sync",
                                                    std::vector<std::string>(kGrainSync, kGrainSync + Granulator::kNumSyncRates), 0));

      // Touch scrolling for the patch lists: an invisible strip over the rows (one per list height).
      // Drag = scroll, a single quick touch = load the patch under the finger. "Tap" mode hides the strips,
      // so the row buttons underneath take the touches again.
      list_touch_index_ = screen(mpc::choiceParam("list_touch", "List Touch", { "Drag", "Tap" }, 0));
      drag_index_[0] = screen(mpc::floatParam("list_drag", "List Drag", 0.0f, 1.0f, 0.5f));
      drag_index_[1] = screen(mpc::floatParam("list_drag_page", "List Drag Page", 0.0f, 1.0f, 0.5f));
      suffix_.resize(numParams());
      signed_.resize(numParams(), false);
    }

    // Host thread: one event of a touch on a list strip (which = 0 browser, 1 PATCH page).
    // Host thread: one event of a touch on a list strip (which = 0 browser, 1 PATCH page).
    // The strip reports where the finger is (0 bottom .. 1 top). The list moves by the change between events,
    // so a new touch never pulls the list back: a pause, or a jump bigger than a finger moves in one event,
    // starts a new touch where the finger landed (device, 2026-11-19: quick repeated flicks used to snap back).
    void listDragEvent(int which, float value) {
      std::lock_guard<std::mutex> lock(drag_mutex_);
      int64_t now = nowNs();
      int rows = which == 0 ? kListRows : kListPageRows;
      bool new_touch = now - drag_.last_ns > kGestureGapNs || drag_.which != which ||
                       std::fabs(value - drag_.last_value) > kTouchJump;
      if (new_touch) {
        drag_ = DragGesture();
        drag_.which = which;
        drag_.anchor = value;
        drag_.rows = rows;
        drag_.top = static_cast<float>(list_top_.load());
      }
      else {
        drag_.events++;
        // The list follows the finger: moving up (value up) brings later patches into view.
        int count = static_cast<int>(filteredPatches().size());
        float max_top = static_cast<float>(maxListTop(count));
        drag_.top = std::clamp(drag_.top + (value - drag_.last_value) * rows, 0.0f, max_top);
        int top = static_cast<int>(std::lround(drag_.top));
        if (top != list_top_.load()) {
          list_top_.store(top);
          list_marker_dirty_.store(true);
          display_dirty_.store(true);
          worker_cv_.notify_all();
        }
      }
      drag_.last_value = value;
      drag_.last_ns = now;
    }

    // Worker thread: a gesture that ended after a single event was a tap.
    void checkListTap() {
      int row = -1;
      {
        std::lock_guard<std::mutex> lock(drag_mutex_);
        if (drag_.which < 0 || drag_.done || nowNs() - drag_.last_ns <= kGestureGapNs)
          return;
        drag_.done = true;
        if (drag_.events == 0)
          row = std::clamp(static_cast<int>((1.0f - drag_.anchor) * drag_.rows), 0, drag_.rows - 1);
      }
      if (row < 0)
        return;
      int k = listPatchAt(row);
      if (k >= 0 && k != requested_patch_.load()) {
        setNormalised(kPatch, param(kPatch).toNormalised(static_cast<float>(k)));   // triggers the load
        notifyHostParameterChanged(kPatch);
      }
    }

    static std::string keyLabel(int k) {
      if (k < kNumLetterKeys) return std::string(1, kKeys[k]);
      if (k == kKeySpace) return "SPACE";
      if (k == kKeyDelete) return "DEL";
      return "CLR";
    }

    int modSourceIndex(int s) const { return mod_first_index_ + 3 * s; }
    int modDestIndex(int s) const { return mod_first_index_ + 3 * s + 1; }
    int modAmountIndex(int s) const { return mod_first_index_ + 3 * s + 2; }

    // Momentary screen buttons: act on press, then spring back.
    template <typename F>
    void momentary(int index, float plain, F&& action) {
      if (plain < 0.5f)
        return;
      if (suppress_patch_trigger_.load()) {   // a saved "pressed" value from a restore: just release it
        setNormalised(index, 0.0f);
        return;
      }
      action();
      setNormalised(index, 0.0f);
      notifyHostParameterChanged(index);
    }

    void filterChanged() {
      list_top_.store(0);
      list_follow_.store(true);
      display_dirty_.store(true);
      worker_cv_.notify_all();
    }

    void parameterChangedV11(int index, float plain) {
      if (index == fav_toggle_index_) {
        momentary(index, plain, [this] {
          std::string key = favKey(requested_patch_.load());
          {
            std::lock_guard<std::mutex> lock(filter_mutex_);
            if (!favourites_.erase(key))
              favourites_.insert(key);
          }
          fav_save_.store(true);
          if (fav_only_.load())
            filterChanged();
          display_dirty_.store(true);
          worker_cv_.notify_all();
        });
        return;
      }
      if (index == drag_index_[0] || index == drag_index_[1]) {
        // Only real touches: not a project or state restore, and the browser strip only while the browser is open.
        bool browser_strip = index == drag_index_[0];
        if (getInt(list_touch_index_) == 0 && !suppress_patch_trigger_.load() &&
            (!browser_strip || getInt(browser_index_) != 0))
          listDragEvent(browser_strip ? 0 : 1, plain);
        return;
      }
      if (index == list_touch_index_)
        return;
      if (index == fav_filter_index_) {
        fav_only_.store(plain > 0.5f);
        filterChanged();
        return;
      }
      if (index == fav_state_index_ || index == search_index_ || index == arp_pos_index_ ||
          index == arp_info_index_ || index == grain_info_index_ || index == mod_info_index_)
        return;   // display only
      if (index >= key_first_index_ && index < key_first_index_ + kNumKeys) {
        momentary(index, plain, [this, index] {
          int k = index - key_first_index_;
          {
            std::lock_guard<std::mutex> lock(filter_mutex_);
            if (k < kNumLetterKeys) { if (query_.size() < kMaxQuery) query_ += static_cast<char>(std::tolower(kKeys[k])); }
            else if (k == kKeySpace) { if (!query_.empty() && query_.back() != ' ' && query_.size() < kMaxQuery) query_ += ' '; }
            else if (k == kKeyDelete) { if (!query_.empty()) query_.pop_back(); }
            else query_.clear();
          }
          filterChanged();
        });
        return;
      }
      if (index == panel_index_) {
        if (plain > 0.5f && getInt(browser_index_) != 0) {
          setNormalised(browser_index_, 0.0f);
          notifyHostParameterChanged(browser_index_);
        }
        return;
      }
      if (index >= arp_tap_first_index_ && index < arp_tap_first_index_ + Arp::kMaxSteps) {
        momentary(index, plain, [this, index] {
          int step = arp_step_first_index_ + (index - arp_tap_first_index_);
          set(step, static_cast<float>((getInt(step) + 1) % Arp::kNumStepStates));
          notifyHostParameterChanged(step);
          arp_dirty_.store(true);
        });
        return;
      }
      if (index >= arp_first_index_ && index < arp_step_first_index_ + Arp::kMaxSteps) {
        arp_dirty_.store(true);
        if (index < arp_step_first_index_)
          display_dirty_.store(true);   // the ARP chip's text
        return;
      }
      if (index >= mod_first_index_ && index < mod_first_index_ + 3 * kNumModSlots) {
        if ((index - mod_first_index_) % 3 == 2) {
          dirty_[index].store(true);
          any_dirty_.store(true);
        }
        else {
          mod_routing_dirty_.store(true);
          display_dirty_.store(true);
          worker_cv_.notify_all();
        }
        return;
      }
      if (index >= lfo_shape_first_index_ && index < lfo_shape_first_index_ + kNumMatrixLfos) {
        lfo_shape_dirty_.fetch_or(1 << (index - lfo_shape_first_index_));
        worker_cv_.notify_all();
        return;
      }
      if (index >= exposed2_first_index_ && index < exposed2_first_index_ + kNumExposed2) {
        dirty_[index].store(true);
        any_dirty_.store(true);
        return;
      }
      if (index >= grain_first_index_ && index <= grain_last_index_) {
        grain_dirty_.store(true);
        display_dirty_.store(true);
        return;
      }
    }

    bool formatValueV11(int index, float plain, std::string& out) {
      if (index == search_index_) {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        std::string q = query_;
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        out = q.empty() ? std::string("TYPE TO SEARCH") : q + "_";
        return true;
      }
      if (index >= key_first_index_ && index < key_first_index_ + kNumKeys) {
        out = keyLabel(index - key_first_index_);
        return true;
      }
      if (index == fav_toggle_index_) { out = "FAV"; return true; }
      if (index == arp_info_index_) {
        out = getInt(arp_first_index_) ? std::string(kArpModesShort[getInt(arp_first_index_ + 1)]) + " " +
                                           kArpRates[getInt(arp_first_index_ + 2)]
                                       : std::string("OFF");
        return true;
      }
      if (index == grain_info_index_) {
        if (!getInt(grain_first_index_)) out = "OFF";
        else if (getInt(grain_last_index_ - 1)) out = "FROZEN";
        else out = "MIX " + formatNumber(get(grain_first_index_ + 4), 0) + "%";
        return true;
      }
      if (index == mod_info_index_) {
        int n = activeModSlots();
        out = n == 0 ? std::string("OFF") : std::to_string(n) + (n == 1 ? " SLOT" : " SLOTS");
        return true;
      }
      if (index >= exposed2_first_index_ && index < exposed2_first_index_ + kNumExposed2) {
        const Exposed& x = kExposed2[index - exposed2_first_index_];
        const vital::ValueDetails d = vital::Parameters::getDetails(x.vital);
        if (d.value_scale != vital::ValueDetails::kExponential)
          return false;
        double hz = std::pow(2.0, static_cast<double>(plain));   // LFO frequency: show Hz
        out = formatNumber(hz, hz < 1.0 ? 2 : 1) + "Hz";
        return true;
      }
      if (index < static_cast<int>(suffix_.size()) && !suffix_[index].empty()) {
        const mpc::Param& p = param(index);
        out = p.display(plain);
        if (signed_[index] && plain > 0.0f)
          out = "+" + out;
        if (out == "-0") out = "0";
        out += suffix_[index];
        return true;
      }
      return false;
    }

    int activeModSlots() const {
      int n = 0;
      for (int s = 0; s < kNumModSlots; ++s)
        if (getInt(modSourceIndex(s)) > 0 && getInt(modDestIndex(s)) > 0)
          ++n;
      return n;
    }

    Arp::Settings arpSettings() const {
      Arp::Settings a;
      int i = arp_first_index_;
      a.on = getInt(i) != 0;
      a.mode = getInt(i + 1);
      a.rate = getInt(i + 2);
      a.octaves = getInt(i + 3);
      a.gate = get(i + 4) / 100.0f;
      a.swing = get(i + 5) / 100.0f;
      a.latch = getInt(i + 6) != 0;
      a.length = getInt(i + 7);
      a.accent = get(i + 8) / 100.0f;
      for (int k = 0; k < Arp::kMaxSteps; ++k)
        a.steps[k] = getInt(arp_step_first_index_ + k);
      return a;
    }

    Granulator::Settings grainSettings() const {
      Granulator::Settings g;
      int i = grain_first_index_;
      g.on = getInt(i) != 0;
      g.size_ms = get(i + 1);
      g.scatter = get(i + 2) / 100.0f;
      g.space = get(i + 3) / 100.0f;
      g.mix = get(i + 4) / 100.0f;
      g.density = get(i + 5);
      g.position_ms = get(i + 6);
      g.pitch = get(i + 7);
      g.pitch_rand = get(i + 8);
      g.spread = get(i + 9) / 100.0f;
      g.reverse = get(i + 10) / 100.0f;
      g.feedback = get(i + 11) / 100.0f;
      g.freeze = getInt(i + 12) != 0;
      g.sync = getInt(i + 13);
      return g;
    }

    // Connects the matrix slots and the matrix LFOs in the engine. Caller holds load_mutex_ with audio paused.
    void syncMatrixLocked() {
      int in_use = 0;
      for (int s = 0; s < kNumModSlots; ++s) {
        int src = getInt(modSourceIndex(s));
        int dst = getInt(modDestIndex(s));
        if (src <= 0 || dst <= 0)
          src = dst = 0;
        if (src >= 4 && src <= 7)
          in_use |= 1 << (src - 4);
        if (src != applied_src_[s] || dst != applied_dst_[s]) {
          core_->setUserModulation(s, kModSources[src].vital, kModDestinations[dst].vital, bipolarSource(src));
          applied_src_[s] = src;
          applied_dst_[s] = dst;
        }
      }
      int shapes_due = lfo_shape_dirty_.exchange(0);
      for (int k = 0; k < kNumMatrixLfos; ++k) {
        int bit = 1 << k;
        int lfo = kFirstMatrixLfo + k;
        if ((in_use & bit) && !(lfo_applied_mask_ & bit)) {
          // Taking this LFO over: remember the patch's own settings first.
          for (int c = 0; c < 4; ++c)
            saved_lfo_controls_[k][c] = core_->getControl(kExposed2[4 * k + c].vital);
          core_->setLfoShape(lfo, kLfoShapeToCore[getInt(lfo_shape_first_index_ + k)]);
          for (int c = 0; c < 4; ++c)
            dirty_[exposed2_first_index_ + 4 * k + c].store(true);
          any_dirty_.store(true);
        }
        else if (!(in_use & bit) && (lfo_applied_mask_ & bit)) {
          // Given back: the patch's LFO as it was.
          for (int c = 0; c < 4; ++c)
            core_->setControl(kExposed2[4 * k + c].vital, saved_lfo_controls_[k][c]);
          core_->restoreLfoShape(lfo);
        }
        else if ((in_use & bit) && (shapes_due & bit)) {
          core_->setLfoShape(lfo, kLfoShapeToCore[getInt(lfo_shape_first_index_ + k)]);
        }
      }
      lfo_applied_mask_ = in_use;
      lfo_in_use_.store(in_use);
    }

    // Worker thread, every tick.
    void workerV11() {
      if (mod_routing_dirty_.exchange(false) || lfo_shape_dirty_.load() != 0) {
        std::lock_guard<std::mutex> load_lock(load_mutex_);
        pauseAudio();
        syncMatrixLocked();
        paused_.store(false);   // no markAllDirty: nothing else changed
      }
      if (fav_save_.exchange(false))
        saveFavourites();
      checkListTap();
      // Star in the header: is the shown patch a favourite?
      if (fav_state_index_ >= 0) {
        bool fav;
        {
          std::lock_guard<std::mutex> lock(filter_mutex_);
          fav = favourites_.count(favKey(requested_patch_.load())) != 0;
        }
        if (fav != (getInt(fav_state_index_) != 0)) {
          setNormalised(fav_state_index_, fav ? 1.0f : 0.0f);
          notifyHostParameterChanged(fav_state_index_);
        }
      }
      // Arp play marker, only while the arp panel is open.
      if (arp_pos_index_ >= 0) {
        int step = getInt(panel_index_) == kPanelArp ? arp_step_.load() : -1;
        int want = step < 0 ? Arp::kMaxSteps : step;
        if (want != getInt(arp_pos_index_)) {
          setNormalised(arp_pos_index_, param(arp_pos_index_).toNormalised(static_cast<float>(want)));
          notifyHostParameterChanged(arp_pos_index_);
        }
      }
    }

    // <drive>/Warpline/Favourites.txt next to the user patches, or the plugin folder when that is unknown.
    static fs::path favouritesFile() {
      fs::path plugin_folder(mpc::pluginFolder());
      fs::path parent = plugin_folder.parent_path();
      if (const char* env = std::getenv("WARPLINE_FAVOURITES"))
        return fs::path(env);
      if (parent.filename() == "VST2 Plugins" || parent.filename() == "Synths")
        return parent.parent_path() / "Warpline" / "Favourites.txt";
      return plugin_folder / "Favourites.txt";
    }

    void loadFavourites() {
      std::string text;
      if (!mpc::readTextFile(favouritesFile().string(), text))
        return;
      std::lock_guard<std::mutex> lock(filter_mutex_);
      size_t start = 0;
      while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty())
          favourites_.insert(line);
        start = end + 1;
      }
    }

    // Worker thread. Written as .new then renamed, so a power cut never leaves half a file.
    void saveFavourites() {
      std::string text;
      {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        for (const std::string& f : favourites_)
          text += f + "\n";
      }
      fs::path file = favouritesFile();
      std::error_code ec;
      fs::create_directories(file.parent_path(), ec);
      fs::path tmp = file;
      tmp += ".new";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
          return;
        out << text;
        if (!out)
          return;
      }
      fs::rename(tmp, file, ec);
    }

    std::unique_ptr<VitalCore> core_;
    std::vector<float> offset_;            // Vital value = plain value + offset (choices that start above 0)
    std::vector<PatchEntry> patches_;
    std::unique_ptr<std::atomic<bool>[]> dirty_;
    std::atomic<bool> any_dirty_{ false };

    std::atomic<bool> paused_{ false };
    std::atomic<bool> in_process_{ false };

    std::mutex load_mutex_;              // serialises loads (never taken on the audio thread)
    std::string current_patch_text_;
    std::string current_patch_path_;
    std::atomic<int> loaded_patch_{ 0 };
    int patch_polyphony_ = 8;
    int patch_oversampling_ = 1;

    std::atomic<int> requested_patch_{ 0 };
    std::atomic<int64_t> last_patch_request_ns_{ 0 };
    std::atomic<bool> quality_dirty_{ false };
    std::atomic<bool> suppress_patch_trigger_{ false };
    std::atomic<bool> stepping_{ false };
    int prev_index_ = -1;
    int next_index_ = -1;
    int category_index_ = -1;
    int live_view_index_ = -1;
    std::array<int, kListRows> list_row_index_ {};
    int list_scroll_index_ = -1;
    int list_current_index_ = -1;
    int browser_index_ = -1;
    std::atomic<int> list_top_ { 0 };
    std::atomic<bool> list_follow_ { true };
    std::atomic<bool> list_marker_dirty_ { false };
    std::atomic<bool> list_scroll_from_plugin_ { false };
    std::atomic<bool> display_dirty_ { false };
    int live_first_index_ = -1;
    std::array<int, 5> live_base_index_ {};
    std::atomic<bool> live_view_ { false };
    std::atomic<int64_t> live_hold_until_ns_ { 0 };
    int factory_count_ = 1;
    std::atomic<int> category_{ 0 };


    // 1.1 state.
    int first_v11_index_ = -1;
    int fav_toggle_index_ = -1, fav_state_index_ = -1, fav_filter_index_ = -1, search_index_ = -1, key_first_index_ = -1;
    int panel_index_ = -1, arp_info_index_ = -1, grain_info_index_ = -1, mod_info_index_ = -1;
    int arp_first_index_ = -1, arp_step_first_index_ = -1, arp_tap_first_index_ = -1, arp_pos_index_ = -1;
    int mod_first_index_ = -1, lfo_shape_first_index_ = -1, exposed2_first_index_ = -1;
    int grain_first_index_ = -1, grain_last_index_ = -1;
    std::vector<std::string> suffix_;
    std::vector<bool> signed_;
    static constexpr int64_t kGestureGapNs = 250LL * 1000 * 1000;
    static constexpr float kTouchJump = 0.2f;   // more than this between two events = a new touch
    struct DragGesture {
      int which = -1;
      float anchor = 0.0f;
      float top = 0.0f;        // list position, fractional, while dragging
      float last_value = -10.0f;
      int rows = kListRows;
      int events = 0;          // events after the first
      int64_t last_ns = 0;
      bool done = false;
    };
    std::mutex drag_mutex_;
    DragGesture drag_;
    int list_touch_index_ = -1;
    std::array<int, 2> drag_index_ { -1, -1 };
    std::vector<float> offset2_;
    std::vector<std::string> search_names_;
    mutable std::mutex filter_mutex_;
    std::string query_;
    std::set<std::string> favourites_;
    std::atomic<bool> fav_only_{ false };
    std::atomic<bool> fav_save_{ false };
    Arp arp_;
    int arp_channel_ = 0;
    std::atomic<bool> arp_dirty_{ true };
    std::atomic<int> arp_step_{ -1 };
    Granulator granulator_;
    std::atomic<bool> grain_dirty_{ true };
    std::atomic<bool> mod_routing_dirty_{ true };
    std::atomic<int> lfo_shape_dirty_{ 0 };
    std::atomic<int> lfo_in_use_{ 0 };
    int lfo_applied_mask_ = 0;
    std::array<int, kNumModSlots> applied_src_ {};
    std::array<int, kNumModSlots> applied_dst_ {};
    std::array<std::array<float, 4>, kNumMatrixLfos> saved_lfo_controls_ {};

    std::mutex status_mutex_;
    std::string status_text_ = "OK";
    std::vector<std::string> status_notes_;

    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    bool quit_ = false;
};

MPC_PLUGIN_ENTRY(Warpline)
