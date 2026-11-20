#include "MultiPoleFilter.h"
#include <algorithm>
#include <cmath>
#include <numbers>

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

    decimRatio_ = 32;
    if (sampleRate_ <= 16000.0) decimRatio_ = 8;
    if (sampleRate_ > 16000.0 && sampleRate_ <= 36000.0) decimRatio_ = 16;
    if (sampleRate_ > 72000.0 && sampleRate_ <= 144000.0) decimRatio_ = 64;
    if (sampleRate_ > 144000.0) decimRatio_ = 128;

    decHalf_ = 2 * decimRatio_;
    firDelay_ = (kSubHalf + 6) * decimRatio_;
    std::size_t ringSize = 256;
    const auto required = static_cast<std::size_t>(firDelay_ + 2 * decHalf_ + 16);
    while (ringSize < required) ringSize <<= 1;
    inMask_ = ringSize - 1;
    std::ranges::for_each(linearChannels_, [ringSize](LinearChannel& ch) { ch.inRing.resize(ringSize, 0.0); });

    // Precompute Blackman-sinc anti-aliasing decimator taps.
    const int decLen = 2 * decHalf_ + 1;
    decTaps_.resize(static_cast<std::size_t>(decLen));
    double decSum = 0.0;
    int k = 0;
    while (k < decLen) {
        const double t = static_cast<double>(k - decHalf_) * (.70 / static_cast<double>(decimRatio_));
        double sinc = 1.0;
        if (std::abs(t) >= 1e-12)
            sinc = std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double u = static_cast<double>(k) / static_cast<double>(2 * decHalf_);
        const double win = .42 - .50 * std::cos(2.0 * std::numbers::pi * u) + .08 * std::cos(4.0 * std::numbers::pi * u);
        decTaps_[static_cast<std::size_t>(k)] = sinc * win;
        decSum += decTaps_[static_cast<std::size_t>(k)];
        ++k;
    }
    std::ranges::for_each(decTaps_, [decSum](double& v) { v /= decSum; });

    // Precompute 8-tap polyphase Blackman-sinc interpolator normalized per phase.
    interpTaps_.resize(static_cast<std::size_t>(decimRatio_ * 8));
    int p = 0;
    while (p < decimRatio_) {
        double rowSum = 0.0;
        int j = 0;
        while (j < 8) {
            const double offset = static_cast<double>(j - 4) + static_cast<double>(p) / static_cast<double>(decimRatio_);
            const double dt = offset * .75;
            double sinc = 1.0;
            if (std::abs(dt) >= 1e-12)
                sinc = std::sin(std::numbers::pi * dt) / (std::numbers::pi * dt);
            const double tau = offset * .25;
            const double win = .42 + .50 * std::cos(std::numbers::pi * tau) + .08 * std::cos(2.0 * std::numbers::pi * tau);
            const double tap = sinc * win;
            interpTaps_[static_cast<std::size_t>(p * 8 + j)] = tap;
            rowSum += tap;
            ++j;
        }
        j = 0;
        while (j < 8) {
            interpTaps_[static_cast<std::size_t>(p * 8 + j)] /= rowSum;
            ++j;
        }
        ++p;
    }

    // Precompute inverse cosine transform basis for real-time sub-cut updates without trig calls.
    constexpr int dftSize = 2 * kDftHalf;
    int m = 0;
    while (m <= kSubHalf) {
        const double hann = .5 + .5 * std::cos(std::numbers::pi * static_cast<double>(m) / static_cast<double>(kSubHalf + 1));
        int bin = 0;
        while (bin <= kDftHalf) {
            const bool endpoint = bin == 0 || bin == kDftHalf;
            const double weight = endpoint ? 1.0 : 2.0;
            const double c = std::cos(2.0 * std::numbers::pi * static_cast<double>(bin * m) / static_cast<double>(dftSize));
            cosBasis_[static_cast<std::size_t>(m)][static_cast<std::size_t>(bin)] = (hann * weight * c) / static_cast<double>(dftSize);
            ++bin;
        }
        ++m;
    }

    reset();
    updateCoefficients();
}

