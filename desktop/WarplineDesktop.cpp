// WarplineDesktop.cpp - Warpline as a desktop plugin (Audio Unit and VST3 on macOS, VST3 on Linux and Windows).
//
// A thin JUCE plugin that hosts the unchanged MPC build of Warpline: the engine is the same code as on MPC, built as
// its own library (warpline.dylib / .so / .dll) inside the bundle's Contents/Resources/VST, next to Content/ and
// plugin.json. This plugin loads it and talks to it through the VST2 calls MPC itself uses (dispatcher,
// processReplacing, parameters, chunks), so nothing in the engine changes and the two never share symbols (Warpline's
// engine carries its own small stand-in for the bits of JUCE that Vital uses).
//
// There is no MPC screen here: DAWs show their own list of Warpline's parameters. Beta, for testing.
// Part of Warpline. GPLv3 (see LICENSE). JUCE 7 is used under the GPLv3.

#include <JuceHeader.h>

#include "sdk/mpc_vst2_abi.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if JUCE_WINDOWS
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace {

using MainProc = AEffect* (*)(audioMasterCallback);

// Where this plugin's own binary is, so the engine is found inside the same bundle.
juce::File thisBinary() {
#if JUCE_WINDOWS
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&thisBinary), &module);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return juce::File(juce::String(path));
#else
  Dl_info info {};
  if (dladdr(reinterpret_cast<void*>(&thisBinary), &info) != 0 && info.dli_fname != nullptr)
    return juce::File(juce::String::fromUTF8(info.dli_fname));
  return {};
#endif
}

// <bundle>/Contents/Resources/VST/warpline.<ext>: the binary sits in Contents/MacOS or Contents/<arch>-<os>.
juce::File engineFile() {
  juce::File contents = thisBinary().getParentDirectory().getParentDirectory();
#if JUCE_MAC
  const char* name = "warpline.dylib";
#elif JUCE_WINDOWS
  const char* name = "warpline.dll";
#else
  const char* name = "warpline.so";
#endif
  return contents.getChildFile("Resources").getChildFile("VST").getChildFile(name);
}

} // namespace

class WarplineProcessor;

// One of the engine's parameters, read and written straight through the VST2 calls.
class EngineParameter : public juce::AudioProcessorParameter {
public:
  EngineParameter(WarplineProcessor& owner, int index, juce::String name) : owner_(owner), index_(index), name_(std::move(name)) {}
  float getValue() const override;
  void setValue(float v) override;
  float getDefaultValue() const override { return default_; }
  juce::String getName(int maxLength) const override { return name_.substring(0, maxLength); }
  juce::String getLabel() const override { return {}; }
  juce::String getText(float value, int maxLength) const override;
  float getValueForText(const juce::String& text) const override { return juce::jlimit(0.0f, 1.0f, text.getFloatValue()); }
  void setDefault(float v) { default_ = v; }
private:
  WarplineProcessor& owner_;
  int index_;
  juce::String name_;
  float default_ = 0.0f;
};

