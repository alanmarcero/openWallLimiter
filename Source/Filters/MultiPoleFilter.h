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

    [[nodiscard]] FilterType type() const noexcept { return type_; }
    [[nodiscard]] double cutoff() const noexcept { return targetCutoffHz_; }
    [[nodiscard]] FilterSlope slope() const noexcept { return slope_; }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_; }

    [[nodiscard]] float processSample(int channel, float sample) noexcept;
    void processBlock(float* const* channelData, int numChannels, int numSamples) noexcept;

private:
    void updateCoefficients() noexcept;
    void updateCoefficientsForFrequency(double cutoffHz) noexcept;

    FilterType type_ = FilterType::LowPass;
    FilterSlope slope_ = FilterSlope::Slope24dB;
    double targetCutoffHz_ = 1000.0;
    double currentCutoffHz_ = 1000.0;
    double sampleRate_ = 44100.0;
    bool bypassed_ = false;
    bool hasProcessedAudio_ = false;
    float currentBypassGain_ = 1.0f; // 1.0 = active, 0.0 = bypassed

    int activeSections_ = 2;
    std::array<std::array<BiquadFilter, kMaxSections>, 2> filters_{};
};

} // namespace analog_master::core
