// VitalCore.cpp - see VitalCore.h. Part of Warpline (GPLv3).

#include "QuietVital.h"
#include "VitalCore.h"

#include <algorithm>

#include "sound_engine.h"
#include "synth_parameters.h"
#include "synth_types.h"
#include "synth_lfo.h"
#include "line_generator.h"
#include "sample_source.h"
#include "tuning.h"
#include "wavetable_creator.h"
#include "wavetable_group.h"
#include "wave_source.h"
#include "wave_frame.h"
#include "value.h"

#include <cmath>

VitalCore::VitalCore() {
  engine_ = std::make_unique<vital::SoundEngine>();
  tuning_ = std::make_unique<Tuning>();
  engine_->setTuning(tuning_.get());

  for (int i = 0; i < kNumOscillators; ++i) {
    vital::Wavetable* wavetable = engine_->getWavetable(i);
    if (wavetable) {
      wavetable_creators_[i] = std::make_unique<WavetableCreator>(wavetable);
      wavetable_creators_[i]->init();
    }
  }

  vital::control_map controls = engine_->getControls();
  controls_.assign(controls.begin(), controls.end());   // std::map is already sorted
  for (int i = 0; i < kNumGen; ++i) {
    vital::Output* out = engine_->getModulationSource("gen_" + std::to_string(i + 1));
    gen_[i] = out ? dynamic_cast<vital::cr::Value*>(out->owner) : nullptr;
  }
  loadInit();
}

void VitalCore::setGenValue(int index, float value) {
  if (index >= 0 && index < kNumGen && gen_[index])
    gen_[index]->set(std::clamp(value, 0.0f, 1.0f));
}

void VitalCore::rememberPatchTables() {
  for (int i = 0; i < kNumOscillators; ++i) {
    patch_tables_[i] = wavetable_creators_[i] ? wavetable_creators_[i]->stateToJson().dump() : std::string();
    table_choice_[i] = kTablePatch;
  }
}

// ----- built-in wavetables -----------------------------------------------------------------------
// Each one is a list of single-cycle frames (2048 samples) spread over Vital's 257 frame positions;
// Vital interpolates between them and band-limits on playback. My own generators, no samples.
namespace {
  constexpr int kFrame = vital::WaveFrame::kWaveformSize;
  constexpr double kTau = 6.283185307179586;

  double polyNaive(int shape, double t, double k) {   // k = position in the table 0..1
    switch (shape) {
      case VitalCore::kTableSine: return std::sin(kTau * t);
      case VitalCore::kTableTriangle: return t < 0.25 ? 4.0 * t : (t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0);
      case VitalCore::kTableSaw: return 1.0 - 2.0 * t;
      case VitalCore::kTableSquare: return t < 0.5 ? 1.0 : -1.0;
      case VitalCore::kTablePwm: {            // pulse width 50% -> 4%
        double width = 0.5 - 0.46 * k;
        return t < width ? 1.0 : -1.0;
      }
      case VitalCore::kTableHarmonics: {      // adds harmonics one by one: sine -> bright saw (32 partials)
        double partials = 1.0 + 31.0 * k;
        double v = 0.0;
        for (int h = 1; h <= 32; ++h) {
          double g = std::clamp(partials - (h - 1), 0.0, 1.0);
          if (g <= 0.0) break;
          v += g * std::sin(kTau * h * t) / h;
        }
        return v;
      }
      case VitalCore::kTableFormant: {        // a saw through two vowel formants, A > E > I > O > U
        static const double kF1[] = { 800, 400, 300, 450, 330 }, kF2[] = { 1150, 2000, 2300, 800, 700 };
        double x = k * 4.0;
        int a = std::min(3, static_cast<int>(x));
        double f = x - a;
        double f1 = kF1[a] + (kF1[a + 1] - kF1[a]) * f, f2 = kF2[a] + (kF2[a + 1] - kF2[a]) * f;
        const double base = 110.0;   // the frame is one cycle of a 110 Hz note
        double v = 0.0;
        for (int h = 1; h <= 64; ++h) {
          double hz = base * h;
          double g = 1.0 / (1.0 + std::pow((hz - f1) / 90.0, 2)) + 0.6 / (1.0 + std::pow((hz - f2) / 130.0, 2));
          v += g * std::sin(kTau * h * t) / std::sqrt(static_cast<double>(h));
        }
        return v;
      }
      case VitalCore::kTableSync: {           // hard sync: a saw at 1x -> 7x the cycle, reset every cycle
        double ratio = 1.0 + 6.0 * k;
        double ph = t * ratio;
        ph -= std::floor(ph);
        return 1.0 - 2.0 * ph;
      }
      default: return 0.0;
    }
  }

