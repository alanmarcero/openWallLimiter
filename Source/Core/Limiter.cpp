#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace wall {
namespace {
double dbGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
float bounded(float value, float low, float high, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
double clean(float sample) noexcept {
    return std::isfinite(sample) ? std::clamp(static_cast<double>(sample), -1e12, 1e12) : 0;
}
double slew(double sampleRate, double seconds) noexcept {
    return -std::expm1(-1.0 / (sampleRate * seconds));
}
} // namespace

void Limiter::Minimum::reset() noexcept { head = size = 0; }

double Limiter::Minimum::push(double value, std::uint64_t time, int window) noexcept {
    const auto capacity = entries.size();
    while (size && time - entries[head].time >= static_cast<std::uint64_t>(window)) {
        head = (head + 1) % capacity;
        --size;
    }
    while (size && entries[(head + size - 1) % capacity].value >= value)
        --size;
    entries[(head + size) % capacity] = {value, time};
    ++size;
    return entries[head].value;
}

void Limiter::prepare(double sampleRate, int channels) {
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0
        ? std::clamp(sampleRate, 8000.0, 3072000.0) : 48000.0;
    channelCount_ = std::clamp(channels, 1, 2);
    latency_ = static_cast<int>(std::ceil(sampleRate_ * .005));
    const auto window = static_cast<std::size_t>(latency_ + 1);
    for (auto& channel : channels_) {
        channel.audio.resize(static_cast<std::size_t>(latency_));
        channel.gains.resize(window);
        channel.minimum.entries.resize(window);
    }
    upwardAttack_ = slew(sampleRate_, .005);
    upwardRelease_ = slew(sampleRate_, .080);
    driveSlew_ = slew(sampleRate_, .005);
    setSettings(settings_);
    reset();
}

void Limiter::reset() noexcept {
    position_ = 0;
    time_ = 0;
    upwardEnvelope_ = 0;
    reductionDb_ = 0;
    drive_ = driveTarget_;
    started_ = false;
    for (auto& channel : channels_) {
        std::fill(channel.audio.begin(), channel.audio.end(), 0);
        std::fill(channel.gains.begin(), channel.gains.end(), 1);
        channel.minimum.reset();
        channel.sum = static_cast<double>(channel.gains.size());
        channel.sumCorrection = 0;
        channel.fast = channel.slow = 1;
    }
}

void Limiter::setSettings(const Settings& requested) noexcept {
    settings_.thresholdDb = bounded(requested.thresholdDb, -72, 0, 0);
    settings_.ceilingDb = bounded(requested.ceilingDb, -36, 0, -1);
    settings_.release = bounded(requested.release, .1f, 10, 1);
    settings_.clip = bounded(requested.clip, 0, 1, 0);
    settings_.upwardDb = bounded(requested.upwardDb, 0, 12, 0);
    settings_.stereoLink = bounded(requested.stereoLink, 0, 1, 1);
    settings_.mode = requested.mode >= Mode::Modern && requested.mode <= Mode::Classic
        ? requested.mode : Mode::Modern;
    driveTarget_ = dbGain(-settings_.thresholdDb);
    if (!started_) drive_ = driveTarget_;
    ceiling_ = dbGain(settings_.ceilingDb);
    upwardGain_ = dbGain(settings_.upwardDb);

    double fastSeconds = .045, slowSeconds = .180, clipDb = 1.5;
    switch (settings_.mode) {
        case Mode::Smooth: fastSeconds = .120; slowSeconds = .280; clipDb = .75; break;
        case Mode::Aggressive: fastSeconds = .025; slowSeconds = .025; clipDb = 6; break;
        case Mode::Safe: fastSeconds = .250; slowSeconds = .250; clipDb = 0; break;
        case Mode::Classic: fastSeconds = .100; slowSeconds = .100; clipDb = 3; break;
        case Mode::Modern: break;
    }
    releaseFast_ = slew(sampleRate_, fastSeconds * settings_.release);
    releaseSlow_ = slew(sampleRate_, slowSeconds * settings_.release);
    classicSlew_ = dbGain(60.0 / (sampleRate_ * settings_.release));
    clipAllowance_ = dbGain(clipDb * settings_.clip);
    clipKnee_ = settings_.mode == Mode::Safe ? 0 : .12 * settings_.clip;
}

double Limiter::releaseGain(Channel& channel, double target) const noexcept {
    switch (settings_.mode) {
        case Mode::Modern: {
            // Deep attenuation recovers more slowly than light transient control.
            const auto coefficient = releaseFast_ * channel.fast + releaseSlow_ * (1 - channel.fast);
            channel.fast = std::min(target, channel.fast + coefficient * (target - channel.fast));
            return channel.fast;
        }
        case Mode::Smooth:
            channel.fast = std::min(target, channel.fast + releaseFast_ * (target - channel.fast));
            channel.slow = std::min(target, channel.slow + releaseSlow_ * (target - channel.slow));
            return .35 * channel.fast + .65 * channel.slow;
        case Mode::Classic:
            channel.fast = std::min(target, channel.fast * classicSlew_);
            return channel.fast;
        case Mode::Aggressive:
        case Mode::Safe:
            channel.fast = std::min(target, channel.fast + releaseFast_ * (target - channel.fast));
            return channel.fast;
    }
    return target;
}

double Limiter::shape(double sample) const noexcept {
    if (clipKnee_ > 0) {
        const double magnitude = std::abs(sample), start = ceiling_ * (1 - clipKnee_);
        if (magnitude > start) {
            const double span = ceiling_ - start;
            sample = std::copysign(start + span * std::tanh((magnitude - start) / span), sample);
        }
    }
    return std::clamp(sample, -ceiling_, ceiling_);
}

void Limiter::process(float* left, float* right, int samples) noexcept {
    if (!left || samples <= 0) return;
    if (latency_ == 0) {
        for (int i = 0; i < samples; ++i) {
            left[i] = static_cast<float>(std::clamp(clean(left[i]), -ceiling_, ceiling_));
            if (right) right[i] = static_cast<float>(std::clamp(clean(right[i]), -ceiling_, ceiling_));
        }
        return;
    }
    const int window = latency_ + 1;
    double minimumGain = 1;
    started_ = true;
    for (int i = 0; i < samples; ++i) {
        drive_ += driveSlew_ * (driveTarget_ - drive_);
        std::array<double, 2> input{clean(left[i]) * drive_,
            channelCount_ == 2 && right ? clean(right[i]) * drive_ : 0};
        if (upwardGain_ > 1) {
            const auto peak = std::max(std::abs(input[0]), std::abs(input[1]));
            upwardEnvelope_ += (peak - upwardEnvelope_) * (peak > upwardEnvelope_ ? upwardAttack_ : upwardRelease_);
            // Gate below -90 dB; full upward lift from -72 to -36 dB,
            // tapering to unity by -18 dB. Silence remains exactly zero.
            const auto gate = std::clamp((upwardEnvelope_ - .0000316227766) / .000219565867, 0.0, 1.0);
            const auto quiet = std::clamp((.125892541 - upwardEnvelope_) / .110043609, 0.0, 1.0);
            const auto lift = 1 + (upwardGain_ - 1) * gate * quiet;
            input[0] *= lift;
            input[1] *= lift;
        }
        std::array<double, 2> target;
        for (int c = 0; c < 2; ++c)
            target[c] = std::min(1.0, ceiling_ * clipAllowance_ / std::max(std::abs(input[c]), 1e-30));
        const auto linked = std::min(target[0], target[1]);
        for (int c = 0; c < channelCount_; ++c) {
            auto& channel = channels_[c];
            const auto safeGain = target[c] + settings_.stereoLink * (linked - target[c]);
            const auto held = channel.minimum.push(safeGain, time_, window);
            const auto envelope = releaseGain(channel, held);
            const auto gainPosition = static_cast<std::size_t>(time_ % static_cast<std::uint64_t>(window));
            // Compensated summation prevents long-session moving-average drift.
            const auto difference = envelope - channel.gains[gainPosition] - channel.sumCorrection;
            const auto sum = channel.sum + difference;
            channel.sumCorrection = (sum - channel.sum) - difference;
            channel.sum = sum;
            channel.gains[gainPosition] = envelope;
            const auto gain = std::clamp(channel.sum / window, 0.0, 1.0);
            minimumGain = std::min(minimumGain, gain);
            const auto delayed = channel.audio[position_];
            channel.audio[position_] = input[c];
            const auto output = static_cast<float>(shape(delayed * gain));
            if (c == 0) left[i] = output;
            else if (right) right[i] = output;
        }
        position_ = (position_ + 1) % static_cast<std::size_t>(latency_);
        ++time_;
    }
    reductionDb_ = static_cast<float>(-20 * std::log10(std::max(minimumGain, 1e-12)));
}

} // namespace wall
