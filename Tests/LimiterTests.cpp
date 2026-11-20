#include "Limiter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> countAllocations{false};
std::atomic<int> allocations{0};
int checks = 0;
void require(bool condition, const std::string& description) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}
constexpr std::array modes{wall::Mode::Modern, wall::Mode::Smooth,
    wall::Mode::Aggressive, wall::Mode::Safe, wall::Mode::Classic};
float gain(float db) { return std::pow(10.0f, db / 20.0f); }

std::vector<float> render(const std::vector<float>& input, wall::Settings settings,
                          double rate = 48000, int block = 257) {
    wall::Limiter limiter;
    limiter.prepare(rate, 1);
    limiter.setSettings(settings);
    auto output = input;
    for (int offset = 0; offset < static_cast<int>(output.size()); offset += block)
        limiter.process(output.data() + offset, nullptr,
                        std::min(block, static_cast<int>(output.size()) - offset));
    return output;
}

void ceilingAndLatency() {
    for (double rate : {44100., 48000., 96000., 192000., 3072000.}) {
        wall::Limiter limiter;
        limiter.prepare(rate, 1);
        const int latency = limiter.latencySamples();
        require(latency % 16 == 0 && latency >= rate * .010 && latency < rate * .010 + 16,
                "ten millisecond maximum lookahead latency rounded up to a multiple of 16");
        std::vector<float> impulse(latency + 32, 0);
        impulse[0] = .1f;
        limiter.process(impulse.data(), nullptr, static_cast<int>(impulse.size()));
        require(std::abs(impulse[latency] - .1f) < 1e-7f, "impulse at reported latency");
        impulse[latency] = 0;
        require(std::all_of(impulse.begin(), impulse.end(), [](float v) { return v == 0; }), "no impulse outside latency");
        for (auto mode : modes) {
            wall::Settings s;
            s.mode = mode;
            s.thresholdDb = -30;
            s.ceilingDb = -9;
            s.clip = 1;
            s.release = .1f;
            std::vector<float> input(latency * 4 + 3000);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = i % 83 == 0 ? 120.f : 1.2f * std::sin(static_cast<float>(i * .37));
            auto output = render(input, s, rate);
            require(std::all_of(output.begin(), output.end(), [&](float v) {
                return std::isfinite(v) && std::abs(v) <= gain(s.ceilingDb) + 1e-6f;
            }), "ceiling at every mode and processing rate");
        }
    }
}

void partitionAndReset() {
    std::vector<float> input(48000);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = .3f * std::sin(static_cast<float>(i * .073)) + (i % 1777 == 0 ? 2.f : 0.f);
    for (auto mode : modes) {
        wall::Settings s;
        s.mode = mode;
        s.thresholdDb = -12;
        s.upwardDb = 3;
        s.clip = .6f;
        auto one = render(input, s, 48000, 1);
        auto irregular = render(input, s, 48000, 271);
        require(one == irregular, "sample partition invariance");
        wall::Limiter limiter;
        limiter.prepare(48000, 1);
        limiter.setSettings(s);
        auto actual = input;
        limiter.process(actual.data(), nullptr, static_cast<int>(actual.size()));
        limiter.reset();
        actual = input;
        limiter.process(actual.data(), nullptr, static_cast<int>(actual.size()));
        require(actual == irregular, "reset restores deterministic initial state");
    }
}