void MultiPoleFilter::reset() noexcept {
    std::ranges::for_each(filters_, [](auto& chFilters) {
        std::ranges::for_each(chFilters, [](BiquadFilter& section) { section.reset(); });
    });
    std::ranges::for_each(linearChannels_, [](LinearChannel& ch) {
        std::fill(ch.inRing.begin(), ch.inRing.end(), 0.0);
        ch.decRing.fill(0.0);
        ch.upRing.fill(0.0);
        ch.inPos = ch.decPos = ch.upPos = 0;
        ch.phase = 0;
    });
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

void MultiPoleFilter::setLinearPhaseSubCut(bool enabled) noexcept {
    linearPhase_ = enabled;
    updateCoefficientsForFrequency(currentCutoffHz_);
}

void MultiPoleFilter::updateCoefficients() noexcept {
    currentCutoffHz_ = targetCutoffHz_;
    updateCoefficientsForFrequency(currentCutoffHz_);
}

void MultiPoleFilter::updateCoefficientsForFrequency(double cutoffHz) noexcept {
    const int order = slopeToOrder(slope_);
    if (linearPhase_ && type_ == FilterType::HighPass) {
        const double decRate = sampleRate_ / static_cast<double>(std::max(decimRatio_, 1));
        const double safeFc = std::clamp(cutoffHz, 5.0, .42 * decRate);
        constexpr int dftSize = 2 * kDftHalf;

        std::array<double, kDftHalf + 1> lpSpec{};
        lpSpec[0] = 1.0;
        int bin = 1;
        while (bin <= kDftHalf) {
            const double freq = static_cast<double>(bin) * decRate / static_cast<double>(dftSize);
            const double ratio = safeFc / freq;
            double ratioPow = 1.0;
            int p = 0;
            while (p < 2 * order) {
                ratioPow *= ratio;
                ++p;
            }
            const double hpMag = 1.0 / std::sqrt(1.0 + ratioPow);
            lpSpec[static_cast<std::size_t>(bin)] = 1.0 - hpMag;
            ++bin;
        }

        double tapSum = 0.0;
        int m = 0;
        while (m <= kSubHalf) {
            const auto& basisRow = cosBasis_[static_cast<std::size_t>(m)];
            double acc = 0.0;
            int k = 0;
            while (k <= kDftHalf) {
                acc += lpSpec[static_cast<std::size_t>(k)] * basisRow[static_cast<std::size_t>(k)];
                ++k;
            }
            subTaps_[static_cast<std::size_t>(kSubHalf - m)] = acc;
            subTaps_[static_cast<std::size_t>(kSubHalf + m)] = acc;
            const double pairAcc = 2.0 * acc;
            tapSum += m == 0 ? acc : pairAcc;
            ++m;
        }
        if (std::abs(tapSum) > 1e-18) {
            std::ranges::for_each(subTaps_, [tapSum](double& v) { v /= tapSum; });
        }
    }
    FilterDesign design{};
    if (type_ == FilterType::LowPass) design = designButterworthLowPass(sampleRate_, cutoffHz, order);
    if (type_ != FilterType::LowPass) design = designButterworthHighPass(sampleRate_, cutoffHz, order);

    const int newActiveSections = design.count;
    if (newActiveSections != activeSections_) {
        int ch = 0;
        while (ch < 2) {
            int s = std::min(activeSections_, newActiveSections);
            while (s < kMaxSections) {
                filters_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(s)].reset();
                ++s;
            }
            ++ch;
        }
    }

    activeSections_ = newActiveSections;
    int s = 0;
    while (s < activeSections_) {
        const auto su = static_cast<std::size_t>(s);
        filters_[0][su].setCoefficients(design.sections[su]);
        filters_[1][su].setCoefficients(design.sections[su]);
        ++s;
    }
}

