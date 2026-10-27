#include "LoudnessMeter.h"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace analog_master::core {
float LoudnessMeter::lufs(double energy) noexcept {
    return energy > 1e-30 ? static_cast<float>(-.691 + 10 * std::log10(energy))
                          : -std::numeric_limits<float>::infinity();
}
void LoudnessMeter::prepare(double sr, int channels, bool integrated) {
    sr = std::isfinite(sr) ? std::clamp(sr, 8000., 384000.) : 48000.;
    channels_ = std::clamp(channels, 1, 2);
    measureIntegrated_ = integrated;
    histogram_.assign(integrated ? binCount : 0, {});
    prefix_.assign(integrated ? binCount + 1 : 0, {});
    epoch_ = 0;
    bucketSamples_ = std::max(1, static_cast<int>(std::lround(sr / 100.)));
    // De Man K-weighting fit, as published in libebur128 (see docs/MASTERING_DSP.md).
    double k = std::tan(std::numbers::pi * 1681.974450955533 / sr);
    const double vh = std::pow(10., 3.999843853973347 / 20.);
    const double vb = std::pow(vh, .4996667741545416);
    constexpr double q = .7071752369554196;
    double d = 1 + k / q + k * k;
    const BiquadCoeffs shelf{(vh + vb * k / q + k * k) / d, 2 * (k * k - vh) / d,
                             (vh - vb * k / q + k * k) / d, 2 * (k * k - 1) / d, (1 - k / q + k * k) / d};
    k = std::tan(std::numbers::pi * 38.13547087602444 / sr);
    constexpr double hq = .5003270373238773;
    d = 1 + k / hq + k * k;
    const BiquadCoeffs hp{1, -2, 1, 2 * (k * k - 1) / d, (1 - k / hq + k * k) / d};
    for (auto &channel : filters_) {
        channel[0].setCoefficients(shelf);
        channel[1].setCoefficients(hp);
    }
    reset();
}
void LoudnessMeter::reset() noexcept {
    for (auto &channel : filters_)
        for (auto &f : channel)
            f.reset();
    if (++epoch_ == 0) {
        std::fill(histogram_.begin(), histogram_.end(), Bin{});
        std::fill(prefix_.begin(), prefix_.end(), Bin{});
        epoch_ = 1;
    }
    bucketEnergy_ = shortSum_ = momentarySum_ = gatedSum_ = 0;
    gatedCount_ = ticks_ = 0;
    frame_ = cursor_ = 0;
    shortTerm_ = integrated_ = -std::numeric_limits<float>::infinity();
}
void LoudnessMeter::process(float l, float r) noexcept {
    const float input[2]{l, r};
    for (int c = 0; c < channels_; ++c) {
        const auto x = filters_[c][1].process(filters_[c][0].process(input[c]));
        bucketEnergy_ += static_cast<double>(x) * x;
    }
    if (++frame_ == bucketSamples_)
        finishBucket();
}
void LoudnessMeter::finishBucket() noexcept {
    const double e = bucketEnergy_ / bucketSamples_;
    // Ignore pre-reset ring contents until the corresponding window fills again.
    if (ticks_ >= 300)
        shortSum_ -= energies_[cursor_];
    if (ticks_ >= 40)
        momentarySum_ -= energies_[(cursor_ + 260) % 300];
    energies_[cursor_] = e;
    cursor_ = (cursor_ + 1) % 300;
    shortSum_ = std::max(0., shortSum_ + e);
    momentarySum_ = std::max(0., momentarySum_ + e);
    ++ticks_;
    frame_ = 0;
    bucketEnergy_ = 0;
    if (shortTermReady())
        shortTerm_ = lufs(shortSum_ / 300.);
    if (!measureIntegrated_ || ticks_ < 40 || ticks_ % 10 != 0)
        return;
    const double block = momentarySum_ / 40.;
    constexpr double absoluteGate = 1.172465304582298e-7; // 10^((-70 + .691)/10)
    if (block > absoluteGate) {
        const int index = std::clamp(static_cast<int>((lufs(block) + 70.) * binsPerLu), 0, binCount - 1);
        addEnergy(index, block);
        gatedSum_ += block;
        ++gatedCount_;
    }
    if (gatedCount_ == 0)
        return;
    const double gate = std::max(absoluteGate, .1 * gatedSum_ / static_cast<double>(gatedCount_));
    const int boundary = std::clamp(static_cast<int>((lufs(gate) + 70.) * binsPerLu), 0, binCount - 1);
    const auto below = prefixThrough(boundary);
    double sum = std::max(0., gatedSum_ - below.energy);
    std::uint64_t count = gatedCount_ - below.count;
    const auto &crossing = histogram_[static_cast<size_t>(boundary)];
    // Exact energy sums; only the bin intersecting the gate uses mean classification.
    if (crossing.epoch == epoch_ && crossing.count &&
        crossing.energy / static_cast<double>(crossing.count) > gate) {
        sum += crossing.energy;
        count += crossing.count;
    }
    integrated_ = count ? lufs(sum / static_cast<double>(count)) : -std::numeric_limits<float>::infinity();
}
void LoudnessMeter::addEnergy(int index, double energy) noexcept {
    auto &bin = histogram_[static_cast<size_t>(index)];
    if (bin.epoch != epoch_)
        bin = {0, 0, epoch_};
    bin.energy += energy;
    ++bin.count;
    for (int i = index + 1; i <= binCount; i += i & -i) {
        auto &node = prefix_[static_cast<size_t>(i)];
        if (node.epoch != epoch_)
            node = {0, 0, epoch_};
        node.energy += energy;
        ++node.count;
    }
}
LoudnessMeter::Bin LoudnessMeter::prefixThrough(int index) const noexcept {
    Bin sum;
    for (int i = index + 1; i > 0; i -= i & -i) {
        const auto &node = prefix_[static_cast<size_t>(i)];
        if (node.epoch == epoch_) {
            sum.energy += node.energy;
            sum.count += node.count;
        }
    }
    return sum;
}
} // namespace analog_master::core