void stereo() {
    for (float link : {0.f, .25f, .5f, .75f, 1.f}) {
        wall::Settings s;
        s.ceilingDb = -6;
        s.stereoLink = link;
        wall::Limiter limiter;
        limiter.prepare(48000, 2);
        limiter.setSettings(s);
        std::vector<float> left(16000), right(16000);
        std::size_t i = 0;
        while (i < left.size()) {
            const float carrier = std::sin(static_cast<float>(2 * 3.14159265358979 * 2000 * double(i) / 48000));
            left[i] = 4.f * carrier;
            right[i] = .1f * carrier;
            ++i;
        }
        limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
        float leftPeak = 0, rightPeak = 0;
        i = 12000;
        while (i < left.size()) {
            leftPeak = std::max(leftPeak, std::abs(left[i]));
            rightPeak = std::max(rightPeak, std::abs(right[i]));
            ++i;
        }
        require(leftPeak <= gain(-6) + 1e-5f, "partial linking keeps loud channel safe");
        require(rightPeak <= .1001f, "partial linking never raises independent gain");
        if (link == 1)
            require(std::abs(leftPeak / rightPeak - 40) < .05f, "full linking preserves stereo ratio");
        if (link == 0)
            require(std::abs(rightPeak - .1f) < 2e-3f, "unlinked quiet high-band channel untouched");
    }
    {
        wall::Settings subSettings;
        subSettings.ceilingDb = -6;
        subSettings.stereoLink = 0;
        wall::Limiter subLimiter;
        subLimiter.prepare(48000, 2);
        subLimiter.setSettings(subSettings);
        std::vector<float> subL(24000), subR(24000);
        std::size_t i = 0;
        while (i < subL.size()) {
            const float bass = std::sin(static_cast<float>(2 * 3.14159265358979 * 50 * double(i) / 48000));
            subL[i] = 4.f * bass;
            subR[i] = .1f * bass;
            ++i;
        }
        subLimiter.process(subL.data(), subR.data(), static_cast<int>(subL.size()));
        float peakL = 0, peakR = 0;
        i = 16000;
        while (i < subL.size()) {
            peakL = std::max(peakL, std::abs(subL[i]));
            peakR = std::max(peakR, std::abs(subR[i]));
            ++i;
        }
        require(std::abs(peakL / peakR - 40.f) < .5f, "sub-bass stays 100% linked even at 0% stereo link");
    }
    {
        wall::Settings kickHat;
        kickHat.ceilingDb = 0;
        kickHat.stereoLink = .5f;
        wall::Limiter khLimiter;
        khLimiter.prepare(48000, 2);
        khLimiter.setSettings(kickHat);
        std::vector<float> khL(16000), khR(16000);
        std::size_t i = 0;
        while (i < khL.size()) {
            const float kick = .55f * std::sin(static_cast<float>(2 * 3.14159265358979 * 55 * double(i) / 48000));
            const float hat = 1.8f * std::sin(static_cast<float>(2 * 3.14159265358979 * 4000 * double(i) / 48000));
            khL[i] = kick + hat;
            khR[i] = kick;
            ++i;
        }
        khLimiter.process(khL.data(), khR.data(), static_cast<int>(khL.size()));
        float rightKickPeak = 0;
        i = 10000;
        while (i < khR.size()) {
            rightKickPeak = std::max(rightKickPeak, std::abs(khR[i]));
            ++i;
        }
        require(rightKickPeak > .52f, "panned high-band stab does not duck centered sub kick");
    }
    wall::Settings s;
    s.stereoLink = 0;
    std::vector<float> input(9000, .4f);
    input[100] = 3;
    auto mono = render(input, s);
    wall::Limiter limiter;
    limiter.prepare(48000, 2);
    limiter.setSettings(s);
    auto left = input, right = input;
    limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
    require(left == mono && right == mono, "dual mono matches mono");
}

void stereoReleaseLink() {
    int reportedLatency = 0;
    auto renderRight = [&](float releaseLink) {
        wall::Settings settings;
        settings.mode = wall::Mode::Smooth;
        settings.ceilingDb = -6;
        settings.stereoLink = 0;
        settings.releaseLink = releaseLink;
        wall::Limiter limiter;
        limiter.prepare(48000, 2);
        limiter.setSettings(settings);
        reportedLatency = limiter.latencySamples();
        std::vector<float> left(8000, 0), right(8000);
        std::size_t i = 0;
        while (i < right.size()) {
            right[i] = .2f * std::sin(static_cast<float>(2 * 3.14159265358979 * 2000 * double(i) / 48000));
            ++i;
        }
        i = 2000;
        while (i < 2064) {
            left[i] = 4.f * std::sin(static_cast<float>(2 * 3.14159265358979 * 3000 * double(i) / 48000));
            ++i;
        }
        limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
        return right;
    };
    const auto independent = renderRight(0);
    const auto linked = renderRight(1);
    float indPeak = 0, lnkPeak = 0;
    const auto start = static_cast<std::size_t>(2064 + reportedLatency);
    std::size_t idx = start;
    while (idx < start + 240) {
        indPeak = std::max(indPeak, std::abs(independent[idx]));
        lnkPeak = std::max(lnkPeak, std::abs(linked[idx]));
        ++idx;
    }
    require(std::abs(indPeak - .2f) < 2e-3f, "independent release leaves the quiet channel alone");
    require(lnkPeak < indPeak * .9f, "release link shares the other channel's recovery attenuation");
}

