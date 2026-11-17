/* Copyright 2013-2019 Matt Tytel
 * Modified 2026 for the MPC edition: file loading (Scala/.tun/.kbm) removed.
 *
 * vital is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * vital is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with vital.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tuning.h"

#include "utils.h"

namespace {
  constexpr int kDefaultMidiReference = 60;
}

Tuning::Tuning() : default_(true) {
  scale_start_midi_note_ = kDefaultMidiReference;
  reference_midi_note_ = 0;

  setDefaultTuning();
}

void Tuning::loadScale(std::vector<float> scale) {
  scale_ = scale;
  if (scale.size() <= 1) {
    setConstantTuning(kDefaultMidiReference);
    return;
  }

  int scale_size = static_cast<int>(scale.size() - 1);
  int mapping_size = scale_size;
  if (keyboard_mapping_.size())
    mapping_size = static_cast<int>(keyboard_mapping_.size());

  float octave_offset = scale[scale_size];
  int start_octave = -kTuningCenter / mapping_size - 1;
  int mapping_position = -kTuningCenter - start_octave * mapping_size;

  float current_offset = start_octave * octave_offset;
  for (int i = 0; i < kTuningSize; ++i) {
    if (mapping_position >= mapping_size) {
      current_offset += octave_offset;
      mapping_position = 0;
    }

    int note_in_scale = mapping_position;
    if (keyboard_mapping_.size())
      note_in_scale = keyboard_mapping_[mapping_position];

    tuning_[i] = current_offset + scale[note_in_scale];
    mapping_position++;
  }
}

void Tuning::setConstantTuning(float note) {
  for (int i = 0; i < kTuningSize; ++i)
    tuning_[i] = note;
}

void Tuning::setDefaultTuning() {
  for (int i = 0; i < kTuningSize; ++i)
    tuning_[i] = i - kTuningCenter;

  scale_.clear();
  for (int i = 0; i <= vital::kNotesPerOctave; ++i)
    scale_.push_back(i);

  keyboard_mapping_.clear();

  default_ = true;
  tuning_name_ = "";
  mapping_name_ = "";
}

vital::mono_float Tuning::convertMidiNote(int note) const {
  int scale_offset = note - scale_start_midi_note_;
  return tuning_[kTuningCenter + scale_offset] + scale_start_midi_note_ + reference_midi_note_;
}

void Tuning::setReferenceFrequency(float frequency) {
  setReferenceNoteFrequency(0, frequency);
}

void Tuning::setReferenceNoteFrequency(int midi_note, float frequency) {
  reference_midi_note_ = vital::utils::frequencyToMidiNote(frequency) - midi_note;
}

void Tuning::setReferenceRatio(float ratio) {
  reference_midi_note_ = vital::utils::ratioToMidiTranspose(ratio);
}

json Tuning::stateToJson() const {
  json data;
  data["scale_start_midi_note"] = scale_start_midi_note_;
  data["reference_midi_note"] = reference_midi_note_;
  data["tuning_name"] = tuning_name_;
  data["mapping_name"] = mapping_name_;
  data["default"] = default_;

  json scale_data;
  for (float scale_value : scale_)
    scale_data.push_back(scale_value);
  data["scale"] = scale_data;

  if (keyboard_mapping_.size()) {
    json mapping_data;
    for (int mapping_value : keyboard_mapping_)
      mapping_data.push_back(mapping_value);
    data["mapping"] = mapping_data;
  }

  return data;
}

void Tuning::jsonToState(const json& data) {
  scale_start_midi_note_ = data["scale_start_midi_note"];
  reference_midi_note_ = data["reference_midi_note"];
  std::string tuning_name = data["tuning_name"];
  tuning_name_ = tuning_name;
  std::string mapping_name = data["mapping_name"];
  mapping_name_ = mapping_name;
  
  if (data.count("default"))
    default_ = data["default"];

  json scale_data = data["scale"];
  scale_.clear();
  for (json& value : scale_data) {
    float scale_value = value;
    scale_.push_back(scale_value);
  }

  keyboard_mapping_.clear();
  if (data.count("mapping")) {
    json mapping_data = data["mapping"];
    for (json& value : mapping_data) {
      int keyboard_value = value;
      keyboard_mapping_.push_back(keyboard_value);
    }
  }

  loadScale(scale_);
}
