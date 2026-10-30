#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace wall {
namespace {
// Times are in milliseconds. The Release control scales the release, sustain and classic
// rates. clipDb is the clip allowance at Clip = 1.
struct Profile {
    double attack, hold, fast, slow, sustainAttack, sustainRelease, classicDbPerSecond, clipDb;
    bool rounded;
};
constexpr std::array<Profile, 5> profiles{{
    {2.5, 12, 45, 300, 400, 400, 0, 3, false},   // Modern
    {5, 12, 60, 350, 300, 350, 0, 1.5, true},    // Smooth
    {1, 3, 12, 150, 600, 200, 0, 5, false},      // Aggressive
    {5, 25, 100, 450, 250, 400, 0, 0, true},     // Safe
    {1.5, 10, 30, 300, 400, 400, 40, 3, false},  // Classic
}};
constexpr double maximumHoldSeconds = .025;

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

void Limiter::Box::reset() noexcept {
    std::fill(values.begin(), values.end(), 1);
    position = 0;
    sum = length;
    sumCorrection = 0;
}

void Limiter::Box::resize(int newLength) noexcept {
    length = newLength;
    sum = sumCorrection = 0;
    for (int i = 1; i <= length; ++i)
        sum += values[(position + values.size() - static_cast<std::size_t>(i)) % values.size()];
}

double Limiter::Box::push(double value) noexcept {
    const auto leaving = values[(position + values.size() - static_cast<std::size_t>(length)) % values.size()];
    values[position] = value;
    position = (position + 1) % values.size();
    // Compensated summation prevents long-session moving-average drift.
    const auto difference = value - leaving - sumCorrection;
    const auto next = sum + difference;
    sumCorrection = (next - sum) - difference;
    sum = next;
    return std::clamp(sum / length, 0.0, 1.0);
}

void Limiter::prepare(double sampleRate, int channels) {
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0
        ? std::clamp(sampleRate, 8000.0, 3072000.0) : 48000.0;
    channelCount_ = std::clamp(channels, 1, 2);
    // A multiple of 16 keeps the base-rate latency whole at every oversampling factor.
    latency_ = static_cast<int>(std::ceil(sampleRate_ * .005 / 16)) * 16;
    const auto window = static_cast<std::size_t>(latency_ + 1);
    const auto longestHold = static_cast<std::size_t>(std::ceil(sampleRate_ * maximumHoldSeconds));
    for (auto& channel : channels_) {
        channel.audio.resize(static_cast<std::size_t>(latency_));
        channel.allowances.resize(static_cast<std::size_t>(latency_));
        channel.gains.resize(window);
        channel.minimum.entries.resize(window + longestHold);
        channel.first.values.resize(window + 1);
        channel.second.values.resize(window + 1);
    }
    upwardAttack_ = slew(sampleRate_, .005);
    upwardRelease_ = slew(sampleRate_, .080);
    driveSlew_ = slew(sampleRate_, .005);
    attack_ = 0;
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
        std::fill(channel.allowances.begin(), channel.allowances.end(), 1);
        std::fill(channel.gains.begin(), channel.gains.end(), 1);
        channel.minimum.reset();
        channel.first.reset();
        channel.second.reset();
        channel.releaseDb = channel.roundDb = channel.sustainDb = 0;
    }
}

void Limiter::setSettings(const Settings& requested) noexcept {
    settings_.thresholdDb = bounded(requested.thresholdDb, -72, 0, 0);
    settings_.ceilingDb = bounded(requested.ceilingDb, -36, 0, -1);
    settings_.release = bounded(requested.release, .1f, 10, 1);
    settings_.clip = bounded(requested.clip, 0, 1, 0);
    settings_.upwardDb = bounded(requested.upwardDb, 0, 12, 0);
    settings_.stereoLink = bounded(requested.stereoLink, 0, 1, 1);
    const auto mode = requested.mode >= Mode::Modern && requested.mode <= Mode::Classic
        ? requested.mode : Mode::Modern;
    if (mode != settings_.mode) {
        // Both release stages start from the current attenuation. This prevents
        // a stale rounding stage and a gain reset.
        for (auto& channel : channels_)
            channel.releaseDb = channel.roundDb = std::max(channel.releaseDb, channel.roundDb);
    }
    settings_.mode = mode;
    driveTarget_ = dbGain(-settings_.thresholdDb);
    if (!started_) drive_ = driveTarget_;
    ceiling_ = dbGain(settings_.ceilingDb);
    upwardGain_ = dbGain(settings_.upwardDb);

    const auto& profile = profiles[static_cast<std::size_t>(mode)];
    const double scale = settings_.release * .001;
    releaseFast_ = slew(sampleRate_, profile.fast * scale);
    releaseSlow_ = slew(sampleRate_, profile.slow * scale);
    sustainAttack_ = slew(sampleRate_, profile.sustainAttack * scale);
    sustainRelease_ = slew(sampleRate_, profile.sustainRelease * scale);
    classicStepDb_ = profile.classicDbPerSecond / (sampleRate_ * settings_.release);
    clipDb_ = profile.clipDb * settings_.clip;
    hold_ = static_cast<int>(std::round(sampleRate_ * profile.hold * .001));
    const auto attack = std::clamp(static_cast<int>(std::round(sampleRate_ * profile.attack * .001)), 1,
                                   std::max(latency_, 1));
    if (attack != attack_ && latency_ > 0) {
        // Two cascaded averages span attack + 1 samples and give an S-shaped gain ramp.
        attack_ = attack;
        const auto first = (attack_ + 3) / 2;
        for (auto& channel : channels_) {
            channel.first.resize(first);
            channel.second.resize(attack_ + 2 - first);
        }
    }
}