void silenceInvalidAndAllocation() {
    wall::Limiter limiter;
    limiter.prepare(48000, 2);
    wall::Settings s;
    s.upwardDb = 10;
    limiter.setSettings(s);
    std::vector<float> left(12000, 0), right(12000, 0);
    limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
    require(left == right && std::all_of(left.begin(), left.end(), [](float v) { return v == 0; }), "upward gain preserves digital silence");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    left[0] = nan;
    left[1] = inf;
    left[2] = -inf;
    left[3] = std::numeric_limits<float>::max();
    right = left;
    s.thresholdDb = nan;
    s.ceilingDb = -inf;
    s.release = inf;
    s.clip = -100;
    s.upwardDb = nan;
    s.stereoLink = inf;
    s.releaseLink = inf;
    s.lookaheadMs = inf;
    s.mode = static_cast<wall::Mode>(999);
    allocations = 0;
    countAllocations = true;
    limiter.setSettings(s);
    limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
    countAllocations = false;
    require(allocations == 0, "setSettings and process allocate no memory");
    require(std::all_of(left.begin(), left.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1; }), "nonfinite audio/settings cannot poison output");
    require(std::isfinite(limiter.gainReductionDb()) && limiter.gainReductionDb() >= 0, "finite nonnegative gain reduction meter");
    limiter.process(nullptr, nullptr, -1);
    wall::Limiter unprepared;
    float sample = nan;
    unprepared.process(&sample, nullptr, 1);
    require(std::isfinite(sample), "unprepared core sanitizes audio");
    for (double rate : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        limiter.prepare(rate, 99);
        require(limiter.latencySamples() > 0 && limiter.latencySamples() < 20000, "invalid prepare arguments handled");
    }
}

void driveUpwardModesAndRelease() {
    std::vector<float> quiet(48000, .001f);
    wall::Settings neutral;
    auto plain = render(quiet, neutral);
    auto s = neutral;
    s.thresholdDb = -12;
    auto driven = render(quiet, s);
    require(std::abs(driven.back() / plain.back() - gain(12)) < .001f, "threshold adds drive independently of ceiling");
    s = neutral;
    s.ceilingDb = -6;
    auto ceilingOnly = render(quiet, s);
    require(std::abs(ceilingOnly.back() - plain.back()) < 1e-7f, "ceiling does not apply makeup drive");
    s = neutral;
    s.upwardDb = 10;
    std::vector<float> audibleQuiet(48000), lowHiss(48000);
    std::size_t qIdx = 0;
    while (qIdx < audibleQuiet.size()) {
        const float wave = std::sin(static_cast<float>(2 * 3.14159265358979 * 1000 * double(qIdx) / 48000));
        audibleQuiet[qIdx] = .005f * wave;
        lowHiss[qIdx] = .0001f * wave;
        ++qIdx;
    }
    const auto plainTone = render(audibleQuiet, neutral);
    const auto lifted = render(audibleQuiet, s);
    float plainPeak = 0, liftedPeak = 0, hissPeak = 0;
    qIdx = 36000;
    while (qIdx < audibleQuiet.size()) {
        plainPeak = std::max(plainPeak, std::abs(plainTone[qIdx]));
        liftedPeak = std::max(liftedPeak, std::abs(lifted[qIdx]));
        ++qIdx;
    }
    require(liftedPeak > plainPeak * 2.5f, "upward lifts K-weighted quiet audible content");
    const auto dcRumble = render(quiet, s);
    require(std::abs(dcRumble.back()) <= .001001f, "K-weighted upward stage rejects DC rumble");
    const auto expandedHiss = render(lowHiss, s);
    qIdx = 36000;
    while (qIdx < expandedHiss.size()) {
        hissPeak = std::max(hissPeak, std::abs(expandedHiss[qIdx]));
        ++qIdx;
    }
    require(hissPeak < .000085f, "downward expander floor below -60 LUFS attenuates hiss");
    auto noise = render(std::vector<float>(48000, 1e-7f), s);
    require(std::abs(noise.back()) <= 1e-7f, "upward gate never lifts deep noise");

    std::vector<float> program(40000, .05f);
    for (int i = 3000; i < 6000; ++i)
        program[i] = 3.f * std::sin(static_cast<float>(i * .091));
    std::array<std::vector<float>, 5> results;
    for (std::size_t i = 0; i < modes.size(); ++i) {
        s = neutral;
        s.mode = modes[i];
        s.clip = .7f;
        results[i] = render(program, s);
    }
    for (std::size_t i = 0; i < results.size(); ++i)
        for (std::size_t j = i + 1; j < results.size(); ++j) {
            double difference = 0;
            for (std::size_t k = 0; k < program.size(); ++k)
                difference += std::abs(results[i][k] - results[j][k]);
            require(difference > .1, "every mode has measurably distinct behavior");
        }
    s = neutral;
    s.mode = wall::Mode::Classic;
    s.release = .1f;
    const auto fast = render(program, s);
    s.release = 10;
    const auto slow = render(program, s);
    require(fast[16000] > slow[16000] * 1.5f, "release multiplier changes recovery");
    s.mode = wall::Mode::Safe;
    s.clip = 0;
    auto safe = render(program, s);
    s.clip = 1;
    require(safe == render(program, s), "Safe mode never allocates intentional clipping");
}