class WarplineProcessor : public juce::AudioProcessor {
public:
  WarplineProcessor()
      : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    current_ = this;
    loadEngine();
    current_ = nullptr;
  }

  ~WarplineProcessor() override {
    if (effect_) {
      effect_->dispatcher(effect_, effStopProcess, 0, 0, nullptr, 0.0f);
      effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
      effect_->dispatcher(effect_, effClose, 0, 0, nullptr, 0.0f);
      effect_ = nullptr;
    }
#if JUCE_WINDOWS
    if (library_) FreeLibrary(static_cast<HMODULE>(library_));
#else
    if (library_) dlclose(library_);
#endif
  }

  // ----- the engine ---------------------------------------------------------------------------------------------
  AEffect* effect() const { return effect_; }

  juce::String paramText(int index, float value) const {
    if (!effect_) return {};
    // The engine formats only its current value: text for that, a plain number for other values.
    std::lock_guard<std::mutex> lock(text_mutex_);
    float before = effect_->getParameter(effect_, index);
    bool other = std::fabs(before - value) > 1.0e-6f;
    char text[256] = {};
    if (!other) {
      effect_->dispatcher(effect_, effGetParamDisplay, index, 0, text, 0.0f);
      return juce::String::fromUTF8(text);
    }
    return juce::String(value, 3);   // another value than the current one: the engine only formats its own
  }

  // ----- juce::AudioProcessor -----------------------------------------------------------------------------------
  const juce::String getName() const override { return "Warpline"; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 2.0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return "Warpline"; }
  void changeProgramName(int, const juce::String&) override {}
  bool hasEditor() const override { return false; }
  juce::AudioProcessorEditor* createEditor() override { return nullptr; }

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
  }

  void prepareToPlay(double sampleRate, int blockSize) override {
    sample_rate_ = sampleRate;
    if (!effect_) return;
    effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
    effect_->dispatcher(effect_, effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate));
    effect_->dispatcher(effect_, effSetBlockSize, 0, blockSize, nullptr, 0.0f);
    effect_->dispatcher(effect_, effMainsChanged, 0, 1, nullptr, 0.0f);
    effect_->dispatcher(effect_, effStartProcess, 0, 0, nullptr, 0.0f);
    events_.resize(kMaxEvents);
    event_list_.resize(sizeof(VstEvents) + kMaxEvents * sizeof(VstEvent*));
  }

  void releaseResources() override {
    if (!effect_) return;
    effect_->dispatcher(effect_, effStopProcess, 0, 0, nullptr, 0.0f);
    effect_->dispatcher(effect_, effMainsChanged, 0, 0, nullptr, 0.0f);
  }

  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
    juce::ScopedNoDenormals no_denormals;
    const int frames = buffer.getNumSamples();
    if (!effect_ || buffer.getNumChannels() < 2) {
      buffer.clear();
      return;
    }
    updateTime(frames);

    // MIDI for this block (MIDI 1.0 channel messages only).
    int n = 0;
    for (const auto meta : midi) {
      const auto msg = meta.getMessage();
      if (n >= kMaxEvents || msg.getRawDataSize() > 3 || msg.isSysEx()) continue;
      VstMidiEvent& e = events_[static_cast<size_t>(n)];
      e = VstMidiEvent {};
      e.type = kVstMidiType;
      e.byteSize = sizeof(VstMidiEvent);
      e.deltaFrames = juce::jlimit(0, frames - 1, meta.samplePosition);
      const juce::uint8* raw = msg.getRawData();
      for (int i = 0; i < msg.getRawDataSize(); ++i) e.midiData[i] = static_cast<char>(raw[i]);
      ++n;
    }
    if (n > 0) {
      auto* list = reinterpret_cast<VstEvents*>(event_list_.data());
      list->numEvents = n;
      list->reserved = 0;
      for (int i = 0; i < n; ++i) list->events[i] = reinterpret_cast<VstEvent*>(&events_[static_cast<size_t>(i)]);
      effect_->dispatcher(effect_, effProcessEvents, 0, 0, list, 0.0f);
    }

    float* outs[2] = { buffer.getWritePointer(0), buffer.getWritePointer(1) };
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, frames);
    effect_->processReplacing(effect_, nullptr, outs, frames);
  }

  void getStateInformation(juce::MemoryBlock& dest) override {
    if (!effect_) return;
    void* data = nullptr;
    auto size = effect_->dispatcher(effect_, effGetChunk, 0, 0, &data, 0.0f);
    if (data != nullptr && size > 0)
      dest.append(data, static_cast<size_t>(size));
  }

  void setStateInformation(const void* data, int size) override {
    if (!effect_ || data == nullptr || size <= 0) return;
    std::vector<char> copy(static_cast<const char*>(data), static_cast<const char*>(data) + size);
    effect_->dispatcher(effect_, effSetChunk, 0, size, copy.data(), 0.0f);
  }

