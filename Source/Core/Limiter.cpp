#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wall {
namespace {
constexpr double maximumHoldSeconds = .040;

double dbGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
float bounded(float value, float low, float high, float fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, low, high);
}
double clean(float sample) noexcept {
    if (!std::isfinite(sample)) return 0;
    return std::clamp(static_cast<double>(sample), -1e12, 1e12);
}
double slew(double sampleRate, double seconds) noexcept {
    return -std::expm1(-1.0 / (sampleRate * seconds));
}
double smoothstep(double edge0, double edge1, double x) noexcept {
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
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
    int i = 1;
    while (i <= length) {
        sum += values[(position + values.size() - static_cast<std::size_t>(i)) % values.size()];
        ++i;
    }
}

double Limiter::Box::push(double value) noexcept {
    if (length <= 1) {
        values[position] = value;
        position = (position + 1) % values.size();
        sum = value;
        sumCorrection = 0;
        return std::clamp(value, 0.0, 1.0);
    }
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

void Limiter::TptBiquad::set(double b0, double b1, double b2, double d1, double d2) noexcept {
    const double dPlus = std::max(1e-18, 1.0 + d1 + d2);
    const double dMinus = std::max(1e-18, 1.0 - d1 + d2);
    const double rootProd = std::sqrt(dPlus * dMinus);
    const double g = std::sqrt(dPlus / dMinus);
    const double kDamp = 2.0 * (1.0 - d2) / rootProd;
    a1 = 1.0 / (1.0 + g * (g + kDamp));
    a2 = g * a1;
    a3 = g * a2;
    const double mHp = (b0 - b1 + b2) / dMinus;
    const double mBp = 2.0 * (b0 - b2) / rootProd;
    const double mLp = (b0 + b1 + b2) / dPlus;
    m0 = mHp;
    m1 = mBp - mHp * kDamp;
    m2 = mLp - mHp;
}

void Limiter::TptBiquad::setButterworth(double cutoffHz, double q, double sampleRate, bool highpass) noexcept {
    const double clampedFc = std::clamp(cutoffHz, 5.0, .45 * sampleRate);
    const double g = std::tan(std::numbers::pi * clampedFc / sampleRate);
    const double kDamp = 1.0 / q;
    a1 = 1.0 / (1.0 + g * (g + kDamp));
    a2 = g * a1;
    a3 = g * a2;
    m0 = highpass ? 1.0 : 0.0;
    m1 = highpass ? -kDamp : 0.0;
    m2 = highpass ? -1.0 : 1.0;
}

double Limiter::TptBiquad::process(double x) noexcept {
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    if (std::abs(ic1) < 1e-25) ic1 = 0;
    if (std::abs(ic2) < 1e-25) ic2 = 0;
    return m0 * x + m1 * v1 + m2 * v2;
}

void Limiter::Envelope::reset() noexcept {
    std::fill(gains.begin(), gains.end(), 1);
    minimum.reset();
    std::ranges::for_each(stages, [](Box& stage) { stage.reset(); });
    releaseDb = roundDb = sustainDb = 0;
}

void Limiter::prepare(double sampleRate, int channels) {
    sampleRate_ = 48000.0;
    if (std::isfinite(sampleRate) && sampleRate > 0)
        sampleRate_ = std::clamp(sampleRate, 8000.0, 3072000.0);
    channelCount_ = std::clamp(channels, 1, 2);
    // A multiple of 16 keeps the base-rate PDC budget whole at every oversampling factor.
    latency_ = static_cast<int>(std::ceil(sampleRate_ * (maxLookaheadMs * .001) / 16.0)) * 16;
    const auto window = static_cast<std::size_t>(latency_ + 1);
    const auto longestHold = static_cast<std::size_t>(std::ceil(sampleRate_ * maximumHoldSeconds));
    const auto setupEnvelope = [&](Envelope& env) {
        env.gains.resize(window);
        env.minimum.entries.resize(window + longestHold);
        std::ranges::for_each(env.stages, [&](Box& stage) { stage.values.resize(window + 2); });
    };
    setupEnvelope(subEnvelope_);

    // ITU BS.1770 K-weighting shelf and RLB high-pass coefficients (matches LoudnessMeter).
    double k = std::tan(std::numbers::pi * 1681.974450955533 / sampleRate_);
    const double vh = std::pow(10.0, 3.999843853973347 / 20.0);
    const double vb = std::pow(vh, .4996667741545416);
    constexpr double qShelf = .7071752369554196;
    double d = 1.0 + k / qShelf + k * k;
    const std::array<double, 5> shelf{
        (vh + vb * k / qShelf + k * k) / d, 2.0 * (k * k - vh) / d,
        (vh - vb * k / qShelf + k * k) / d, 2.0 * (k * k - 1.0) / d, (1.0 - k / qShelf + k * k) / d};
    k = std::tan(std::numbers::pi * 38.13547087602444 / sampleRate_);
    constexpr double qHp = .5003270373238773;
    d = 1.0 + k / qHp + k * k;
    const std::array<double, 5> rlb{1.0 / d, -2.0 / d, 1.0 / d, 2.0 * (k * k - 1.0) / d, (1.0 - k / qHp + k * k) / d};

    constexpr double qSub1 = .541196100146197;
    constexpr double qSub2 = 1.3065629648763766;
    std::ranges::for_each(channels_, [&](Channel& channel) {
        channel.audio.resize(static_cast<std::size_t>(latency_));
        channel.audioSub.resize(static_cast<std::size_t>(latency_));
        channel.allowances.resize(static_cast<std::size_t>(latency_));
        setupEnvelope(channel.envelope);
        channel.kFilter[0].set(shelf[0], shelf[1], shelf[2], shelf[3], shelf[4]);
        channel.kFilter[1].set(rlb[0], rlb[1], rlb[2], rlb[3], rlb[4]);
        channel.subLpFilter[0].setButterworth(120.0, qSub1, sampleRate_, false);
        channel.subLpFilter[1].setButterworth(120.0, qSub2, sampleRate_, false);
        channel.subHpFilter[0].setButterworth(120.0, qSub1, sampleRate_, true);
        channel.subHpFilter[1].setButterworth(120.0, qSub2, sampleRate_, true);
        channel.hfHpFilter[0].setButterworth(250.0, qSub1, sampleRate_, true);
        channel.hfHpFilter[1].setButterworth(250.0, qSub2, sampleRate_, true);
    });

    upwardAttack_ = slew(sampleRate_, .060);
    upwardRelease_ = slew(sampleRate_, .250);
    kRmsSlew_ = slew(sampleRate_, .050);
    peakRelease_ = slew(sampleRate_, .080);
    hfFastAttack_ = slew(sampleRate_, .0008);
    hfFastRelease_ = slew(sampleRate_, .010);
    hfSlowSlew_ = slew(sampleRate_, .045);
    msSlew_ = slew(sampleRate_, .005);
    driveSlew_ = slew(sampleRate_, .005);
    attack_ = -1;
    setSettings(settings_);
    reset();
}

void Limiter::reset() noexcept {
    position_ = 0;
    time_ = 0;
    upwardEnergy_ = 0;
    midEnv_ = sideEnv_ = 0;
    reductionDb_ = 0;
    drive_ = driveTarget_;
    started_ = false;
    subEnvelope_.reset();
    std::ranges::for_each(channels_, [](Channel& channel) {
        std::fill(channel.audio.begin(), channel.audio.end(), 0);
        std::fill(channel.audioSub.begin(), channel.audioSub.end(), 0);
        std::fill(channel.allowances.begin(), channel.allowances.end(), 1);
        channel.envelope.reset();
        channel.kFilter[0].reset();
        channel.kFilter[1].reset();
        channel.subLpFilter[0].reset();
        channel.subLpFilter[1].reset();
        channel.subHpFilter[0].reset();
        channel.subHpFilter[1].reset();
        channel.hfHpFilter[0].reset();
        channel.hfHpFilter[1].reset();
        channel.subEnergy = channel.hiEnergy = 0;
        channel.kMeanSq = channel.rmsMeanSq = channel.peakEnv = channel.hfFast = channel.hfSlow = channel.fullSlow = 0;
    });
}

void Limiter::setSettings(const Settings& requested) noexcept {
    settings_.thresholdDb = bounded(requested.thresholdDb, -72, 0, 0);
    settings_.ceilingDb = bounded(requested.ceilingDb, -36, 0, -1);
    settings_.release = bounded(requested.release, .1f, 10, 1);
    settings_.clip = bounded(requested.clip, 0, 1, 0);
    settings_.upwardDb = bounded(requested.upwardDb, 0, 12, 0);
    settings_.stereoLink = bounded(requested.stereoLink, 0, 1, 1);
    settings_.releaseLink = bounded(requested.releaseLink, 0, 1, 0);
    settings_.lookaheadMs = bounded(requested.lookaheadMs, 0, maxLookaheadMs, 5);
    const bool validMode = requested.mode >= Mode::Modern && requested.mode <= Mode::Classic;
    const auto mode = validMode ? requested.mode : Mode::Modern;
    if (mode != settings_.mode) {
        // Carry outgoing attenuation into both release stages so mode switches never reset gain.
        subEnvelope_.releaseDb = subEnvelope_.roundDb = std::max(subEnvelope_.releaseDb, subEnvelope_.roundDb);
        std::ranges::for_each(channels_, [](Channel& channel) {
            auto& env = channel.envelope;
            env.releaseDb = env.roundDb = std::max(env.releaseDb, env.roundDb);
        });
    }
    settings_.mode = mode;
    driveTarget_ = dbGain(-settings_.thresholdDb);
    if (!started_) drive_ = driveTarget_;
    ceiling_ = dbGain(settings_.ceilingDb);
    upwardGain_ = dbGain(settings_.upwardDb);

    const auto& profile = modeProfiles[static_cast<std::size_t>(mode)];
    const double scale = settings_.release * .001;
    releaseFast_ = slew(sampleRate_, profile.fast * scale);
    releaseSlow_ = slew(sampleRate_, profile.slow * scale);
    sustainAttack_ = slew(sampleRate_, profile.sustainAttack * scale);
    sustainRelease_ = slew(sampleRate_, profile.sustainRelease * scale);
    classicStepDb_ = profile.classicDbPerSecond / (sampleRate_ * settings_.release);
    clipDb_ = profile.clipDb * settings_.clip;
    hold_ = static_cast<int>(std::round(sampleRate_ * profile.hold * .001));
    lookahead_ = std::clamp(static_cast<int>(std::round(sampleRate_ * settings_.lookaheadMs * .001)), 0, latency_);
    int attack = 0;
    if (lookahead_ > 0)
        attack = std::clamp(static_cast<int>(std::round(lookahead_ * profile.attackRatio)), 1, lookahead_);
    if (attack != attack_ && latency_ > 0) {
        // Four unequal moving-average passes form a cubic B-spline kernel of exact span `attack`.
        attack_ = attack;
        const int d0 = static_cast<int>(std::round(.34 * attack_));
        const int d1 = static_cast<int>(std::round(.28 * (attack_ - d0) / .66));
        const int d2 = static_cast<int>(std::round(.22 * (attack_ - d0 - d1) / .38));
        const int d3 = attack_ - d0 - d1 - d2;
        const std::array<int, 4> lengths{1 + d0, 1 + d1, 1 + d2, 1 + d3};
        const auto resizeStages = [&](Envelope& env) {
            std::size_t s = 0;
            while (s < env.stages.size()) {
                env.stages[s].resize(lengths[s]);
                ++s;
            }
        };
        resizeStages(subEnvelope_);
        std::ranges::for_each(channels_, [&](Channel& channel) { resizeStages(channel.envelope); });
    }
}

double Limiter::advanceRelease(Envelope& env, double heldDb, double kOverDb, double crest) const noexcept {
    const auto& profile = modeProfiles[static_cast<std::size_t>(settings_.mode)];
    auto& release = env.releaseDb;
    const auto floorDb = std::max(heldDb, env.sustainDb);
    const auto transient = std::max(0.0, release - floorDb);
    const auto body = std::max(0.0, std::min(release, env.sustainDb) - heldDb);
    const auto fastCurve = releaseFast_ * (1.0 + .10 * std::min(12.0, transient) + 1.1 * crest);
    const auto adaptive = release - transient * fastCurve - body * releaseSlow_;
    const auto nextRelease = profile.classicDbPerSecond > 0 ? release - classicStepDb_ : adaptive;
    release = std::max(heldDb, nextRelease);

    // Slow reservoir tracks 50 ms K-weighted RMS overdrive rather than raw sub-bass peak GR.
    const double reservoirTarget = std::min(heldDb, kOverDb);
    const double sustainRate = reservoirTarget > env.sustainDb ? sustainAttack_ : sustainRelease_;
    env.sustainDb += (reservoirTarget - env.sustainDb) * sustainRate;
    if (!profile.rounded) return env.roundDb = release;
    const auto roundSpan = std::max(0.0, env.roundDb - floorDb);
    const auto roundCurve = releaseFast_ * (1.0 + .10 * std::min(12.0, roundSpan) + 1.1 * crest);
    env.roundDb = std::max(release, env.roundDb + roundCurve * (release - env.roundDb));
    return env.roundDb;
}

double Limiter::shape(double sample, double allowance) const noexcept {
    const double clampedAllowance = std::min(allowance, 1.98);
    const double magnitude = std::abs(sample);
    if (clampedAllowance <= 1.0) return std::clamp(sample, -ceiling_, ceiling_);

    // Quartic C2 soft knee: matches f'=1, f''=0 at knee onset and f'=0, f''=0 at ceiling.
    const double start = ceiling_ * (2.0 - clampedAllowance);
    const double end = ceiling_ * clampedAllowance;
    if (magnitude <= start) return sample;
    if (magnitude >= end) return std::copysign(ceiling_, sample);
    const double u = (magnitude - start) / (end - start);
    const double inv = 1.0 - u;
    const double curved = ceiling_ - (ceiling_ - start) * (inv * inv * inv) * (1.0 + u);
    return std::copysign(std::min(curved, ceiling_), sample);
}

void Limiter::process(float* left, float* right, int samples) noexcept {
    if (!left || samples <= 0) return;
    if (latency_ == 0) {
        int i = 0;
        while (i < samples) {
            left[i] = static_cast<float>(std::clamp(clean(left[i]), -ceiling_, ceiling_));
            if (right) right[i] = static_cast<float>(std::clamp(clean(right[i]), -ceiling_, ceiling_));
            ++i;
        }
        return;
    }

    const int activeAttack = std::max(attack_, 0);
    const int window = activeAttack + 1 + hold_;
    const auto gainWindow = static_cast<std::uint64_t>(latency_ + 1);
    const auto gainDelay = static_cast<std::uint64_t>(latency_ - activeAttack);
    double minimumGain = 1;
    started_ = true;
    const bool hasClip = std::abs(clipDb_) > 1e-6;
    const auto pushStages = [](Envelope& env, double value) noexcept {
        double v = value;
        std::size_t s = 0;
        while (s < env.stages.size()) {
            v = env.stages[s].push(v);
            ++s;
        }
        return v;
    };

    int i = 0;
    while (i < samples) {
        drive_ += driveSlew_ * (driveTarget_ - drive_);
        double rightIn = 0.0;
        if (channelCount_ == 2 && right) rightIn = clean(right[i]) * drive_;
        std::array<double, 2> input{clean(left[i]) * drive_, rightIn};

        // Update BS.1770 K-weighted signal per active channel for upward gating and dual release.
        std::array<double, 2> kWeighted{};
        int kc = 0;
        while (kc < channelCount_) {
            auto& ch = channels_[static_cast<std::size_t>(kc)];
            kWeighted[static_cast<std::size_t>(kc)] = ch.kFilter[1].process(ch.kFilter[0].process(input[static_cast<std::size_t>(kc)]));
            ++kc;
        }

        if (upwardGain_ > 1.0) {
            const double stereoEnergy = kWeighted[0] * kWeighted[0] + kWeighted[1] * kWeighted[1];
            const double monoEnergy = 2.0 * kWeighted[0] * kWeighted[0];
            const double instEnergy = channelCount_ == 2 ? stereoEnergy : monoEnergy;
            const double upRate = instEnergy > upwardEnergy_ ? upwardAttack_ : upwardRelease_;
            upwardEnergy_ += upRate * (instEnergy - upwardEnergy_);
            double lufs = -140.0;
            if (upwardEnergy_ > 1e-30) lufs = -.691 + 10.0 * std::log10(upwardEnergy_);

            // Lift quiet musical detail (-58 to -18 LUFS); expand hiss/rumble downward below -60 LUFS.
            const double liftZone = smoothstep(-60.0, -48.0, lufs) * (1.0 - smoothstep(-32.0, -18.0, lufs));
            const double expandDb = -std::min(6.0, .6 * settings_.upwardDb) * (1.0 - smoothstep(-84.0, -60.0, lufs));
            const double totalUpDb = settings_.upwardDb * liftZone + expandDb;
            const double lift = dbGain(totalUpDb);
            input[0] *= lift;
            input[1] *= lift;
            kWeighted[0] *= lift;
            kWeighted[1] *= lift;
        }

        // Split <120 Hz sub-bass (4-pole Butterworth LP) and >250 Hz transient flux per channel.
        std::array<double, 2> sub{}, hi{}, absHp{}, subShareIn{}, allowance{1.0, 1.0}, limits{}, kOverDb{}, crest{};
        int c = 0;
        while (c < channelCount_) {
            const auto idx = static_cast<std::size_t>(c);
            auto& ch = channels_[idx];
            const double x = input[idx];
            const double absX = std::abs(x);

            sub[idx] = ch.subLpFilter[1].process(ch.subLpFilter[0].process(x));
            hi[idx] = x - sub[idx];
            const double hpIn = ch.subHpFilter[1].process(ch.subHpFilter[0].process(x));
            absHp[idx] = std::abs(hpIn);
            ch.subEnergy += msSlew_ * (sub[idx] * sub[idx] - ch.subEnergy);
            ch.hiEnergy += msSlew_ * (hpIn * hpIn - ch.hiEnergy);
            subShareIn[idx] = ch.subEnergy / (ch.subEnergy + ch.hiEnergy + 1e-30);

            const double absHf = std::abs(ch.hfHpFilter[1].process(ch.hfHpFilter[0].process(x)));
            const double hfRate = absHf > ch.hfFast ? hfFastAttack_ : hfFastRelease_;
            ch.hfFast += hfRate * (absHf - ch.hfFast);
            ch.hfSlow += hfSlowSlew_ * (absHf - ch.hfSlow);
            ch.fullSlow += hfSlowSlew_ * (absX - ch.fullSlow);

            if (hasClip) {
                const double hfDenom = std::max(ch.hfSlow, .04 * ch.fullSlow + 1e-12);
                const double flux = std::clamp((ch.hfFast / hfDenom - 1.15) / 1.5, 0.0, 1.0);
                allowance[idx] = dbGain(clipDb_ * flux);
            }
            limits[idx] = ceiling_ * allowance[idx];

            const double kw = kWeighted[idx];
            ch.kMeanSq += kRmsSlew_ * (kw * kw - ch.kMeanSq);
            ch.rmsMeanSq += kRmsSlew_ * (x * x - ch.rmsMeanSq);
            const double kRms = std::sqrt(std::max(ch.kMeanSq, 1e-30));
            const double rms = std::sqrt(std::max(ch.rmsMeanSq, 1e-30));
            const double decayedPeak = ch.peakEnv + peakRelease_ * (absX - ch.peakEnv);
            ch.peakEnv = absX > ch.peakEnv ? absX : decayedPeak;
            const double kRatio = (std::numbers::sqrt2 * kRms) / std::max(ceiling_, 1e-12);
            if (kRatio > 1.0) kOverDb[idx] = 20.0 * std::log10(kRatio);
            crest[idx] = std::clamp(ch.peakEnv / std::max(std::numbers::sqrt2 * rms, 1e-12) - 1.0, 0.0, 8.0);
            ++c;
        }

        // Compute 100%-linked <120 Hz sub target and Mid/Side-weighted >120 Hz channel targets.
        double subTarget = 1.0;
        std::array<double, 2> hiTarget{1.0, 1.0};
        double minLimit = limits[0];
        if (channelCount_ == 2) minLimit = std::min(limits[0], limits[1]);
        const double effSub0 = std::min(std::abs(sub[0]), std::abs(input[0]));
        double effSub1 = 0.0;
        if (channelCount_ == 2) effSub1 = std::min(std::abs(sub[1]), std::abs(input[1]));
        const double subPeak = std::max(effSub0, effSub1);
        const bool anyOver = std::abs(input[0]) > limits[0]
            || (channelCount_ == 2 && std::abs(input[1]) > limits[1]);
        if (anyOver)
            subTarget = std::min(1.0, minLimit / std::max(subPeak, 1e-30));

        int chIdx = 0;
        while (chIdx < channelCount_) {
            const auto u = static_cast<std::size_t>(chIdx);
            const double absIn = std::abs(input[u]);
            if (absIn > limits[u]) {
                const double fullTarget = limits[u] / absIn;
                if (absHp[u] <= .35 * limits[u]) {
                    subTarget = std::min(subTarget, fullTarget);
                }
                if (absHp[u] > .35 * limits[u] && subTarget * std::abs(sub[u]) > .85 * limits[u]) {
                    subTarget = std::min(subTarget, .85 * limits[u] / std::abs(sub[u]));
                }
            }
            ++chIdx;
        }
        chIdx = 0;
        while (chIdx < channelCount_) {
            const auto u = static_cast<std::size_t>(chIdx);
            const double absIn = std::abs(input[u]);
            if (absIn > limits[u]) {
                const double fullTarget = limits[u] / absIn;
                const double absHi = std::abs(hi[u]);
                if (absHp[u] <= .35 * limits[u] || absHi <= 1e-12) {
                    hiTarget[u] = fullTarget;
                }
                if (absHp[u] > .35 * limits[u] && absHi > 1e-12) {
                    const double subContrib = subTarget * sub[u] * std::copysign(1.0, hi[u]);
                    hiTarget[u] = std::min(fullTarget, std::clamp((limits[u] - subContrib) / absHi, 0.0, 1.0));
                }
            }
            ++chIdx;
        }

        // Mid/Side stereo link weighting above 120 Hz.
        double hiLink = settings_.stereoLink;
        if (channelCount_ == 2) {
            const double mid = .5 * (hi[0] + hi[1]);
            const double side = .5 * (hi[0] - hi[1]);
            midEnv_ += msSlew_ * (mid * mid - midEnv_);
            sideEnv_ += msSlew_ * (side * side - sideEnv_);
            const double midShare = midEnv_ / (midEnv_ + sideEnv_ + 1e-30);
            hiLink = settings_.stereoLink * (midShare + (1.0 - midShare) * settings_.stereoLink);
        }
        double linkedHi = hiTarget[0];
        double maxKOver = kOverDb[0];
        double maxCrest = crest[0];
        if (channelCount_ == 2) {
            linkedHi = std::min(hiTarget[0], hiTarget[1]);
            maxKOver = std::max(kOverDb[0], kOverDb[1]);
            maxCrest = std::max(crest[0], crest[1]);
        }

        const double heldSub = subEnvelope_.minimum.push(subTarget, time_, window);
        double subRelAmount = 0.0;
        const bool subIdle = heldSub >= 1.0 && subEnvelope_.releaseDb <= 0.0
            && subEnvelope_.roundDb <= 0.0 && subEnvelope_.sustainDb <= 0.0;
        if (!subIdle) {
            subRelAmount = advanceRelease(subEnvelope_, -20.0 * std::log10(std::max(heldSub, 1e-30)),
                                          maxKOver, maxCrest);
        }
        double subEnv = 1.0;
        if (subRelAmount > 0.0) subEnv = dbGain(-subRelAmount);
        subEnvelope_.gains[time_ % gainWindow] = pushStages(subEnvelope_, subEnv);
        const double subGain = subEnvelope_.gains[(time_ + gainWindow - gainDelay) % gainWindow];

        std::array<double, 2> releaseAmounts{};
        c = 0;
        while (c < channelCount_) {
            const auto idx = static_cast<std::size_t>(c);
            auto& env = channels_[idx].envelope;
            const double partialLink = hiLink * (1.0 - subShareIn[idx]);
            const double chHiLink = settings_.stereoLink >= .9999f ? 1.0 : partialLink;
            const double safeGain = hiTarget[idx] + chHiLink * (linkedHi - hiTarget[idx]);
            const double held = env.minimum.push(safeGain, time_, window);
            const bool idle = held >= 1.0 && env.releaseDb <= 0.0 && env.roundDb <= 0.0 && env.sustainDb <= 0.0;
            if (!idle) {
                const double linkedKOver = kOverDb[idx] + chHiLink * (maxKOver - kOverDb[idx]);
                const double linkedCrest = crest[idx] + chHiLink * (maxCrest - crest[idx]);
                releaseAmounts[idx] = advanceRelease(env, -20.0 * std::log10(std::max(held, 1e-30)),
                                                     linkedKOver, linkedCrest);
            }
            ++c;
        }

        double sharedRelease = releaseAmounts[0];
        if (channelCount_ == 2) sharedRelease = std::max(releaseAmounts[0], releaseAmounts[1]);
        std::array<double, 2> hiGains{1.0, 1.0};
        c = 0;
        while (c < channelCount_) {
            const auto idx = static_cast<std::size_t>(c);
            auto& env = channels_[idx].envelope;
            const double linkedRel = releaseAmounts[idx] + settings_.releaseLink * (sharedRelease - releaseAmounts[idx]);
            double envGain = 1.0;
            if (linkedRel > 0.0) envGain = dbGain(-linkedRel);
            env.gains[time_ % gainWindow] = pushStages(env, envGain);
            hiGains[idx] = env.gains[(time_ + gainWindow - gainDelay) % gainWindow];
            ++c;
        }

        // Unify sub and high gains at 100% stereo link; keep sub independent otherwise.
        double minHiGain = hiGains[0];
        double maxHiGain = hiGains[0];
        if (channelCount_ == 2) {
            minHiGain = std::min(hiGains[0], hiGains[1]);
            maxHiGain = std::max(hiGains[0], hiGains[1]);
        }
        const double clampedSub = std::clamp(subGain, minHiGain, maxHiGain);
        const double appliedSubGain = settings_.stereoLink >= .9999f ? clampedSub : subGain;

        c = 0;
        while (c < channelCount_) {
            const auto idx = static_cast<std::size_t>(c);
            auto& channel = channels_[idx];
            const double delayed = channel.audio[position_];
            const double delayedSub = channel.audioSub[position_];
            const double delayedAllowance = channel.allowances[position_];
            const double subDom = subShareIn[idx];
            const double chGain = std::min(hiGains[idx], 1.0 - subDom * (1.0 - appliedSubGain));
            minimumGain = std::min(minimumGain, std::min(chGain, appliedSubGain));
            channel.audio[position_] = input[idx];
            channel.audioSub[position_] = sub[idx];
            channel.allowances[position_] = allowance[idx];
            const double combined = delayedSub * appliedSubGain + (delayed - delayedSub) * chGain;
            const auto output = static_cast<float>(shape(combined, delayedAllowance));
            if (c == 0) left[i] = output;
            if (c == 1 && right) right[i] = output;
            ++c;
        }
        if (++position_ == static_cast<std::size_t>(latency_))
            position_ = 0;
        ++time_;
        ++i;
    }
    reductionDb_ = static_cast<float>(-20.0 * std::log10(std::max(minimumGain, 1e-12)));
}

} // namespace wall
