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
#include <string>
#include <thread>
#include <vector>

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
      info.version = 1000;
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

      dirty_ = std::make_unique<std::atomic<bool>[]>(numParams());
      for (int i = 0; i < numParams(); ++i)
        dirty_[i].store(false);

      // Every patch is also a program: the MPC preset list becomes the patch browser.
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        addProgram(trimName(patches_[i].name, 24).c_str(), { static_cast<float>(i) });

      core_ = std::make_unique<VitalCore>();
      core_->prepare(44100.0);
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
      resumeAudio();
    }

    void reset() override {
      // Audio thread (resume, or after a state load). Stops hanging notes.
      if (!paused_.load())
        core_->allSoundsOff();
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

      if (ctx.transport.valid && ctx.transport.tempoValid && ctx.transport.bpm > 0.0)
        core_->setBpm(static_cast<float>(ctx.transport.bpm));
      if (ctx.transport.valid && ctx.transport.playing && ctx.transport.ppqValid && ctx.transport.bpm > 0.0)
        core_->setTime(ctx.transport.ppqPos * 60.0 / ctx.transport.bpm);

      // Render between MIDI events so notes land on the right sample.
      int pos = 0;
      int e = 0;
      while (pos < ctx.numFrames) {
        while (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset <= pos)
          handleMidi(ctx.midi[e++]);
        int next = ctx.numFrames;
        if (e < ctx.numMidiEvents)
          next = std::min(next, std::max(pos + 1, ctx.midi[e].sampleOffset));
        core_->render(left + pos, right + pos, next - pos);
        pos = next;
      }
      while (e < ctx.numMidiEvents)
        handleMidi(ctx.midi[e++]);

      if (right == left) {
        // Mono output: nothing more to do (left already holds the left channel).
      }
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
      if (index == browser_index_ || index == list_current_index_)
        return;   // screen state only
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
      markAllDirty();
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
      int filter = category_.load();   // 0 = all, else type + 1
      for (int k = 1; k <= n; ++k) {
        int i = ((from + dir * k) % n + n) % n;
        if (filter == 0 || patches_[i].type == filter - 1)
          return i;
      }
      return from;
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
      int filter = category_.load();
      for (int i = 0; i < static_cast<int>(patches_.size()); ++i)
        if (filter == 0 || patches_[i].type == filter - 1)
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
    }

    void handleMidi(const mpc::MidiEvent& m) {
      int channel = m.channel();
      if (m.isNoteOn())
        core_->noteOn(m.data[1], m.data[2] / 127.0f, 0, channel);
      else if (m.isNoteOff())
        core_->noteOff(m.data[1], m.data[2] / 127.0f, 0, channel);
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
        else if (cc == 120)
          core_->allSoundsOff();
        else if (cc == 123)
          core_->allNotesOff();
      }
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

    std::mutex status_mutex_;
    std::string status_text_ = "OK";
    std::vector<std::string> status_notes_;

    std::thread worker_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    bool quit_ = false;
};

MPC_PLUGIN_ENTRY(Warpline)