  int framesFor(int table) {
    switch (table) {
      case VitalCore::kTableSine: case VitalCore::kTableTriangle: case VitalCore::kTableSaw: case VitalCore::kTableSquare:
        return 1;
      case VitalCore::kTableHarmonics: return 32;
      case VitalCore::kTableFormant: return 17;
      default: return 65;
    }
  }

  const char* tableName(int table) {
    static const char* const kNames[] = { "Patch", "Basic Shapes", "Sine", "Triangle", "Saw", "Square", "PWM",
                                          "Harmonics", "Formant", "Sync" };
    return kNames[std::clamp(table, 0, VitalCore::kNumTables - 1)];
  }

  void buildTable(WavetableCreator& creator, int table) {
    if (table == VitalCore::kTableShapes) {
      creator.initPredefinedWaves();
      creator.setName(tableName(table));
      return;
    }
    creator.clear();
    WavetableGroup* group = new WavetableGroup();
    WaveSource* source = new WaveSource();
    int frames = framesFor(table);
    std::vector<float> buffer(kFrame);
    for (int f = 0; f < frames; ++f) {
      double k = frames > 1 ? static_cast<double>(f) / (frames - 1) : 0.0;
      int position = frames > 1 ? static_cast<int>(std::lround(k * (vital::kNumOscillatorWaveFrames - 1))) : 0;
      double peak = 1e-9;
      for (int i = 0; i < kFrame; ++i) {
        buffer[i] = static_cast<float>(polyNaive(table, static_cast<double>(i) / kFrame, k));
        peak = std::max(peak, static_cast<double>(std::fabs(buffer[i])));
      }
      for (float& v : buffer)
        v = static_cast<float>(v / peak);
      source->insertNewKeyframe(position);
      WaveSourceKeyframe* keyframe = source->getKeyframe(f);
      keyframe->wave_frame()->loadTimeDomain(buffer.data());
    }
    // Pulse widths and sync ratios cross-fade badly in the spectrum: move through them in time instead.
    source->setInterpolationStyle(frames > 1 ? WavetableComponent::kLinear : WavetableComponent::kNone);
    if (table == VitalCore::kTablePwm || table == VitalCore::kTableSync)
      source->setInterpolationMode(WaveSource::kTime);
    group->addComponent(source);
    creator.addGroup(group);
    creator.setName(tableName(table));
    creator.render();
  }
} // namespace

void VitalCore::setWavetable(int osc, int table) {
  if (osc < 0 || osc >= kNumOscillators || !wavetable_creators_[osc])
    return;
  table = std::clamp(table, 0, kNumTables - 1);
  if (table == table_choice_[osc])
    return;
  table_choice_[osc] = table;
  WavetableCreator& creator = *wavetable_creators_[osc];
  if (table == kTablePatch) {
    try {
      if (patch_tables_[osc].empty())
        creator.init();
      else {
        creator.jsonToState(json::parse(patch_tables_[osc]));
        creator.render();
      }
    }
    catch (const std::exception&) {
      creator.init();
    }
    return;
  }
  buildTable(creator, table);
}

VitalCore::~VitalCore() = default;

vital::Value* VitalCore::findControl(const std::string& name) const {
  auto it = std::lower_bound(controls_.begin(), controls_.end(), name,
                             [](const std::pair<std::string, vital::Value*>& a, const std::string& b) {
                               return a.first < b;
                             });
  if (it == controls_.end() || it->first != name)
    return nullptr;
  return it->second;
}

bool VitalCore::hasControl(const std::string& name) const { return findControl(name) != nullptr; }

void VitalCore::setControl(const std::string& name, float value) {
  if (vital::Value* v = findControl(name))
    v->set(value);
}

float VitalCore::getControl(const std::string& name) const {
  if (vital::Value* v = findControl(name))
    return v->value();
  return 0.0f;
}

