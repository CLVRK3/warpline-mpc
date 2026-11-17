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
  loadInit();
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

  for (auto& control : controls_) {
    if (vital::Parameters::isParameter(control.first))
      control.second->set(vital::Parameters::getDetails(control.first).default_value);
  }
  tuning_->setDefaultTuning();
  engine_->checkOversampling();
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

void VitalCore::clearModulations() {
  vital::ModulationConnectionBank& bank = engine_->getModulationBank();
  for (int index : connected_modulations_) {
    vital::ModulationConnection* connection = bank.atIndex(index);
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
  }
  connected_modulations_.clear();

  int num_connections = static_cast<int>(bank.numConnections());
  for (int i = 0; i < num_connections; ++i)
    bank.atIndex(i)->modulation_processor->lineMapGenerator()->initLinear();
  engine_->disableUnnecessaryModSources();
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
        if (index >= vital::kMaxModulationConnections)
          break;
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

    // LFO shapes.
    if (settings.count("lfos")) {
      int i = 0;
      for (const json& lfo : settings["lfos"]) {
        if (i >= vital::kNumLfos)
          break;
        LineGenerator* source = engine_->getLfoSource(i);
        source->jsonToState(lfo);
        source->render();
        i++;
      }
    }

    // Tuning stored with the patch, if any.
    if (settings.count("tuning"))
      tuning_->jsonToState(settings["tuning"]);
    else
      tuning_->setDefaultTuning();

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
    modulation_data["source"] = connection->source_name;
    modulation_data["destination"] = connection->destination_name;
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
