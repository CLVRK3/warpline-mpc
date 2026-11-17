// VitalCore.h - owns one Vital sound engine and drives it in fixed-size chunks.
//
// Part of Vitalium MPC. GPLv3 (see LICENSE): the engine is Vital by Matt Tytel.
//
// Threading: everything except process()/note/controller calls must run off the audio thread
// (constructor, init(), the patch loader). The plugin wrapper swaps whole VitalCore instances
// rather than editing a running engine from another thread.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vital { class SoundEngine; class Value; }
class WavetableCreator;
class Tuning;

// What happened when a .vital patch was loaded (shown on the PATCHES page).
struct PatchReport {
  bool ok = false;
  std::string name;
  std::string author;
  std::string style;
  std::string error;
  std::vector<std::string> notes;   // e.g. "Unison 16 -> 8 voices"
};

class VitalCore {
  public:
    static constexpr int kNumOscillators = 3;

    VitalCore();
    ~VitalCore();

    // Off the audio thread: reset everything to Vital's "Init" patch.
    void loadInit();
    // Off the audio thread: prepare for a sample rate.
    void prepare(double sample_rate);

    // Audio thread.
    void noteOn(int note, float velocity, int sample, int channel);
    void noteOff(int note, float lift, int sample, int channel);
    void allNotesOff();
    void allSoundsOff();
    void pitchBend(float value);           // -1..1
    void modWheel(float value);            // 0..1
    void channelPressure(float value);     // 0..1
    void sustain(bool on);
    void setBpm(float bpm);
    void setTime(double seconds);
    // Renders up to any number of frames (internally chunked to the engine's block size).
    void render(float* left, float* right, int num_frames);

    // Controls by Vital's internal name ("osc_1_level", "filter_1_cutoff", ...). Engine values,
    // not normalised. Safe on the audio thread (a Value::set is a plain store).
    bool hasControl(const std::string& name) const;
    void setControl(const std::string& name, float value);
    float getControl(const std::string& name) const;

    // Raw access for the patch loader.
    vital::SoundEngine* engine() { return engine_.get(); }
    WavetableCreator* wavetableCreator(int i) { return wavetable_creators_[i].get(); }
    Tuning* tuning() { return tuning_.get(); }

    int activeVoices();
    // A control's value including its modulation right now, 0..1 of its range (the plain value when
    // no note is playing). Read from another thread while audio runs: display use only.
    float liveNormalised(const std::string& name);
    // Off the audio thread: run the engine on silence so the first real block has no page-fault spike.
    void warmUp();

    // Off the audio thread: load a .vital patch (the JSON text of the file).
    bool loadPatch(const std::string& text, PatchReport& report);
    // Off the audio thread: keep CPU in budget for the MPC. Records what changed in report.notes.
    void applyLimits(int max_unison, int max_polyphony, int oversampling, PatchReport* report);

    // Off the audio thread: the whole engine state as .vital JSON text (Vital's LoadSave::stateToJson).
    std::string savePatch(const std::string& name, const std::string& author, const std::string& style,
                          const std::string& comments, const std::vector<std::string>& macro_names);
    // Off the audio thread: load a .vitaltable wavetable into oscillator 0..2.
    bool loadWavetable(int osc, const std::string& vitaltable_text);
    // Off the audio thread: connect a modulation (source and destination use Vital's names).
    // Off the audio thread: LFO 0..7 shape. 0 triangle, 1 sine, 2 square, 3 saw up, 4 saw down.
    void setLfoShape(int lfo, int shape);
    bool addModulation(const std::string& source, const std::string& destination, float amount,
                       bool bipolar = false);

  private:
    std::unique_ptr<vital::SoundEngine> engine_;
    std::unique_ptr<Tuning> tuning_;
    std::array<std::unique_ptr<WavetableCreator>, kNumOscillators> wavetable_creators_;
    std::vector<std::pair<std::string, vital::Value*>> controls_;   // sorted by name
    std::vector<int> connected_modulations_;                         // bank indices in use
    void clearModulations();
    bool connectModulation(int bank_index, const std::string& source, const std::string& destination);
    vital::Value* findControl(const std::string& name) const;
    double time_ = 0.0;
    double sample_rate_ = 44100.0;
};