void VitalCore::loadInit() {
  engine_->allSoundsOff();
  for (int i = 0; i < kNumOscillators; ++i)
    if (wavetable_creators_[i])
      wavetable_creators_[i]->init();
  engine_->getSample()->init();

  for (int i = 0; i < vital::kNumLfos; ++i)
    engine_->getLfoSource(i)->initTriangle();
  patch_lfos_.clear();

  for (auto& control : controls_) {
    if (vital::Parameters::isParameter(control.first))
      control.second->set(vital::Parameters::getDetails(control.first).default_value);
  }
  tuning_->setDefaultTuning();
  clearModulations();
  connectUserModulations();
  engine_->checkOversampling();
  rememberPatchTables();
}

void VitalCore::prepare(double sample_rate) {
  sample_rate_ = sample_rate;
  engine_->setSampleRate(static_cast<int>(sample_rate));
  engine_->updateAllModulationSwitches();
  engine_->checkOversampling();
  warmUp();
}

void VitalCore::noteOn(int note, float velocity, int sample, int channel) {
  engine_->noteOn(note, velocity, sample, channel);
}

void VitalCore::noteOff(int note, float lift, int sample, int channel) {
  engine_->noteOff(note, lift, sample, channel);
}

void VitalCore::allNotesOff() { engine_->allNotesOff(0); }
void VitalCore::allSoundsOff() { engine_->allSoundsOff(); }

void VitalCore::pitchBend(float value) {
  engine_->setZonedPitchWheel(value, 0, vital::kNumMidiChannels - 1);
}

void VitalCore::modWheel(float value) { engine_->setModWheelAllChannels(value); }

void VitalCore::channelPressure(float value) {
  engine_->setChannelRangeAftertouch(0, vital::kNumMidiChannels - 1, value, 0);
}

void VitalCore::sustain(bool on) {
  if (on)
    engine_->sustainOnRange(0, vital::kNumMidiChannels - 1);
  else
    engine_->sustainOffRange(0, 0, vital::kNumMidiChannels - 1);
}

void VitalCore::setBpm(float bpm) { engine_->setBpm(bpm); }
void VitalCore::setTime(double seconds) { time_ = seconds; }

void VitalCore::render(float* left, float* right, int num_frames) {
  const double sample_time = 1.0 / sample_rate_;
  for (int offset = 0; offset < num_frames;) {
    int n = std::min(num_frames - offset, vital::kMaxBufferSize);
    engine_->correctToTime(time_);
    engine_->process(n);

    const vital::mono_float* out = reinterpret_cast<const vital::mono_float*>(engine_->output(0)->buffer);
    for (int i = 0; i < n; ++i) {
      left[offset + i] = out[vital::poly_float::kSize * i];
      right[offset + i] = out[vital::poly_float::kSize * i + 1];
    }
    time_ += n * sample_time;
    offset += n;
  }
}

int VitalCore::activeVoices() { return engine_->getNumActiveVoices(); }

void VitalCore::warmUp() {
  float left[vital::kMaxBufferSize], right[vital::kMaxBufferSize];
  double time = time_;
  for (int i = 0; i < 16; ++i)
    render(left, right, vital::kMaxBufferSize);
  time_ = time;
}

// ---------------------------------------------------------------------------------------------
// Patches. Adapted from Vital's LoadSave::jsonToState and SynthBase modulation handling.
// ---------------------------------------------------------------------------------------------

#include "json/json.h"
#include "load_save.h"
#include "modulation_connection_processor.h"

using json = nlohmann::json;

void VitalCore::disconnectIndex(int bank_index) {
  auto it = std::find(connected_modulations_.begin(), connected_modulations_.end(), bank_index);
  if (it == connected_modulations_.end())
    return;
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  vital::ModulationConnection* connection = bank.atIndex(bank_index);
  vital::modulation_change change;
  change.source = engine_->getModulationSource(connection->source_name);
  change.mono_destination = engine_->getMonoModulationDestination(connection->destination_name);
  change.mono_modulation_switch = engine_->getMonoModulationSwitch(connection->destination_name);
  change.destination_scale = vital::Parameters::getParameterRange(connection->destination_name);
  change.poly_modulation_switch = engine_->getPolyModulationSwitch(connection->destination_name);
  change.poly_destination = engine_->getPolyModulationDestination(connection->destination_name);
  change.modulation_processor = connection->modulation_processor.get();
  change.disconnecting = true;
  engine_->disconnectModulation(change);
  connection->source_name = "";
  connection->destination_name = "";
  connected_modulations_.erase(it);
}

void VitalCore::clearModulations() {
  std::vector<int> connected = connected_modulations_;
  for (int index : connected)
    disconnectIndex(index);
  connected_modulations_.clear();

  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  int num_connections = static_cast<int>(bank.numConnections());
  for (int i = 0; i < num_connections; ++i)
    bank.atIndex(i)->modulation_processor->lineMapGenerator()->initLinear();
  engine_->disableUnnecessaryModSources();
}

