#pragma once
#include "ArtworkBeatClock.h"
#include "Limiter.h"
#include "LoudnessMeter.h"
#include "MultiPoleFilter.h"
#include "Parameters.h"
#include "TruePeakGuard.h"
#include "suite/ABCompare.h"
#include "suite/ParameterUndo.h"
#include <array>
#include <atomic>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <utility>
#include <vector>

struct WallPreset {
    const char *name;
    std::vector<std::pair<const char *, float>> overrides;
};

class WallProcessor final : public juce::AudioProcessor {
  public:
    WallProcessor();
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout &) const override;
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    void processBlockBypassed(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    juce::AudioProcessorParameter *getBypassParameter() const override {
        return parameters.getParameter("bypass");
    }
    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return .05; }
    int getNumPrograms() override { return int(factoryPresets().size()); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &) override;
    void setStateInformation(const void *, int) override;
    void resetMeters() noexcept { meterReset.store(true); }
    void applyPreset(int index);
    static const std::vector<WallPreset> &factoryPresets();
    juce::MemoryBlock captureParameters();
    bool loadParameterState(const juce::MemoryBlock &);
    void toggleAB();
    void copyAB(int from, int to);
    int activeAB() const noexcept { return abCompare.active(); }
    void undo() { undoManager.undo(); }
    void redo() { undoManager.redo(); }
    bool canUndo() const { return undoManager.canUndo(); }
    bool canRedo() const { return undoManager.canRedo(); }
    void matchLoudnessTarget();
    int storedEditorWidth() const noexcept { return editorWidth; }
    int storedEditorHeight() const noexcept { return editorHeight; }
    void setStoredEditorSize(int width, int height);
    static constexpr int defaultEditorWidth = 1120;
    static constexpr int defaultEditorHeight = 828;
    // 90% of the default size, so 40 px buttons stay at least 36 px tall.
    static constexpr int minEditorWidth = 1008;
    static constexpr int minEditorHeight = 746;
    static constexpr int maxEditorWidth = 2560;
    static constexpr int maxEditorHeight = 1800;
    static constexpr int sincPadHalfSpan = 16;
    juce::AudioProcessorValueTreeState parameters;
    juce::UndoManager undoManager;
    suite::ParameterUndo parameterUndo;
    // Peaks since the last editor read. The editor reads them with exchange(noReading).
    // noReading tells the editor that no audio block ran since that read.
    static constexpr float noReading = -1000.f;
    std::atomic<float> inputDb{noReading}, outputDb{noReading}, reductionDb{noReading};
    std::atomic<float> shortLufs{-100}, integratedLufs{-100}, truePeakDb{-100};
    float artworkPhase() const noexcept { return artworkClock.currentPhase(); }
    double preparedSampleRate() const noexcept { return preparedRate; }
    std::atomic<int> effectiveFactor{4};

  private:
    using Oversampler = juce::dsp::Oversampling<float>;
    using Delay = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>;
    struct SincPad {
        static constexpr int tapsCount = 2 * sincPadHalfSpan;
        std::array<std::vector<float>, 2> rings{};
        std::array<std::size_t, 2> positions{};
        std::array<double, tapsCount> taps{};
        std::size_t mask = 0;
        int baseLag = 0, intDelay = 0;
        bool exactInteger = true;
        void prepare(int maxSamples, float delaySamples);
        void reset() noexcept;
        float process(int channel, float sample) noexcept;
    };
    struct Path {
        std::unique_ptr<Oversampler> oversampler;
        wall::Limiter limiter;
        SincPad pad;
        juce::AudioBuffer<float> buffer;
        float nativeLatency = 0;
    };
    std::vector<Path> paths{5};
    std::array<std::atomic<float> *, 24> values{};
    float value(size_t i) const noexcept { return values[i]->load(std::memory_order_relaxed); }
    void process(juce::AudioBuffer<float> &, bool);
    void processChunk(juce::AudioBuffer<float> &, int, int, bool);
    void runPath(int, const wall::Settings &, int);
    std::array<analog_master::core::MultiPoleFilter, 2> filters{
        analog_master::core::MultiPoleFilter{analog_master::core::FilterType::HighPass},
        analog_master::core::MultiPoleFilter{analog_master::core::FilterType::LowPass}};
    analog_master::core::LoudnessMeter meter;
    std::unique_ptr<Oversampler> peakMeter;
    juce::AudioBuffer<float> input, dry, metering;
    Delay dryDelay;
    wall::TruePeakGuard peakGuard;
    juce::SmoothedValue<float> bypassMix, matchGain, deltaMix;
    std::array<float, 2> tiltState{0.0f, 0.0f};
    std::array<std::array<float, 5>, 2> ditherError{};
    std::atomic<bool> meterReset{false};
    int channels = 2, chunkSize = 512, latency = 0, pathLatency = 0, currentPath = 2, nextPath = -1, warmup = 0,
        fade = 0, fadeLength = 960;
    ArtworkBeatClock artworkClock;
    double preparedRate = 48000;
    bool ready = false;
    uint32_t randomState = 0x91a5c3u;
    suite::ABCompare abCompare;
    int editorWidth = defaultEditorWidth, editorHeight = defaultEditorHeight, currentProgram = 0;
    float randomUnit() noexcept;
    juce::ValueTree sanitizeTree(juce::ValueTree);
    void restoreTree(juce::ValueTree, bool undoable, const juce::String &undoName);
    void restoreBlock(const juce::MemoryBlock &, bool undoable, const juce::String &undoName);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WallProcessor)
};
