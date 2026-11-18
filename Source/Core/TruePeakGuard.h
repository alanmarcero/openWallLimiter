#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace wall {
// Post-downsampling lookahead guard. Its detector reconstructs eight phases with
// a windowed-sinc FIR, then anticipates those peaks in the delayed audio path.
class TruePeakGuard {
  public:
    static constexpr int delaySamples = 128;
    // Reconstruction reserve. The main limiter also limits to it, so the guard only removes overshoot.
    static constexpr float reserveDb = -.3f;
    static constexpr double reserve = .96605087899;
    void prepare(double sr) noexcept {
        release = 1 - std::exp(-1 / (sr * .05));
        reset();
        for (int phase = 0; phase < 8; ++phase) {
            double normalization = 0;
            for (int k = 0; k < 96; ++k) {
                double t = double(k) - 47. - double(phase) * .125;
                double v = std::abs(t) < 1e-10 ? 1 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
                v *= .5 + .5 * std::cos(std::numbers::pi * t / 49.);
                kernels[size_t(phase)][size_t(k)] = v;
                normalization += v;
            }
            for (auto &v : kernels[size_t(phase)])
                v /= normalization;
        }
    }
    void reset() noexcept {
        for (auto &c : audio)
            c.fill(0);
        targets.fill(1);
        gains.fill(1);
        position = targetPosition = gainPosition = 0;
        recentHotSamples = activeTargetSamples = 0;
        sum = 64;
        envelope = 1;
    }
    void process(float &left, float &right, float ceiling, bool enabled) noexcept {
        const std::array<float, 2> delayed{audio[0][size_t(position)], audio[1][size_t(position)]};
        audio[0][size_t(position)] = left;
        audio[1][size_t(position)] = right;
        if (!enabled) {
            left = delayed[0];
            right = delayed[1];
            position = (position + 1) & (delaySamples - 1);
            return;
        }
        double peak = std::max(std::abs(left), std::abs(right));
        if (peak >= double(ceiling) * 0.30 || recentHotSamples > 0) {
            if (peak >= double(ceiling) * 0.30)
                recentHotSamples = 96;
            else
                --recentHotSamples;
            for (size_t channel = 0; channel < 2; ++channel)
                for (size_t phase = 1; phase < 8; ++phase) {
                    const auto &kernel = kernels[phase];
                    double v = 0;
                    for (int k = 0; k < 96; ++k)
                        v += kernel[size_t(k)] *
                             audio[channel][size_t((position - k + delaySamples) & (delaySamples - 1))];
                    peak = std::max(peak, std::abs(v));
                }
        }
        const double target = std::min(1., double(ceiling) * reserve / std::max(peak, 1e-20));
        targets[size_t(targetPosition)] = target;
        targetPosition = (targetPosition + 1) % int(targets.size());
        if (target < 1.0)
            activeTargetSamples = int(targets.size());
        else if (activeTargetSamples > 0)
            --activeTargetSamples;
        const double held = activeTargetSamples > 0 ? *std::min_element(targets.begin(), targets.end()) : 1.0;
        envelope = std::min(held, envelope + release * (held - envelope));
        sum += envelope - gains[size_t(gainPosition)];
        gains[size_t(gainPosition)] = envelope;
        gainPosition = (gainPosition + 1) & 63;
        const float gain = float(std::clamp(sum / 64., 0., 1.));
        left = delayed[0] * gain;
        right = delayed[1] * gain;
        position = (position + 1) & (delaySamples - 1);
    }

  private:
    std::array<std::array<float, delaySamples>, 2> audio{};
    std::array<std::array<double, 96>, 8> kernels{};
    std::array<double, 192> targets{};
    std::array<double, 64> gains{};
    int position = 0, targetPosition = 0, gainPosition = 0;
    int recentHotSamples = 0, activeTargetSamples = 0;
    double sum = 64, envelope = 1, release = .0004;
};
} // namespace wall