void VitalCore::connectUserModulations() {
  for (int s = 0; s < kNumUserModulations; ++s) {
    const UserModulation& m = user_modulations_[s];
    int index = kFirstUserModulation + s;
    if (m.source.empty() || m.destination.empty())
      continue;
    if (connectModulation(index, m.source, m.destination))
      setControl("modulation_" + std::to_string(index + 1) + "_bipolar", m.bipolar ? 1.0f : 0.0f);
  }
  engine_->updateAllModulationSwitches();
}

void VitalCore::setUserModulation(int slot, const std::string& source, const std::string& destination, bool bipolar) {
  if (slot < 0 || slot >= kNumUserModulations)
    return;
  int index = kFirstUserModulation + slot;
  UserModulation& m = user_modulations_[slot];
  if (m.source == source && m.destination == destination && m.bipolar == bipolar)
    return;
  disconnectIndex(index);
  m.source = source;
  m.destination = destination;
  m.bipolar = bipolar;
  if (!source.empty() && !destination.empty() && connectModulation(index, source, destination))
    setControl("modulation_" + std::to_string(index + 1) + "_bipolar", bipolar ? 1.0f : 0.0f);
  // Vital's disableUnnecessaryModSources() switches every LFO, envelope and random source off; connecting
  // switches a source on. So switch off the ones the old routing used, then on again every source still
  // connected (the patch's own and the other slots).
  engine_->disableUnnecessaryModSources();
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  for (int connected : connected_modulations_)
    engine_->enableModSource(bank.atIndex(connected)->source_name);
  engine_->updateAllModulationSwitches();
}

void VitalCore::restoreLfoShape(int lfo) {
  if (lfo < 0 || lfo >= vital::kNumLfos)
    return;
  LineGenerator* g = engine_->getLfoSource(lfo);
  if (lfo < static_cast<int>(patch_lfos_.size()) && !patch_lfos_[lfo].empty()) {
    try {
      g->jsonToState(json::parse(patch_lfos_[lfo]));
    }
    catch (const std::exception&) {
      g->initTriangle();
    }
  }
  else {
    g->initTriangle();
  }
  g->render();
}

bool VitalCore::connectModulation(int bank_index, const std::string& source, const std::string& destination) {
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  vital::ModulationConnection* connection = bank.atIndex(bank_index);
  connection->source_name = source;
  connection->destination_name = destination;

  vital::modulation_change change;
  change.source = engine_->getModulationSource(source);
  change.mono_destination = engine_->getMonoModulationDestination(destination);
  change.mono_modulation_switch = engine_->getMonoModulationSwitch(destination);
  if (change.source == nullptr || change.mono_destination == nullptr || change.mono_modulation_switch == nullptr) {
    connection->source_name = "";
    connection->destination_name = "";
    return false;
  }
  change.destination_scale = vital::Parameters::getParameterRange(destination);
  change.poly_modulation_switch = engine_->getPolyModulationSwitch(destination);
  change.poly_destination = engine_->getPolyModulationDestination(destination);
  change.modulation_processor = connection->modulation_processor.get();

  if (change.poly_destination && change.poly_destination->router() == change.modulation_processor) {
    connection->source_name = "";
    connection->destination_name = "";
    return false;
  }

  int num_audio_rate = 0;
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    if (bank.atIndex(i)->source_name == source && bank.atIndex(i)->destination_name != destination &&
        !bank.atIndex(i)->modulation_processor->isControlRate()) {
      num_audio_rate++;
    }
  }
  change.num_audio_rate = num_audio_rate;
  change.disconnecting = false;
  engine_->connectModulation(change);
  connected_modulations_.push_back(bank_index);
  return true;
}