private:
  static constexpr int kMaxEvents = 512;

  static VstIntPtr hostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr,
                                            float opt) {
    WarplineProcessor* self = effect != nullptr ? static_cast<WarplineProcessor*>(effect->user) : current_;
    switch (opcode) {
      case audioMasterVersion: return 2400;
      case audioMasterGetTime: return self ? reinterpret_cast<VstIntPtr>(&self->time_) : 0;
      case audioMasterGetSampleRate: return self ? static_cast<VstIntPtr>(self->sample_rate_) : 44100;
      case audioMasterAutomate:
        if (self && index >= 0 && index < static_cast<int>(self->params_.size()))
          self->params_[static_cast<size_t>(index)]->sendValueChangedMessageToListeners(opt);
        return 0;
      case audioMasterUpdateDisplay:
        if (self) self->updateHostDisplay(ChangeDetails().withParameterInfoChanged(true));
        return 1;
      case audioMasterGetProductString:
        if (ptr) std::strcpy(static_cast<char*>(ptr), "Warpline Desktop");
        return 1;
      case audioMasterGetVendorString:
        if (ptr) std::strcpy(static_cast<char*>(ptr), "CLVRKE");
        return 1;
      default: break;
    }
    juce::ignoreUnused(value);
    return 0;
  }

  void loadEngine() {
    juce::File file = engineFile();
#if JUCE_WINDOWS
    library_ = LoadLibraryW(file.getFullPathName().toWideCharPointer());
    auto main = library_ ? reinterpret_cast<MainProc>(GetProcAddress(static_cast<HMODULE>(library_), "VSTPluginMain")) : nullptr;
#else
    library_ = dlopen(file.getFullPathName().toRawUTF8(), RTLD_NOW | RTLD_LOCAL);
    auto main = library_ ? reinterpret_cast<MainProc>(dlsym(library_, "VSTPluginMain")) : nullptr;
#endif
    if (!main) {
      DBG("Warpline: cannot load the engine at " + file.getFullPathName());
      return;
    }
    effect_ = main(&WarplineProcessor::hostCallback);
    if (!effect_) return;
    effect_->user = this;
    effect_->dispatcher(effect_, effOpen, 0, 0, nullptr, 0.0f);
    for (int i = 0; i < effect_->numParams; ++i) {
      char name[256] = {};
      effect_->dispatcher(effect_, effGetParamName, i, 0, name, 0.0f);
      auto* p = new EngineParameter(*this, i, juce::String::fromUTF8(name));
      p->setDefault(effect_->getParameter(effect_, i));
      params_.push_back(p);
      addParameter(p);
    }
  }

  void updateTime(int frames) {
    time_ = VstTimeInfo {};
    time_.sampleRate = sample_rate_;
    time_.tempo = 120.0;
    time_.timeSigNumerator = 4;
    time_.timeSigDenominator = 4;
    time_.flags = kVstTempoValid | kVstTimeSigValid;
    if (auto* head = getPlayHead()) {
      if (auto pos = head->getPosition()) {
        if (auto bpm = pos->getBpm()) time_.tempo = *bpm;
        if (auto ppq = pos->getPpqPosition()) { time_.ppqPos = *ppq; time_.flags |= kVstPpqPosValid; }
        if (auto bar = pos->getPpqPositionOfLastBarStart()) { time_.barStartPos = *bar; time_.flags |= kVstBarsValid; }
        if (auto samples = pos->getTimeInSamples()) time_.samplePos = static_cast<double>(*samples);
        if (auto sig = pos->getTimeSignature()) { time_.timeSigNumerator = sig->numerator; time_.timeSigDenominator = sig->denominator; }
        if (pos->getIsPlaying()) time_.flags |= kVstTransportPlaying;
        if (auto loop = pos->getLoopPoints()) {
          time_.cycleStartPos = loop->ppqStart;
          time_.cycleEndPos = loop->ppqEnd;
          time_.flags |= kVstCyclePosValid;
          if (pos->getIsLooping()) time_.flags |= kVstTransportCycleActive;
        }
      }
    }
    juce::ignoreUnused(frames);
  }

  static inline thread_local WarplineProcessor* current_ = nullptr;   // during VSTPluginMain, before effect->user is set
  void* library_ = nullptr;
  AEffect* effect_ = nullptr;
  std::vector<EngineParameter*> params_;
  double sample_rate_ = 44100.0;
  VstTimeInfo time_ {};
  std::vector<VstMidiEvent> events_;
  std::vector<char> event_list_;
  mutable std::mutex text_mutex_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WarplineProcessor)
};

float EngineParameter::getValue() const {
  AEffect* e = owner_.effect();
  return e ? e->getParameter(e, index_) : 0.0f;
}

void EngineParameter::setValue(float v) {
  if (AEffect* e = owner_.effect())
    e->setParameter(e, index_, v);
}

juce::String EngineParameter::getText(float value, int maxLength) const {
  return owner_.paramText(index_, value).substring(0, maxLength);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WarplineProcessor(); }