float MultiPoleFilter::processLinearPhaseSample(int channel, float sample, float wetMix) noexcept {
    auto& ch = linearChannels_[static_cast<std::size_t>(channel)];
    if (ch.inRing.empty()) return sample;

    ch.inRing[ch.inPos] = static_cast<double>(sample);
    const double delayed = ch.inRing[(ch.inPos - static_cast<std::size_t>(firDelay_)) & inMask_];

    if (ch.phase == 0) {
        double decVal = 0.0;
        const int decLen = 2 * decHalf_ + 1;
        int k = 0;
        while (k < decLen) {
            decVal += decTaps_[static_cast<std::size_t>(k)] * ch.inRing[(ch.inPos - static_cast<std::size_t>(k)) & inMask_];
            ++k;
        }
        ch.decRing[ch.decPos] = decVal;

        double subVal = 0.0;
        int l = 0;
        while (l < kSubTaps) {
            subVal += subTaps_[static_cast<std::size_t>(l)] * ch.decRing[(ch.decPos - static_cast<std::size_t>(l)) & 63];
            ++l;
        }
        ch.upRing[ch.upPos] = subVal;
    }

    double lp = 0.0;
    const auto interpBase = static_cast<std::size_t>(ch.phase * 8);
    int j = 0;
    while (j < 8) {
        lp += interpTaps_[interpBase + static_cast<std::size_t>(j)] * ch.upRing[(ch.upPos - static_cast<std::size_t>(j)) & 15];
        ++j;
    }

    ch.inPos = (ch.inPos + 1) & inMask_;
    ++ch.phase;
    if (ch.phase == decimRatio_) {
        ch.phase = 0;
        ch.decPos = (ch.decPos + 1) & 63;
        ch.upPos = (ch.upPos + 1) & 15;
    }
    return static_cast<float>(delayed - static_cast<double>(wetMix) * lp);
}

float MultiPoleFilter::processSample(int channel, float sample) noexcept {
    if (std::abs(currentCutoffHz_ - targetCutoffHz_) > 0.001) {
        updateCoefficients();
    }
    if (channel < 0 || channel >= 2) {
        return sample;
    }
    if (linearPhase_ && type_ == FilterType::HighPass) {
        const float wet = bypassed_ ? 0.0f : 1.0f;
        return processLinearPhaseSample(channel, sample, wet);
    }
    if (bypassed_) {
        return sample;
    }

    float current = sample;
    int s = 0;
    while (s < activeSections_) {
        current = filters_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(s)].process(current);
        ++s;
    }
    return current;
}

void MultiPoleFilter::processBlock(float* const* channelData, int numChannels, int numSamples) noexcept {
    if (channelData == nullptr || numSamples <= 0) {
        return;
    }

    const bool useLinearPhase = linearPhase_ && type_ == FilterType::HighPass;
    const float targetBypassGain = bypassed_ ? 0.0f : 1.0f;
    if (!useLinearPhase && bypassed_ && currentBypassGain_ <= 1e-5f) {
        if (std::abs(currentCutoffHz_ - targetCutoffHz_) > 1e-3)
            updateCoefficients();
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

        int i = 0;
        while (i < chunk) {
            currentBypassGain_ = std::clamp(currentBypassGain_ + bypassStep, 0.0f, 1.0f);
            const float wetMix = currentBypassGain_;
            const int sampleIdx = offset + i;

            int ch = 0;
            while (ch < channelsToProcess) {
                float* data = channelData[ch];
                if (data != nullptr) {
                    const float dry = data[sampleIdx];
                    if (useLinearPhase) {
                        data[sampleIdx] = processLinearPhaseSample(ch, dry, wetMix);
                    }
                    if (!useLinearPhase) {
                        float s = dry;
                        int sec = 0;
                        while (sec < activeSections_) {
                            s = filters_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(sec)].process(s);
                            ++sec;
                        }
                        data[sampleIdx] = dry + wetMix * (s - dry);
                    }
                }
                ++ch;
            }
            ++i;
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
