#include "MultiPoleFilter.h"
#include <algorithm>
#include <cmath>

namespace analog_master::core {

MultiPoleFilter::MultiPoleFilter(FilterType type)
    : type_(type) {
}

void MultiPoleFilter::prepare(double sampleRate) noexcept {
    if (sampleRate <= 0.0) {
        return;
    }
    sampleRate_ = sampleRate;
    currentCutoffHz_ = targetCutoffHz_;
    currentBypassGain_ = bypassed_ ? 0.0f : 1.0f;
    hasProcessedAudio_ = false;
    reset();
    updateCoefficients();
}

void MultiPoleFilter::reset() noexcept {
    for (auto& chFilters : filters_) {
        for (auto& section : chFilters) {
            section.reset();
        }
    }
    currentCutoffHz_ = targetCutoffHz_;
    currentBypassGain_ = bypassed_ ? 0.0f : 1.0f;
    hasProcessedAudio_ = false;
}

void MultiPoleFilter::setType(FilterType type) noexcept {
    if (type_ == type) {
        return;
    }
    type_ = type;
    updateCoefficients();
}

void MultiPoleFilter::setCutoff(double cutoffHz) noexcept {
    if (std::abs(targetCutoffHz_ - cutoffHz) < 0.001) {
        return;
    }
    targetCutoffHz_ = cutoffHz;
    if (!hasProcessedAudio_) {
        currentCutoffHz_ = targetCutoffHz_;
        updateCoefficients();
    }
}

void MultiPoleFilter::setSlope(FilterSlope slope) noexcept {
    if (slope_ == slope) {
        return;
    }
    slope_ = slope;
    updateCoefficientsForFrequency(currentCutoffHz_);
}

void MultiPoleFilter::setBypassed(bool bypassed) noexcept {
    bypassed_ = bypassed;
    if (!hasProcessedAudio_) {
        currentBypassGain_ = bypassed_ ? 0.0f : 1.0f;
    }
}

void MultiPoleFilter::updateCoefficients() noexcept {
    currentCutoffHz_ = targetCutoffHz_;
    updateCoefficientsForFrequency(currentCutoffHz_);
}

void MultiPoleFilter::updateCoefficientsForFrequency(double cutoffHz) noexcept {
    const int order = slopeToOrder(slope_);
    const FilterDesign design = (type_ == FilterType::LowPass)
        ? designButterworthLowPass(sampleRate_, cutoffHz, order)
        : designButterworthHighPass(sampleRate_, cutoffHz, order);

    const int newActiveSections = design.count;

    // Reset unused / newly activated sections to prevent clicks from historical stale states
    if (newActiveSections != activeSections_) {
        for (int ch = 0; ch < 2; ++ch) {
            for (int s = std::min(activeSections_, newActiveSections); s < kMaxSections; ++s) {
                filters_[ch][s].reset();
            }
        }
    }

    activeSections_ = newActiveSections;

    for (int s = 0; s < activeSections_; ++s) {
        filters_[0][s].setCoefficients(design.sections[s]);
        filters_[1][s].setCoefficients(design.sections[s]);
    }
}

float MultiPoleFilter::processSample(int channel, float sample) noexcept {
    if (std::abs(currentCutoffHz_ - targetCutoffHz_) > 0.001) {
        updateCoefficients();
    }
    if (bypassed_ || channel < 0 || channel >= 2) {
        return sample;
    }

    float current = sample;
    for (int s = 0; s < activeSections_; ++s) {
        current = filters_[channel][s].process(current);
    }
    return current;
}

void MultiPoleFilter::processBlock(float* const* channelData, int numChannels, int numSamples) noexcept {
    if (channelData == nullptr || numSamples <= 0) {
        return;
    }

    const float targetBypassGain = bypassed_ ? 0.0f : 1.0f;
    if (bypassed_ && currentBypassGain_ <= 1e-5f) {
        currentCutoffHz_ = targetCutoffHz_;
        hasProcessedAudio_ = true;
        return;
    }

    const int channelsToProcess = std::min(numChannels, 2);
    const float bypassStep = (targetBypassGain - currentBypassGain_) / static_cast<float>(numSamples);
    const bool smoothingCutoff = std::abs(currentCutoffHz_ - targetCutoffHz_) > 1e-3;
    const double safeStartHz = std::max(5.0, currentCutoffHz_);
    const double safeEndHz = std::max(5.0, targetCutoffHz_);
    const double logRatio = smoothingCutoff ? std::log(safeEndHz / safeStartHz) : 0.0;

    constexpr int kSubBlockSize = 16;
    int offset = 0;
    while (offset < numSamples) {
        const int chunk = std::min(kSubBlockSize, numSamples - offset);
        if (smoothingCutoff) {
            const double t = static_cast<double>(offset + chunk) / static_cast<double>(numSamples);
            currentCutoffHz_ = safeStartHz * std::exp(logRatio * t);
            updateCoefficientsForFrequency(currentCutoffHz_);
        }

        for (int i = 0; i < chunk; ++i) {
            currentBypassGain_ = std::clamp(currentBypassGain_ + bypassStep, 0.0f, 1.0f);
            const float wetMix = currentBypassGain_;
            const int sampleIdx = offset + i;

            for (int ch = 0; ch < channelsToProcess; ++ch) {
                float* data = channelData[ch];
                if (data == nullptr) {
                    continue;
                }
                const float dry = data[sampleIdx];
                float s = dry;
                for (int sec = 0; sec < activeSections_; ++sec) {
                    s = filters_[ch][sec].process(s);
                }
                data[sampleIdx] = dry + wetMix * (s - dry);
            }
        }
        offset += chunk;
    }

    if (smoothingCutoff) {
        currentCutoffHz_ = targetCutoffHz_;
        updateCoefficientsForFrequency(currentCutoffHz_);
    }
    currentBypassGain_ = targetBypassGain;
    hasProcessedAudio_ = true;
}

} // namespace analog_master::core