void anticipatedGainIsSmooth() {
    wall::Limiter probe;
    probe.prepare(48000, 1);
    const int latency = probe.latencySamples();
    std::vector<float> input(3000, .1f);
    input[1000] = 10;
    wall::Settings s;
    s.mode = wall::Mode::Smooth;
    s.lookaheadMs = 5;
    const auto fiveMs = render(input, s);
    const int arrival = 1000 + latency;
    require(fiveMs[static_cast<std::size_t>(arrival - 360)] > .0999f
                && fiveMs[static_cast<std::size_t>(arrival - 120)] < .0999f,
            "5 ms lookahead aligns pre-ramp to 5 ms before delayed impulse");
    s.lookaheadMs = 10;
    const auto tenMs = render(input, s);
    require(tenMs[static_cast<std::size_t>(arrival - 360)] < .0995f,
            "raising lookahead above 5 ms extends true lookahead anticipation");
    s.lookaheadMs = 2;
    const auto twoMs = render(input, s);
    require(twoMs[static_cast<std::size_t>(arrival - 120)] > .0999f
                && twoMs[static_cast<std::size_t>(arrival - 40)] < .0999f,
            "2 ms lookahead shifts pre-ramp closer to delayed impulse");
    float largestStep = 0, largestCurvature = 0;
    int stepIdx = arrival - 460;
    while (stepIdx < arrival - 1) {
        largestStep = std::max(largestStep, std::abs(tenMs[static_cast<std::size_t>(stepIdx)] - tenMs[static_cast<std::size_t>(stepIdx - 1)]));
        largestCurvature = std::max(
            largestCurvature,
            std::abs(tenMs[static_cast<std::size_t>(stepIdx + 1)] - 2.f * tenMs[static_cast<std::size_t>(stepIdx)]
                     + tenMs[static_cast<std::size_t>(stepIdx - 1)]));
        ++stepIdx;
    }
    require(largestStep < .002f && largestCurvature < 2e-5f,
            "multi-pass cubic B-spline attack rounds window edges without slope jumps");
    s.lookaheadMs = 0;
    const auto noLookahead = render(input, s);
    require(noLookahead[static_cast<std::size_t>(arrival - 1)] > .0999f,
            "zero lookahead does not pre-attenuate the audio before a transient");

    // Verify C2 soft-knee continuity on a ramp when zero-lookahead passes signal directly to shape().
    wall::Settings kneeSettings;
    kneeSettings.mode = wall::Mode::Aggressive;
    kneeSettings.ceilingDb = 0;
    kneeSettings.clip = 1;
    kneeSettings.lookaheadMs = 0;
    // Prepend a brief HF transient so flux=1 stays saturated across the 2.5 ms knee sweep.
    std::vector<float> kneeInput(220 + static_cast<std::size_t>(latency), 0.f);
    std::size_t kIdx = 0;
    while (kIdx < 60) {
        const bool even = (kIdx % 2) == 0;
        kneeInput[kIdx] = even ? .65f : -.65f;
        ++kIdx;
    }
    constexpr float rampStart = .45f;
    constexpr float rampStep = .87f / 120.f;
    kIdx = 0;
    while (kIdx <= 120) {
        kneeInput[60 + kIdx] = rampStart + static_cast<float>(kIdx) * rampStep;
        ++kIdx;
    }
    const auto shaped = render(kneeInput, kneeSettings);
    float maxKneeCurvatureJump = 0;
    float prevD2 = 0;
    const auto checkBegin = static_cast<std::size_t>(63 + latency);
    const auto checkEnd = static_cast<std::size_t>(178 + latency);
    kIdx = checkBegin;
    while (kIdx < checkEnd) {
        const float d2 = (shaped[kIdx + 1] - 2.f * shaped[kIdx] + shaped[kIdx - 1]) / (rampStep * rampStep);
        if (kIdx > checkBegin)
            maxKneeCurvatureJump = std::max(maxKneeCurvatureJump, std::abs(d2 - prevD2));
        prevD2 = d2;
        ++kIdx;
    }
    require(shaped[checkEnd] < 1.f && shaped[checkEnd] > .99f && maxKneeCurvatureJump < .35f,
            "C2 soft-knee waveshaper has no second-derivative discontinuity");
    // Also check pure sub-bass + snare transient clip allowance and K-weighted release recovery.
    std::vector<float> subAndSnare(48000);
    kIdx = 0;
    while (kIdx < subAndSnare.size()) {
        const float sub = 1.6f * std::sin(static_cast<float>(2 * 3.14159265358979 * 50 * double(kIdx) / 48000));
        float snare = 0.f;
        if (kIdx >= 24000 && kIdx < 24240)
            snare = 1.5f * std::sin(static_cast<float>(2 * 3.14159265358979 * 1800 * double(kIdx) / 48000));
        subAndSnare[kIdx] = sub + snare;
        ++kIdx;
    }
    wall::Settings clipOff = kneeSettings;
    clipOff.lookaheadMs = 5;
    clipOff.clip = 0;
    wall::Settings clipOn = clipOff;
    clipOn.clip = 1;
    const auto limitedSnare = render(subAndSnare, clipOff);
    const auto clippedSnare = render(subAndSnare, clipOn);
    double snareDiff = 0;
    kIdx = static_cast<std::size_t>(24000 + latency);
    while (kIdx < static_cast<std::size_t>(24240 + latency)) {
        snareDiff += std::abs(clippedSnare[kIdx] - limitedSnare[kIdx]);
        ++kIdx;
    }
    require(snareDiff > .5 && std::isfinite(shaped.back()),
            "HF transient flux preserves snare clip allowance under heavy sustained sub-bass");

    // Verify K-weighted adaptive dual release does not pump vocals/hats slowly after a transient over sub-bass.
    std::vector<float> subBed(48000);
    kIdx = 0;
    while (kIdx < subBed.size()) {
        const float sub = 1.25f * std::sin(static_cast<float>(2 * 3.14159265358979 * 35 * double(kIdx) / 48000));
        const float probeHat = .05f * std::sin(static_cast<float>(2 * 3.14159265358979 * 6000 * double(kIdx) / 48000));
        const bool isSpike = kIdx == 20000;
        const float spike = isSpike ? 4.f : 0.f;
        subBed[kIdx] = sub + probeHat + spike;
        ++kIdx;
    }
    wall::Settings relSettings;
    relSettings.mode = wall::Mode::Modern;
    relSettings.ceilingDb = 0;
    const auto relOut = render(subBed, relSettings);
    double steadyEnergy = 0, recoveredEnergy = 0;
    kIdx = static_cast<std::size_t>(14400 + latency);
    while (kIdx < static_cast<std::size_t>(19200 + latency)) {
        steadyEnergy += double(relOut[kIdx]) * relOut[kIdx];
        ++kIdx;
    }
    kIdx = static_cast<std::size_t>(20000 + latency + 4800);
    while (kIdx < static_cast<std::size_t>(20000 + latency + 9600)) {
        recoveredEnergy += double(relOut[kIdx]) * relOut[kIdx];
        ++kIdx;
    }
    require(std::sqrt(recoveredEnergy / 4800.0) > .94 * std::sqrt(steadyEnergy / 4800.0),
            "K-weighted RMS + crest release recovers rapidly after transient over sub-bass");
}

