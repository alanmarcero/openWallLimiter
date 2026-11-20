#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace wall {
// Post-downsampling brickwall true-peak guard with 16-phase parabolic sinc detection,
// aligned 64-sample 3-pass smooth ramp, and monotonic hold deque at 0.0 dB reserve.
class TruePeakGuard {
  public:
    static constexpr int delaySamples = 128;
    static constexpr float reserveDb = 0.0f;
    static constexpr double reserve = 1.0;

    void prepare(double sr) noexcept {
        release = 1.0 - std::exp(-1.0 / (sr * .05));
        reset();
        int phase = 0;
        while (phase < 16) {
            double normalization = 0.0;
            int k = 0;
            while (k < 64) {
                const int m = 32 - k;
                const double t = double(phase) * .0625 - double(m);
                double sinc = 1.0;
                if (std::abs(t) >= 1e-10)
                    sinc = std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
                const double v = sinc * (.5 + .5 * std::cos(std::numbers::pi * t / 33.0));
                kernels[size_t(phase)][size_t(k)] = v;
                normalization += v;
                ++k;
            }
            std::ranges::for_each(kernels[size_t(phase)], [normalization](double &v) { v /= normalization; });
            ++phase;
        }
    }

    void reset() noexcept {
        std::ranges::for_each(audio, [](auto &c) { c.fill(0.0f); });
        dequeValues.fill(1.0);
        dequeTimes.fill(0);
        stage0.fill(1.0);
        stage1.fill(1.0);
        stage2.fill(1.0);
        position = pos0 = pos1 = pos2 = 0;
        dequeHead = dequeSize = 0;
        recentHotSamples = 0;
        sampleTime = 0;
        sum0 = 23.0;
        sum1 = 21.0;
        sum2 = 21.0;
        corr0 = corr1 = corr2 = 0.0;
        envelope = 1.0;
    }

    void process(float &left, float &right, float ceiling, bool enabled) noexcept {
        const std::array<float, 2> delayed{audio[0][size_t(position)], audio[1][size_t(position)]};
        audio[0][size_t(position)] = left;
        audio[1][size_t(position)] = right;
        if (!enabled) {
            left = delayed[0];
            right = delayed[1];
            position = (position + 1) & (delaySamples - 1);
            ++sampleTime;
            return;
        }

        const double newestPeak = std::max(std::abs(double(left)), std::abs(double(right)));
        const bool hotNow = newestPeak >= double(ceiling) * .25;
        if (hotNow) recentHotSamples = 96;
        if (!hotNow && recentHotSamples > 0) --recentHotSamples;

        double peak = 0.0;
        if (recentHotSamples > 0) {
            size_t ch = 0;
            while (ch < 2) {
                std::array<double, 16> mags{};
                size_t bestPhase = 0;
                size_t phase = 0;
                while (phase < 16) {
                    const auto &kernel = kernels[phase];
                    double v = 0.0;
                    int k = 0;
                    while (k < 64) {
                        v += kernel[size_t(k)] *
                             double(audio[ch][size_t((position - k + delaySamples) & (delaySamples - 1))]);
                        ++k;
                    }
                    const double mag = std::abs(v);
                    mags[phase] = mag;
                    if (mag > mags[bestPhase]) bestPhase = phase;
                    ++phase;
                }
                const double y0 = mags[(bestPhase + 15) & 15];
                const double y1 = mags[bestPhase];
                const double y2 = mags[(bestPhase + 1) & 15];
                const double denom = 2.0 * (2.0 * y1 - y0 - y2);
                double refined = y1;
                if (denom > 1e-12 && y1 >= y0 && y1 >= y2)
                    refined = y1 + (y0 - y2) * (y0 - y2) / (4.0 * denom);
                peak = std::max(peak, refined);
                ++ch;
            }
        }

        // Hold minimum across 64-sample pre-ramp + full 64-tap sinc support around lag 32.
        constexpr std::uint64_t holdWindow = 132;
        double target = 1.0;
        if (peak > double(ceiling) * 1.00002)
            target = std::min(1.0, (double(ceiling) * .9997) / peak);
        while (dequeSize > 0 && sampleTime - dequeTimes[dequeHead] >= holdWindow) {
            dequeHead = (dequeHead + 1) & 255;
            --dequeSize;
        }
        while (dequeSize > 0 && dequeValues[(dequeHead + dequeSize - 1) & 255] >= target)
            --dequeSize;
        const size_t writeIdx = (dequeHead + dequeSize) & 255;
        dequeValues[writeIdx] = target;
        dequeTimes[writeIdx] = sampleTime;
        ++dequeSize;

        const double held = dequeValues[dequeHead];
        const double released = envelope + release * (held - envelope);
        envelope = std::min(held, released);

        // 3-pass unequal moving average (23, 21, 21) spans 62 <= 64 samples without slope kinks.
        const double diff0 = envelope - stage0[size_t(pos0)] - corr0;
        const double next0 = sum0 + diff0;
        corr0 = (next0 - sum0) - diff0;
        sum0 = next0;
        stage0[size_t(pos0)] = envelope;
        pos0 = (pos0 + 1) % 23;
        const double v0 = sum0 / 23.0;

        const double diff1 = v0 - stage1[size_t(pos1)] - corr1;
        const double next1 = sum1 + diff1;
        corr1 = (next1 - sum1) - diff1;
        sum1 = next1;
        stage1[size_t(pos1)] = v0;
        pos1 = (pos1 + 1) % 21;
        const double v1 = sum1 / 21.0;

        const double diff2 = v1 - stage2[size_t(pos2)] - corr2;
        const double next2 = sum2 + diff2;
        corr2 = (next2 - sum2) - diff2;
        sum2 = next2;
        stage2[size_t(pos2)] = v1;
        pos2 = (pos2 + 1) % 21;
        const float gain = float(std::clamp(sum2 / 21.0, 0.0, 1.0));

        left = delayed[0] * gain;
        right = delayed[1] * gain;
        position = (position + 1) & (delaySamples - 1);
        ++sampleTime;
    }

  private:
    std::array<std::array<float, delaySamples>, 2> audio{};
    std::array<std::array<double, 64>, 16> kernels{};
    std::array<double, 256> dequeValues{};
    std::array<std::uint64_t, 256> dequeTimes{};
    std::array<double, 23> stage0{};
    std::array<double, 21> stage1{};
    std::array<double, 21> stage2{};
    int position = 0, pos0 = 0, pos1 = 0, pos2 = 0, recentHotSamples = 0;
    size_t dequeHead = 0, dequeSize = 0;
    std::uint64_t sampleTime = 0;
    double sum0 = 23.0, sum1 = 21.0, sum2 = 21.0;
    double corr0 = 0.0, corr1 = 0.0, corr2 = 0.0;
    double envelope = 1.0, release = .0004;
};
} // namespace wall
