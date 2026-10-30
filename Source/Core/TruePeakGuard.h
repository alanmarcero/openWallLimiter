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
        sum = 64;
        envelope = 1;
    }
    void process(float &left, float &right, float ceiling, bool enabled) noexcept {
        const std::array<float, 2> delayed{audio[0][size_t(position)], audio[1][size_t(position)]};
        audio[0][size_t(position)] = left;
        audio[1][size_t(position)] = right;
        double peak = std::max(std::abs(left), std::abs(right));
        for (size_t channel = 0; channel < 2; ++channel)
            for (const auto &kernel : kernels) {
                double v = 0;
                for (int k = 0; k < 96; ++k)
                    v += kernel[size_t(k)] *
                         audio[channel][size_t((position - k + delaySamples) % delaySamples)];
                peak = std::max(peak, std::abs(v));
            }
        const double target = std::min(1., double(ceiling) * reserve / std::max(peak, 1e-20));
        targets[size_t(targetPosition)] = target;
        targetPosition = (targetPosition + 1) % int(targets.size());
        const double held = *std::min_element(targets.begin(), targets.end());
        envelope = std::min(held, envelope + release * (held - envelope));
        sum += envelope - gains[size_t(gainPosition)];
        gains[size_t(gainPosition)] = envelope;
        gainPosition = (gainPosition + 1) % 64;
        const float gain = enabled ? float(std::clamp(sum / 64., 0., 1.)) : 1.f;
        left = delayed[0] * gain;
        right = delayed[1] * gain;
        position = (position + 1) % delaySamples;
    }

  private:
    std::array<std::array<float, delaySamples>, 2> audio{};
    std::array<std::array<double, 96>, 8> kernels{};
    std::array<double, 192> targets{};
    std::array<double, 64> gains{};
    int position = 0, targetPosition = 0, gainPosition = 0;
    double sum = 64, envelope = 1, release = .0004;
};
} // namespace wall