void sustainedBassStaysClean() {
    // 6 dB of limiting on a steady 50 Hz tone must not modulate the waveform.
    std::vector<float> input(48000);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 1.78f * std::sin(static_cast<float>(2 * 3.14159265358979 * 50 * double(i) / 48000));
    for (const auto mode : {wall::Mode::Modern, wall::Mode::Smooth, wall::Mode::Safe, wall::Mode::Classic}) {
        wall::Settings s;
        s.mode = mode;
        const auto output = render(input, s);
        double sine = 0, cosine = 0, energy = 0;
        for (std::size_t i = 24000; i < 48000; ++i) {
            const auto phase = 2 * 3.14159265358979 * 50 * double(i) / 48000;
            sine += output[i] * std::sin(phase);
            cosine += output[i] * std::cos(phase);
            energy += double(output[i]) * output[i];
        }
        const auto fundamental = 2 * (sine * sine + cosine * cosine) / 24000;
        require((energy - fundamental) / energy < 1e-8, "sustained bass is limited without audible distortion");
    }
}

void lowCeilingAndAutomation() {
    // A UI ceiling of -30 dB can carry an additional reconstruction margin.
    for (const auto mode : modes) {
        wall::Settings s;
        s.mode = mode;
        s.ceilingDb = -30.5f;
        s.clip = .8f;
        const auto output = render(std::vector<float>(12000, 1.f), s);
        require(std::abs(output.back()) <= .02985383f, "ceiling supports low UI settings plus true peak margin");
    }
    wall::Limiter limiter;
    limiter.prepare(192000, 2);
    std::array<float, 347> left{}, right{};
    std::uint32_t random = 8191;
    for (int block = 0; block < 2000; ++block) {
        wall::Settings s;
        s.mode = modes[static_cast<std::size_t>(block) % modes.size()];
        s.thresholdDb = -static_cast<float>(block % 73);
        s.ceilingDb = -static_cast<float>(block % 37);
        s.release = block % 2 ? .1f : 10;
        s.upwardDb = static_cast<float>(block % 13);
        s.stereoLink = static_cast<float>(block % 5) * .25f;
        s.releaseLink = static_cast<float>(block % 5) * .25f;
        s.lookaheadMs = static_cast<float>(block % 6);
        s.clip = static_cast<float>(block % 11) * .1f;
        for (std::size_t i = 0; i < left.size(); ++i) {
            random = random * 1664525u + 1013904223u;
            left[i] = static_cast<float>(static_cast<std::int32_t>(random)) * 1e-8f;
            right[i] = i % 61 == 0 ? -100.f : left[i] * .01f;
        }
        allocations = 0;
        countAllocations = true;
        limiter.setSettings(s);
        limiter.process(left.data(), right.data(), static_cast<int>(left.size()));
        countAllocations = false;
        require(allocations == 0, "automated stereo callback remains allocation free");
        const auto withinCeiling = [&](float v) { return std::isfinite(v) && std::abs(v) <= gain(s.ceilingDb) + 1e-6f; };
        require(std::all_of(left.begin(), left.end(), withinCeiling)
                && std::all_of(right.begin(), right.end(), withinCeiling), "automation and deque wraparound preserve ceiling");
        require(std::isfinite(limiter.gainReductionDb()) && limiter.gainReductionDb() >= 0, "automated gain reduction remains positive");
    }
}

