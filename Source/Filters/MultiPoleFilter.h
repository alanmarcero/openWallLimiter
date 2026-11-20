#pragma once

#include "ButterworthCoeffs.h"
#include <array>
#include <vector>

namespace analog_master::core {

enum class FilterType {
    LowPass,
    HighPass
};

class MultiPoleFilter {
public:
    static constexpr int kMaxSections = 8; // Supports up to 16 poles (96 dB/oct)

    explicit MultiPoleFilter(FilterType type = FilterType::LowPass);

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void setType(FilterType type) noexcept;
    void setCutoff(double cutoffHz) noexcept;
    void setSlope(FilterSlope slope) noexcept;
    void setBypassed(bool bypassed) noexcept;
    void setLinearPhaseSubCut(bool enabled) noexcept;

    [[nodiscard]] FilterType type() const noexcept { return type_; }
    [[nodiscard]] double cutoff() const noexcept { return targetCutoffHz_; }
    [[nodiscard]] FilterSlope slope() const noexcept { return slope_; }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_; }
    [[nodiscard]] int latencySamples() const noexcept {
        const bool activeLinear = linearPhase_ && type_ == FilterType::HighPass;
        return activeLinear ? firDelay_ : 0;
    }

    [[nodiscard]] float processSample(int channel, float sample) noexcept;
    void processBlock(float* const* channelData, int numChannels, int numSamples) noexcept;

private:
    static constexpr int kSubHalf = 18;
    static constexpr int kSubTaps = 2 * kSubHalf + 1;
    static constexpr int kDftHalf = 128;

    struct LinearChannel {
        std::vector<double> inRing;
        std::array<double, 64> decRing{};
        std::array<double, 16> upRing{};
        std::size_t inPos = 0, decPos = 0, upPos = 0;
        int phase = 0;
    };

    float processLinearPhaseSample(int channel, float sample, float wetMix) noexcept;
    void updateCoefficients() noexcept;
    void updateCoefficientsForFrequency(double cutoffHz) noexcept;

    FilterType type_ = FilterType::LowPass;
    FilterSlope slope_ = FilterSlope::Slope24dB;
    double targetCutoffHz_ = 1000.0;
    double currentCutoffHz_ = 1000.0;
    double sampleRate_ = 44100.0;
    bool bypassed_ = false;
    bool linearPhase_ = false;
    bool hasProcessedAudio_ = false;
    float currentBypassGain_ = 1.0f; // 1.0 = active, 0.0 = bypassed

    int activeSections_ = 2;
    int decimRatio_ = 32, decHalf_ = 64, firDelay_ = 0;
    std::size_t inMask_ = 0;
    std::vector<double> decTaps_;
    std::vector<double> interpTaps_;
    std::array<double, kSubTaps> subTaps_{};
    std::vector<std::array<double, kDftHalf + 1>> cosBasis_{static_cast<std::size_t>(kSubHalf + 1)};
    std::array<LinearChannel, 2> linearChannels_{};
    std::array<std::array<BiquadFilter, kMaxSections>, 2> filters_{};
};

} // namespace analog_master::core