bool VitalCore::loadPatch(const std::string& text, PatchReport& report) {
  report = PatchReport();
  json data;
  try {
    data = json::parse(text, nullptr);
  }
  catch (const json::exception&) {
    report.error = "Not a valid .vital file";
    return false;
  }

  try {
    if (!data.is_object() || !data.count("settings")) {
      report.error = "Not a Vital patch";
      return false;
    }
    std::string version = data.count("synth_version") ? data["synth_version"].get<std::string>() : "0.0.0";
    if (LoadSave::compareFeatureVersionStrings(version, ProjectInfo::versionString) > 0)
      report.notes.push_back("Made with a newer Vital (" + version + ")");

    if (LoadSave::compareVersionStrings(version, ProjectInfo::versionString) < 0 ||
        data["settings"].count("sub_octave")) {
      data = LoadSave::updateFromOldVersion(data);
    }

    if (data.count("preset_name") && data["preset_name"].is_string())
      report.name = data["preset_name"].get<std::string>();
    if (data.count("author") && data["author"].is_string())
      report.author = data["author"].get<std::string>();
    if (data.count("preset_style") && data["preset_style"].is_string())
      report.style = data["preset_style"].get<std::string>();

    json settings = data["settings"];
    engine_->allSoundsOff();

    // Controls: everything the patch has, defaults for the rest (LoadSave::loadControls).
    for (auto& control : controls_) {
      const std::string& name = control.first;
      if (settings.count(name) && settings[name].is_number())
        control.second->set(settings[name].get<float>());
      else if (vital::Parameters::isParameter(name))
        control.second->set(vital::Parameters::getDetails(name).default_value);
    }
    engine_->setModWheelAllChannels(getControl("mod_wheel"));

    // Modulations (LoadSave::loadModulations).
    clearModulations();
    if (settings.count("modulations")) {
      int index = 0;
      int dropped = 0;
      for (const json& modulation : settings["modulations"]) {
        if (index >= kFirstUserModulation) {
          if (!modulation.value("source", "").empty())
            dropped++;
          continue;
        }
        std::string source = modulation.value("source", "");
        std::string destination = modulation.value("destination", "");
        vital::ModulationConnection* connection = engine_->getModulationBank().atIndex(index);
        int this_index = index++;
        if (source.empty() || destination.empty())
          continue;
        if (!connectModulation(this_index, source, destination)) {
          dropped++;
          continue;
        }
        if (modulation.count("line_mapping"))
          connection->modulation_processor->lineMapGenerator()->jsonToState(modulation["line_mapping"]);
        else
          connection->modulation_processor->lineMapGenerator()->initLinear();
      }
      if (dropped)
        report.notes.push_back(std::to_string(dropped) + " modulation(s) skipped");
    }

    // Sample oscillator.
    if (engine_->getSample()) {
      if (settings.count("sample"))
        engine_->getSample()->jsonToState(settings["sample"]);
      else
        engine_->getSample()->init();   // no sample stored: Vital's default (white noise)
    }

    // Wavetables (rendered from the data stored inside the patch).
    if (settings.count("wavetables")) {
      int i = 0;
      for (const json& wavetable : settings["wavetables"]) {
        if (i >= kNumOscillators || !wavetable_creators_[i])
          break;
        wavetable_creators_[i]->jsonToState(wavetable);
        wavetable_creators_[i]->render();
        i++;
      }
    }
    rememberPatchTables();

    // LFO shapes.
    patch_lfos_.assign(vital::kNumLfos, std::string());
    if (settings.count("lfos")) {
      int i = 0;
      for (const json& lfo : settings["lfos"]) {
        if (i >= vital::kNumLfos)
          break;
        LineGenerator* source = engine_->getLfoSource(i);
        source->jsonToState(lfo);
        source->render();
        patch_lfos_[i] = lfo.dump();
        i++;
      }
    }

    // Tuning stored with the patch, if any.
    if (settings.count("tuning"))
      tuning_->jsonToState(settings["tuning"]);
    else
      tuning_->setDefaultTuning();

    connectUserModulations();
    engine_->updateAllModulationSwitches();
    engine_->checkOversampling();
  }
  catch (const std::exception& e) {
    report.error = "Patch could not be read";
    return false;
  }

  report.ok = true;
  return true;
}

void VitalCore::applyLimits(int max_unison, int max_polyphony, int oversampling, PatchReport* report) {
  for (int i = 1; i <= kNumOscillators; ++i) {
    std::string name = "osc_" + std::to_string(i) + "_unison_voices";
    if (!getControl("osc_" + std::to_string(i) + "_on"))
      continue;
    int voices = static_cast<int>(getControl(name) + 0.5f);
    if (voices > max_unison) {
      setControl(name, static_cast<float>(max_unison));
      if (report)
        report->notes.push_back("Osc " + std::to_string(i) + " unison " + std::to_string(voices) + " > " +
                                std::to_string(max_unison));
    }
  }
  int poly = static_cast<int>(getControl("polyphony") + 0.5f);
  if (poly > max_polyphony) {
    setControl("polyphony", static_cast<float>(max_polyphony));
    if (report)
      report->notes.push_back("Polyphony " + std::to_string(poly) + " > " + std::to_string(max_polyphony));
  }
  setControl("oversampling", static_cast<float>(oversampling));
  engine_->checkOversampling();
}