void thresholdAutomationIsSmooth() {
    std::array<float, 2> atFifteenMilliseconds{};
    for (int rate : {48000, 96000}) {
        wall::Limiter limiter;
        limiter.prepare(rate, 1);
        wall::Settings s;
        s.ceilingDb = 0;
        limiter.setSettings(s);
        std::vector<float> steady(static_cast<std::size_t>(rate / 10), .01f);
        limiter.process(steady.data(), nullptr, static_cast<int>(steady.size()));
        s.thresholdDb = -20;
        limiter.setSettings(s);
        std::vector<float> transition(static_cast<std::size_t>(rate / 20), .01f);
        limiter.process(transition.data(), nullptr, static_cast<int>(transition.size()));
        float biggest = 0;
        for (std::size_t i = 1; i < transition.size(); ++i)
            biggest = std::max(biggest, std::abs(transition[i] - transition[i - 1]));
        require(biggest < .001f, "automated threshold avoids an audible drive step");
        require(transition.back() > .099f, "smoothed threshold reaches requested drive");
        require(std::abs(transition[static_cast<std::size_t>(limiter.latencySamples() - 1)] - .01f) < 1e-7f,
                "threshold automation follows reported audio delay");
        const auto index = static_cast<std::size_t>(limiter.latencySamples() + rate * 15 / 1000);
        atFifteenMilliseconds[rate == 48000 ? 0 : 1] = transition[index];
    }
    require(std::abs(atFifteenMilliseconds[0] - atFifteenMilliseconds[1]) < .0001f,
            "drive automation smoothing follows time across processing rates");
}