double Limiter::releaseDb(Channel& channel, double heldDb) const noexcept {
    const auto& profile = profiles[static_cast<std::size_t>(settings_.mode)];
    auto& release = channel.releaseDb;
    // Attenuation above the sustained level is a transient and releases fast.
    // The sustained part releases slowly, so the gain does not follow the waveform.
    const auto transient = std::max(0.0, release - std::max(heldDb, channel.sustainDb));
    const auto body = std::max(0.0, std::min(release, channel.sustainDb) - heldDb);
    const auto adaptive = release - transient * releaseFast_ - body * releaseSlow_;
    release = std::max(heldDb, profile.classicDbPerSecond > 0 ? release - classicStepDb_ : adaptive);
    // Sustain follows the held target, so a release tail does not count as sustained.
    channel.sustainDb += (heldDb - channel.sustainDb)
        * (heldDb > channel.sustainDb ? sustainAttack_ : sustainRelease_);
    if (!profile.rounded) return channel.roundDb = release;
    // A second release stage rounds the recovery corner.
    channel.roundDb = std::max(release, channel.roundDb + releaseFast_ * (release - channel.roundDb));
    return channel.roundDb;
}

double Limiter::shape(double sample, double allowance) const noexcept {
    // The quadratic knee starts at ceiling * (2 - allowance). It reaches the ceiling with
    // zero slope at ceiling * allowance, the largest magnitude the gain path passes.
    const double magnitude = std::abs(sample), start = ceiling_ * (2 - allowance);
    if (allowance > 1 && magnitude > start) {
        const double over = magnitude - start;
        sample = std::copysign(magnitude - over * over / (4 * ceiling_ * (allowance - 1)), sample);
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
    // The minimum covers the attack span plus the hold. The gain ring delays the smoothed
    // gain by latency - attack samples, which aligns it with its audio sample.
    const int window = attack_ + 1 + hold_;
    const auto gainWindow = static_cast<std::uint64_t>(latency_ + 1);
    const auto gainDelay = static_cast<std::uint64_t>(latency_ - attack_);
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
        // Sustained attenuation shrinks the clip allowance, so clipping applies to transients.
        std::array<double, 2> allowance, target;
        for (int c = 0; c < 2; ++c) {
            const auto sustainDb = channels_[c].sustainDb;
            allowance[c] = dbGain(clipDb_ / (1 + sustainDb * sustainDb));
            target[c] = std::min(1.0, ceiling_ * allowance[c] / std::max(std::abs(input[c]), 1e-30));
        }
        const auto linked = std::min(target[0], target[1]);
        for (int c = 0; c < channelCount_; ++c) {
            auto& channel = channels_[c];
            const auto safeGain = target[c] + settings_.stereoLink * (linked - target[c]);
            const auto held = channel.minimum.push(safeGain, time_, window);
            const auto envelope = dbGain(-releaseDb(channel, -20 * std::log10(std::max(held, 1e-30))));
            channel.gains[time_ % gainWindow] = channel.second.push(channel.first.push(envelope));
            const auto gain = channel.gains[(time_ + gainWindow - gainDelay) % gainWindow];
            minimumGain = std::min(minimumGain, gain);
            const auto delayed = channel.audio[position_];
            const auto delayedAllowance = channel.allowances[position_];
            channel.audio[position_] = input[c];
            channel.allowances[position_] = allowance[c];
            const auto output = static_cast<float>(shape(delayed * gain, delayedAllowance));
            if (c == 0) left[i] = output;
            else if (right) right[i] = output;
        }
        position_ = (position_ + 1) % static_cast<std::size_t>(latency_);
        ++time_;
    }
    reductionDb_ = static_cast<float>(-20 * std::log10(std::max(minimumGain, 1e-12)));
}

} // namespace wall
