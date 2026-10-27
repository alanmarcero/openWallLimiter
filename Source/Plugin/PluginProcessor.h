#pragma once
#include "Limiter.h"
#include "LoudnessMeter.h"
#include "MultiPoleFilter.h"
#include "Parameters.h"
#include "TruePeakGuard.h"
#include <array>
#include <atomic>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

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
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &) override;
    void setStateInformation(const void *, int) override;
    void resetMeters() noexcept { meterReset.store(true); }
    void applyPreset(int index);
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float> inputDb{-100}, outputDb{-100}, reductionDb{0}, shortLufs{-100}, integratedLufs{-100},
        truePeakDb{-100};
    std::atomic<int> effectiveFactor{4};

  private:
    using Oversampler = juce::dsp::Oversampling<float>;
    using Delay = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>;
    struct Path {
        std::unique_ptr<Oversampler> oversampler;
        wall::Limiter limiter;
        Delay pad;
        juce::AudioBuffer<float> buffer;
        float nativeLatency = 0;
    };
    std::array<Path, 5> paths;
    std::array<std::atomic<float> *, 19> values{};
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
    std::atomic<bool> meterReset{false};
    int channels = 2, chunkSize = 512, latency = 0, currentPath = 2, nextPath = -1, warmup = 0, fade = 0,
        fadeLength = 960;
    double preparedRate = 48000;
    bool ready = false;
    uint32_t randomState = 0x91a5c3u;
    float randomUnit() noexcept;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WallProcessor)
};