void modeChangesKeepCurrentRelease() {
    // Re-entering Smooth must not resurrect a release from its previous visit.
    for (const auto other : modes) {
        if (other == wall::Mode::Smooth) continue;
        wall::Limiter limiter;
        limiter.prepare(48000, 1);
        wall::Settings s;
        s.mode = wall::Mode::Smooth;
        limiter.setSettings(s);
        auto audio = std::vector<float>(48000, 10);
        limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
        s.mode = other;
        limiter.setSettings(s);
        // A second of sustained limiting releases slowly, so allow two seconds.
        audio.assign(96000, .01f);
        limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
        const auto recovered = audio.back();
        require(recovered > .009f, "alternate mode recovers from previous limiting");
        s.mode = wall::Mode::Smooth;
        limiter.setSettings(s);
        audio.assign(4800, .01f);
        limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
        require(std::all_of(audio.begin(), audio.end(), [&](float value) { return value >= recovered - 1e-6f; }),
                "returning to Smooth cannot resurrect stale attenuation");
    }

    // A mode change during recovery must carry gain forward, not reset to unity.
    for (const auto previous : modes)
        for (const auto next : modes) {
            wall::Limiter limiter;
            limiter.prepare(48000, 1);
            wall::Settings s;
            s.mode = previous;
            limiter.setSettings(s);
            auto audio = std::vector<float>(48000, 10);
            limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
            audio.assign(1200, .01f);
            limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
            const auto before = audio.back();
            s.mode = next;
            limiter.setSettings(s);
            audio.assign(64, .01f);
            limiter.process(audio.data(), nullptr, static_cast<int>(audio.size()));
            // Each mode aligns its own attack, so the switch may step the gain by under 0.2 dB.
            require(audio.front() >= before * .977f && audio.back() < before * 1.2f,
                    "mode change carries the active release without a gain reset");
        }
}
} // namespace

void* operator new(std::size_t n) {
    if (countAllocations) ++allocations;
    if (auto* p = std::malloc(std::max<std::size_t>(n, 1))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    ceilingAndLatency();
    partitionAndReset();
    stereo();
    stereoReleaseLink();
    silenceInvalidAndAllocation();
    driveUpwardModesAndRelease();
    anticipatedGainIsSmooth();
    sustainedBassStaysClean();
    lowCeilingAndAutomation();
    thresholdAutomationIsSmooth();
    modeChangesKeepCurrentRelease();
    std::cout << "PASS: " << checks << " limiter behavior checks\n";
}