std::string VitalCore::savePatch(const std::string& name, const std::string& author, const std::string& style,
                                 const std::string& comments, const std::vector<std::string>& macro_names) {
  json settings_data;
  for (auto& control : controls_)
    settings_data[control.first] = control.second->value();

  if (engine_->getSample())
    settings_data["sample"] = engine_->getSample()->stateToJson();

  json modulations;
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    vital::ModulationConnection* connection = bank.atIndex(i);
    json modulation_data;
    bool own = connection->source_name.rfind("gen_", 0) == 0;   // Warpline-only sources: not in a .vital file
    modulation_data["source"] = own ? std::string() : connection->source_name;
    modulation_data["destination"] = own ? std::string() : connection->destination_name;
    LineGenerator* line_mapping = connection->modulation_processor->lineMapGenerator();
    if (!line_mapping->linear())
      modulation_data["line_mapping"] = line_mapping->stateToJson();
    modulations.push_back(modulation_data);
  }
  settings_data["modulations"] = modulations;

  json wavetables;
  for (int i = 0; i < kNumOscillators; ++i)
    wavetables.push_back(wavetable_creators_[i]->stateToJson());
  settings_data["wavetables"] = wavetables;

  json lfos;
  for (int i = 0; i < vital::kNumLfos; ++i)
    lfos.push_back(engine_->getLfoSource(i)->stateToJson());
  settings_data["lfos"] = lfos;

  json data;
  data["synth_version"] = ProjectInfo::versionString;
  data["preset_name"] = name;
  data["author"] = author;
  data["comments"] = comments;
  data["preset_style"] = style;
  for (int i = 0; i < vital::kNumMacros; ++i)
    data["macro" + std::to_string(i + 1)] = i < static_cast<int>(macro_names.size()) ? macro_names[i] : "";
  data["settings"] = settings_data;
  return data.dump();
}

bool VitalCore::loadWavetable(int osc, const std::string& vitaltable_text) {
  if (osc < 0 || osc >= kNumOscillators || !wavetable_creators_[osc])
    return false;
  try {
    json data = json::parse(vitaltable_text, nullptr);
    wavetable_creators_[osc]->jsonToState(data);
    wavetable_creators_[osc]->render();
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool VitalCore::addModulation(const std::string& source, const std::string& destination, float amount,
                              bool bipolar) {
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  int index = -1;
  for (int i = 0; i < vital::kMaxModulationConnections; ++i) {
    if (bank.atIndex(i)->source_name.empty() && bank.atIndex(i)->destination_name.empty()) {
      index = i;
      break;
    }
  }
  if (index < 0 || !connectModulation(index, source, destination))
    return false;
  std::string prefix = "modulation_" + std::to_string(index + 1);
  setControl(prefix + "_amount", amount);
  setControl(prefix + "_bipolar", bipolar ? 1.0f : 0.0f);
  engine_->updateAllModulationSwitches();
  return true;
}

void VitalCore::setLfoShape(int lfo, int shape) {
  if (lfo < 0 || lfo >= vital::kNumLfos)
    return;
  LineGenerator* g = engine_->getLfoSource(lfo);
  switch (shape) {
    case 1: g->initSin(); break;
    case 2: g->initSquare(); break;
    case 3: g->initSawUp(); break;
    case 4: g->initSawDown(); break;
    default: g->initTriangle(); break;
  }
  g->render();
}

float VitalCore::liveNormalised(const std::string& name) {
  const vital::ValueDetails d = vital::Parameters::getDetails(name);
  float range = d.max - d.min;
  if (range <= 0.0f)
    return 0.0f;
  float v = (getControl(name) - d.min) / range;
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  for (int index : connected_modulations_) {
    if (bank.atIndex(index)->destination_name != name)
      continue;
    const vital::StatusOutput* amount = engine_->getStatusOutput("modulation_amount_" + std::to_string(index + 1));
    if (amount == nullptr)
      continue;
    float m = amount->value()[0];
    if (!amount->isClearValue(m))
      v += m;
  }
  return std::min(1.0f, std::max(0.0f, v));
}
