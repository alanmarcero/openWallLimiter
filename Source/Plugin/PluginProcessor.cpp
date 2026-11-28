#include "PluginProcessor.h"
#include "LoudnessTarget.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr std::array<const char *, 24> ids{
    "threshold",      "ceiling",     "mode",       "release",       "clip",      "upward", "stereo_link",
    "oversampling",   "true_peak",   "dither",     "gain_match",    "delta",     "bypass", "highpass_hz",
    "highpass_slope", "highpass_on", "lowpass_hz", "lowpass_slope", "lowpass_on", "lookahead_ms",
    "release_link",   "input_gain",  "stereo_width", "tone_tilt"};
float db(float x) {
    return juce::Decibels::gainToDecibels(x, -100.f);
}
float finite(float x) {
    return std::isfinite(x) ? std::clamp(x, -64.f, 64.f) : 0.f;
}
// Presets never change these, so a preset load keeps the user's monitoring and delivery setup.
constexpr std::array<const char *, 6> sessionIds{"dither",   "gain_match", "delta",
                                                 "bypass",   "lufs_mode",  "lufs_target"};
const juce::Identifier presetNameId{"presetName"}, presetEditedId{"presetEdited"};
// Swaps one state property with a stored value, so undo and redo both swap it back.
// It acts on the current state tree, because replaceState() swaps the tree object out.
struct PropertySwap final : juce::UndoableAction {
    PropertySwap(juce::AudioProcessorValueTreeState &stateIn, const juce::Identifier &idIn, juce::var valueIn)
        : state(stateIn), id(idIn), value(std::move(valueIn)) {}
    bool perform() override {
        const auto previous = state.state.getProperty(id);
        state.state.setProperty(id, value, nullptr);
        if (value.isVoid())
            state.state.removeProperty(id, nullptr);
        value = previous;
        return true;
    }
    bool undo() override { return perform(); }
    int getSizeInUnits() override { return int(sizeof(*this)); }
    juce::AudioProcessorValueTreeState &state;
    juce::Identifier id;
    juce::var value;
};
// Keeps the largest value until the editor swaps it out.
void raise(std::atomic<float> &peak, float value) noexcept {
    float current = peak.load(std::memory_order_relaxed);
    while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}
} // namespace
WallProcessor::WallProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "OpenWallLimiter", wall::parameterLayout()),
      parameterUndo(*this, undoManagers[0]) {
    for (size_t i = 0; i < ids.size(); ++i)
        values[i] = parameters.getRawParameterValue(ids[i]);
    std::ranges::for_each(getParameters(), [this](juce::AudioProcessorParameter *parameter) {
        const auto &id = dynamic_cast<juce::AudioProcessorParameterWithID &>(*parameter).paramID;
        const auto matches = [&](const char *session) { return id == session; };
        sessionParameter.push_back(std::ranges::any_of(sessionIds, matches));
        paramLockIndex.emplace(id, paramLocks.size());
        paramLocks.emplace_back(false);
    });
    // The first recorded edit after a preset load marks the label, in the same undo step as the edit.
    parameterUndo.onEdit = [this](int index) {
        if (sessionParameter[size_t(index)] || bool(parameters.state[presetEditedId]))
            return;
        undoManagers[size_t(abCompare.active())].perform(new PropertySwap(parameters, presetEditedId, true));
    };
    parameters.state.setProperty(presetNameId, factoryPresets().front().name, nullptr);
    abCompare.seed(captureParameters());
}
bool WallProcessor::isBusesLayoutSupported(const BusesLayout &l) const {
    return (l.getMainInputChannelSet() == juce::AudioChannelSet::mono() ||
            l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()) &&
           l.getMainInputChannelSet() == l.getMainOutputChannelSet();
}
void WallProcessor::SincPad::prepare(int maxSamples, float delaySamples) {
    std::size_t ringSize = 64;
    const auto required = static_cast<std::size_t>(std::max(maxSamples + tapsCount + 8, 64));
    while (ringSize < required) ringSize <<= 1;
    mask = ringSize - 1;
    std::ranges::for_each(rings, [ringSize](std::vector<float> &r) { r.assign(ringSize, 0.0f); });
    positions = {0, 0};

    const float clampedDelay = std::max(float(sincPadHalfSpan), delaySamples);
    intDelay = int(std::floor(clampedDelay));
    const float frac = clampedDelay - float(intDelay);
    baseLag = intDelay - sincPadHalfSpan;
    exactInteger = frac < 1e-6f;
    taps.fill(0.0);
    if (exactInteger) {
        taps[size_t(sincPadHalfSpan)] = 1.0;
        return;
    }

    double sum = 0.0;
    int k = 0;
    while (k < tapsCount) {
        const double t = double(k - sincPadHalfSpan) - double(frac);
        const double u = t / double(sincPadHalfSpan);
        double w = 0.0;
        if (std::abs(u) < 1.0)
            w = .42 + .50 * std::cos(std::numbers::pi * u) + .08 * std::cos(2.0 * std::numbers::pi * u);
        double sinc = 1.0;
        if (std::abs(t) >= 1e-12)
            sinc = std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        taps[size_t(k)] = sinc * w;
        sum += taps[size_t(k)];
        ++k;
    }
    std::ranges::for_each(taps, [sum](double &v) { v /= sum; });
}

void WallProcessor::SincPad::reset() noexcept {
    std::ranges::for_each(rings, [](std::vector<float> &r) { std::fill(r.begin(), r.end(), 0.0f); });
    positions = {0, 0};
}

float WallProcessor::SincPad::process(int channel, float sample) noexcept {
    const auto ch = static_cast<std::size_t>(channel);
    auto &ring = rings[ch];
    const auto pos = positions[ch];
    ring[pos] = sample;
    positions[ch] = (pos + 1) & mask;
    if (exactInteger)
        return ring[(pos - static_cast<std::size_t>(intDelay)) & mask];

    double acc = 0.0;
    int k = 0;
    while (k < tapsCount) {
        acc += taps[static_cast<std::size_t>(k)] *
               double(ring[(pos - static_cast<std::size_t>(baseLag + k)) & mask]);
        ++k;
    }
    return float(acc);
}

void WallProcessor::prepareToPlay(double sr, int block) {
    artworkClock.reset();
    ready = false;
    if (std::isfinite(sr)) sr = std::clamp(sr, 8000., 192000.);
    if (!std::isfinite(sr)) sr = 48000.;
    preparedRate = sr;
    channels = std::clamp(getTotalNumInputChannels(), 1, 2);
    chunkSize = std::clamp(block, 32, 4096);
    input.setSize(channels, chunkSize);
    dry.setSize(channels, chunkSize);
    metering.setSize(channels, chunkSize);

    filters[0].setLinearPhaseSubCut(true);
    filters[0].setCutoff(value(13));
    filters[0].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(14))));
    filters[0].setBypassed(value(15) < .5f);
    filters[1].setCutoff(value(16));
    filters[1].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(17))));
    filters[1].setBypassed(value(18) < .5f);
    std::ranges::for_each(filters, [sr](auto &f) { f.prepare(sr); });

    // Host latency stays fixed across lookahead, oversampling, and true-peak changes.
    int maxBasePathLatency = 0;
    size_t idx = 0;
    while (idx < paths.size()) {
        auto &p = paths[idx];
        p.oversampler = std::make_unique<Oversampler>(size_t(channels), idx,
                                                      Oversampler::filterHalfBandFIREquiripple, true, true);
        p.oversampler->initProcessing(size_t(chunkSize));
        p.limiter.prepare(sr * double(1 << idx), channels);
        p.nativeLatency =
            float(p.limiter.latencySamples()) / float(1 << idx) + p.oversampler->getLatencyInSamples();
        maxBasePathLatency = std::max(maxBasePathLatency, int(std::ceil(p.nativeLatency)));
        p.buffer.setSize(channels, chunkSize);
        ++idx;
    }
    pathLatency = maxBasePathLatency + sincPadHalfSpan;
    std::ranges::for_each(paths, [this](Path &p) {
        p.pad.prepare(pathLatency + 4, float(pathLatency) - p.nativeLatency);
    });
    latency = filters[0].latencySamples() + pathLatency + wall::TruePeakGuard::delaySamples;
    peakGuard.prepare(sr);
    const juce::dsp::ProcessSpec spec{sr, juce::uint32(chunkSize), juce::uint32(channels)};
    dryDelay.setMaximumDelayInSamples(latency + 4);
    dryDelay.prepare(spec);
    dryDelay.setDelay(float(latency));
    std::ranges::for_each(ditherError, [](auto &err) { err.fill(0.0f); });
    meter.prepare(sr, channels);
    peakMeter = std::make_unique<Oversampler>(size_t(channels), 2, Oversampler::filterHalfBandFIREquiripple,
                                              true, true);
    peakMeter->initProcessing(size_t(chunkSize));
    bypassMix.reset(sr, .01);
    matchGain.reset(sr, .02);
    deltaMix.reset(sr, .01);
    bypassMix.setCurrentAndTargetValue(value(12));
    matchGain.setCurrentAndTargetValue(value(10) > .5f ? juce::Decibels::decibelsToGain(value(0)) : 1.f);
    deltaMix.setCurrentAndTargetValue(value(11));
    currentPath = std::clamp(int(value(7)), 0, 4);
    if (value(8) > .5f)
        currentPath = std::max(2, currentPath);
    nextPath = -1;
    warmup = fade = 0;
    fadeLength = std::max(1, int(sr * .02));
    setLatencySamples(latency);
    ready = true;
    resetMeters();
}
void WallProcessor::reset() {
    for (auto &p : paths) {
        p.limiter.reset();
        if (p.oversampler)
            p.oversampler->reset();
        p.pad.reset();
    }
    for (auto &f : filters)
        f.reset();
    tiltState = {0.0f, 0.0f};
    std::ranges::for_each(ditherError, [](auto &err) { err.fill(0.0f); });
    dryDelay.reset();
    peakGuard.reset();
    if (peakMeter)
        peakMeter->reset();
    meter.reset();
    nextPath = -1;
    warmup = fade = 0;
    resetMeters();
}
void WallProcessor::releaseResources() {
    ready = false;
}
void WallProcessor::processBlock(juce::AudioBuffer<float> &b, juce::MidiBuffer &) {
    process(b, false);
}
void WallProcessor::processBlockBypassed(juce::AudioBuffer<float> &b, juce::MidiBuffer &) {
    process(b, true);
}
void WallProcessor::process(juce::AudioBuffer<float> &b, bool hostBypass) {
    juce::ScopedNoDenormals noDenormals;
    auto* playHead = getPlayHead();
    artworkClock.update(playHead ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>{},
                        b.getNumSamples(), preparedRate);
    if (!ready || b.getNumChannels() < channels) {
        b.clear();
        return;
    }
    for (int ch = channels; ch < b.getNumChannels(); ++ch)
        b.clear(ch, 0, b.getNumSamples());
    if (meterReset.exchange(false)) {
        meter.reset();
        truePeakDb.store(-100);
    }
    if (truePeakReset.exchange(false))
        truePeakDb.store(-100);
    for (int offset = 0; offset < b.getNumSamples(); offset += chunkSize)
        processChunk(b, offset, std::min(chunkSize, b.getNumSamples() - offset), hostBypass);
}
void WallProcessor::runPath(int index, const wall::Settings &settings, int n) {
    auto &p = paths[size_t(index)];
    p.limiter.setSettings(settings);
    for (int c = 0; c < channels; ++c)
        p.buffer.copyFrom(c, 0, input, c, 0, n);
    auto block = juce::dsp::AudioBlock<float>(p.buffer).getSubBlock(0, size_t(n));
    const auto up = p.oversampler->processSamplesUp(block);
    p.limiter.process(up.getChannelPointer(0), channels > 1 ? up.getChannelPointer(1) : nullptr,
                      int(up.getNumSamples()));
    p.oversampler->processSamplesDown(block);
    int c = 0;
    while (c < channels) {
        int i = 0;
        while (i < n) {
            p.buffer.setSample(c, i, p.pad.process(c, p.buffer.getSample(c, i)));
            ++i;
        }
        ++c;
    }
}
float WallProcessor::randomUnit() noexcept {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return float(randomState >> 8) * (1.f / 16777216.f);
}
void WallProcessor::processChunk(juce::AudioBuffer<float> &buffer, int offset, int n, bool hostBypass) {
    float inPeak = 0, outPeak = 0;
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i) {
            const float x = finite(buffer.getSample(c, offset + i));
            inPeak = std::max(inPeak, std::abs(x));
            input.setSample(c, i, x);
            dryDelay.pushSample(c, x);
            dry.setSample(c, i, dryDelay.popSample(c));
        }
    filters[0].setCutoff(value(13));
    filters[0].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(14))));
    filters[0].setBypassed(value(15) < .5f);
    filters[1].setCutoff(value(16));
    filters[1].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(17))));
    filters[1].setBypassed(value(18) < .5f);
    for (auto &f : filters)
        f.processBlock(input.getArrayOfWritePointers(), channels, n);
    const float inputGain = juce::Decibels::decibelsToGain(value(21));
    const float widthScale = value(22) * .01f;
    const float tiltDb = value(23);
    const float tiltAlpha = float(1.0 - std::exp(-2.0 * 3.141592653589793 * 1200.0 / preparedRate));
    const float tiltHighGain = juce::Decibels::decibelsToGain(tiltDb * .5f);
    const float tiltLowGain = juce::Decibels::decibelsToGain(-tiltDb * .5f);
    if (std::abs(inputGain - 1.f) > 1e-5f || std::abs(widthScale - 1.f) > 1e-5f || std::abs(tiltDb) > 1e-3f) {
        for (int i = 0; i < n; ++i) {
            float l = input.getSample(0, i) * inputGain;
            float r = channels > 1 ? input.getSample(1, i) * inputGain : l;
            if (std::abs(tiltDb) > 1e-3f) {
                tiltState[0] += tiltAlpha * (l - tiltState[0]);
                l = tiltState[0] * tiltLowGain + (l - tiltState[0]) * tiltHighGain;
                if (channels > 1) {
                    tiltState[1] += tiltAlpha * (r - tiltState[1]);
                    r = tiltState[1] * tiltLowGain + (r - tiltState[1]) * tiltHighGain;
                } else {
                    r = l;
                }
            }
            if (channels > 1 && std::abs(widthScale - 1.f) > 1e-5f) {
                const float mid = .5f * (l + r);
                const float side = .5f * (l - r) * widthScale;
                l = mid + side;
                r = mid - side;
            }
            input.setSample(0, i, l);
            if (channels > 1)
                input.setSample(1, i, r);
        }
    }
    const bool tp = value(8) > .5f;
    wall::Settings settings;
    settings.thresholdDb = value(0);
    settings.ceilingDb = value(1);
    settings.mode = static_cast<wall::Mode>(int(value(2)));
    settings.release = value(3);
    settings.clip = value(4) * .01f;
    settings.upwardDb = value(5);
    settings.stereoLink = value(6) * .01f;
    settings.lookaheadMs = value(19);
    settings.releaseLink = value(20) * .01f;
    int requested = std::clamp(int(value(7)), 0, 4);
    if (tp)
        requested = std::max(2, requested);
    if (nextPath < 0 && requested != currentPath) {
        nextPath = requested;
        auto &p = paths[size_t(nextPath)];
        p.limiter.reset();
        p.oversampler->reset();
        p.pad.reset();
        warmup = pathLatency + fadeLength;
        fade = 0;
    }
    runPath(currentPath, settings, n);
    if (nextPath >= 0)
        runPath(nextPath, settings, n);
    effectiveFactor.store(1 << (nextPath >= 0 ? nextPath : currentPath), std::memory_order_relaxed);
    const bool bypassOn = hostBypass || value(12) > .5f;
    const bool matchOn = value(10) > .5f;
    const bool deltaOn = value(11) > .5f;
    const float gainMatchDb = juce::Decibels::decibelsToGain(value(0));
    bypassMix.setTargetValue(bypassOn ? 1.f : 0.f);
    matchGain.setTargetValue(matchOn ? gainMatchDb : 1.f);
    deltaMix.setTargetValue(deltaOn ? 1.f : 0.f);
    const float ceiling = juce::Decibels::decibelsToGain(value(1));
    const int dither = int(value(9));
    const float scale = dither == 1 ? 8388608.f : 32768.f;
    const float maxCode = std::floor(ceiling * scale);
    // Lipshitz, Wannamaker & Vanderkooy 5-tap F-weighted psychoacoustic noise-shaping coefficients.
    constexpr std::array<float, 5> lipshitz{2.033f, -2.165f, 1.959f, -1.590f, 0.6149f};
    constexpr std::array<float, 5> lowRateShape{0.5f, 0.0f, 0.0f, 0.0f, 0.0f};
    const auto &shaper = preparedRate >= 32000.0 ? lipshitz : lowRateShape;

    int i = 0;
    while (i < n) {
        float cross = 0;
        if (nextPath >= 0 && warmup > 0) --warmup;
        if (nextPath >= 0 && warmup == 0) cross = std::min(1.f, float(++fade) / float(fadeLength));
        const float bypass = bypassMix.getNextValue(), match = matchGain.getNextValue(),
                    delta = deltaMix.getNextValue();
        std::array<float, 2> protectedSample{};
        int c = 0;
        while (c < channels) {
            float wet = paths[size_t(currentPath)].buffer.getSample(c, i);
            if (nextPath >= 0)
                wet += cross * (paths[size_t(nextPath)].buffer.getSample(c, i) - wet);
            protectedSample[size_t(c)] = finite(wet);
            ++c;
        }
        peakGuard.process(protectedSample[0], protectedSample[1], ceiling, tp);
        c = 0;
        while (c < channels) {
            float wet = std::clamp(protectedSample[size_t(c)], -ceiling, ceiling);
            if (dither > 0) {
                auto &err = ditherError[size_t(c)];
                const float feedback = shaper[0] * err[0] + shaper[1] * err[1] + shaper[2] * err[2]
                                     + shaper[3] * err[3] + shaper[4] * err[4];
                const float target = wet * scale - feedback;
                const float tpdf = randomUnit() - randomUnit();
                const float code = std::clamp(std::round(target + tpdf), -maxCode, maxCode);
                const float nextErr = std::clamp(code - target, -1.5f, 1.5f);
                err[4] = err[3];
                err[3] = err[2];
                err[2] = err[1];
                err[1] = err[0];
                err[0] = nextErr;
                wet = code / scale;
            }
            // Meters read the limiter output before Gain match, Delta and Bypass change what you hear.
            metering.setSample(c, i, wet);
            outPeak = std::max(outPeak, std::abs(wet));
            wet *= match;
            const float original = dry.getSample(c, i);
            const float monitor = wet - delta * original;
            const float output = monitor + bypass * (original - monitor);
            buffer.setSample(c, offset + i, output);
            ++c;
        }
        meter.process(metering.getSample(0, i), channels > 1 ? metering.getSample(1, i) : 0.f);
        ++i;
    }
    float reduction = paths[size_t(currentPath)].limiter.gainReductionDb();
    if (nextPath >= 0)
        reduction = std::max(reduction, paths[size_t(nextPath)].limiter.gainReductionDb());
    if (nextPath >= 0 && warmup == 0 && fade >= fadeLength) {
        currentPath = nextPath;
        nextPath = -1;
    }
    auto meteringBlock = juce::dsp::AudioBlock<float>(metering).getSubBlock(0, size_t(n));
    const auto up = peakMeter->processSamplesUp(meteringBlock);
    float truePeak = outPeak;
    size_t chIdx = 0;
    while (chIdx < up.getNumChannels()) {
        size_t smpIdx = 0;
        while (smpIdx < up.getNumSamples()) {
            truePeak = std::max(truePeak, std::abs(up.getSample(int(chIdx), int(smpIdx))));
            ++smpIdx;
        }
        ++chIdx;
    }
    raise(inputDb, db(inPeak));
    raise(outputDb, db(outPeak));
    raise(reductionDb, reduction);
    truePeakDb.store(std::max(db(truePeak), truePeakDb.load()));
    shortLufs.store(meter.shortTermLufs());
    integratedLufs.store(meter.integratedLufs());
}
juce::ValueTree WallProcessor::sanitizeTree(juce::ValueTree incoming) {
    if (!incoming.getChildWithProperty("id", "lookahead_ms").isValid()) {
        juce::ValueTree child("PARAM");
        child.setProperty("id", "lookahead_ms", nullptr);
        child.setProperty("value", 5.0f, nullptr);
        incoming.addChild(child, -1, nullptr);
    }
    if (!incoming.getChildWithProperty("id", "release_link").isValid()) {
        juce::ValueTree child("PARAM");
        child.setProperty("id", "release_link", nullptr);
        child.setProperty("value", 0.0f, nullptr);
        incoming.addChild(child, -1, nullptr);
    }
    for (auto *parameter : getParameters())
        if (auto *p = dynamic_cast<juce::RangedAudioParameter *>(parameter)) {
            auto child = incoming.getChildWithProperty("id", p->paramID);
            float v = child.isValid() ? float(child["value"]) : p->convertFrom0to1(p->getDefaultValue());
            if (!std::isfinite(v))
                v = p->convertFrom0to1(p->getDefaultValue());
            v = p->convertFrom0to1(std::clamp(p->convertTo0to1(v), 0.f, 1.f));
            if (!child.isValid()) {
                child = juce::ValueTree("PARAM");
                child.setProperty("id", p->paramID, nullptr);
                incoming.addChild(child, -1, nullptr);
            }
            child.setProperty("value", v, nullptr);
        }
    return incoming;
}
juce::MemoryBlock WallProcessor::captureParameters() {
    auto state = parameters.copyState();
    for (const char *key : {"editorWidth", "editorHeight", "program", "abSlot"})
        state.removeProperty(key, nullptr);
    juce::MemoryBlock block;
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, block);
    return block;
}
void WallProcessor::restoreTree(juce::ValueTree tree, bool undoable, const juce::String &undoName) {
    if (!tree.isValid() || !tree.hasType(parameters.state.getType()))
        return;
    tree = sanitizeTree(tree);
    if (undoable)
        parameterUndo.performTree(parameters, tree, undoName);
    else {
        parameterUndo.suspend();
        parameters.replaceState(tree);
        parameterUndo.rescan();
        parameterUndo.resume();
    }
}
void WallProcessor::restoreBlock(const juce::MemoryBlock &block, bool undoable, const juce::String &undoName) {
    if (auto xml = getXmlFromBinary(block.getData(), int(block.getSize())))
        restoreTree(juce::ValueTree::fromXml(*xml), undoable, undoName);
}
bool WallProcessor::loadParameterState(const juce::MemoryBlock &block, const juce::String &name) {
    auto xml = getXmlFromBinary(block.getData(), int(block.getSize()));
    if (xml == nullptr || !xml->hasTagName(parameters.state.getType()))
        return false;
    auto tree = sanitizeTree(juce::ValueTree::fromXml(*xml));
    std::ranges::for_each(sessionIds, [&](const char *id) {
        const float current = parameters.getRawParameterValue(id)->load();
        tree.getChildWithProperty("id", id).setProperty("value", current, nullptr);
    });
    for (auto *parameter : getParameters()) {
        if (auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(parameter)) {
            if (isParamLocked(ranged->paramID)) {
                const float current = parameters.getRawParameterValue(ranged->paramID)->load();
                tree.getChildWithProperty("id", ranged->paramID).setProperty("value", current, nullptr);
            }
        }
    }
    tree.setProperty(presetNameId, name, nullptr).setProperty(presetEditedId, false, nullptr);
    restoreTree(tree, true, "Preset");
    return true;
}
juce::String WallProcessor::presetLabel() const {
    // Sessions saved before the label existed show the host program name.
    const juce::var programName{factoryPresets()[size_t(currentProgram)].name};
    auto label = parameters.state.getProperty(presetNameId, programName).toString();
    if (parameters.state[presetEditedId])
        label += " *";
    return label;
}
void WallProcessor::markPresetSaved(const juce::String &name) {
    parameters.state.setProperty(presetNameId, name, nullptr).setProperty(presetEditedId, false, nullptr);
}
void WallProcessor::setStoredEditorSize(int width, int height) {
    editorWidth = std::clamp(width, minEditorWidth, maxEditorWidth);
    editorHeight = std::clamp(height, minEditorHeight, maxEditorHeight);
}
void WallProcessor::toggleAB() {
    restoreBlock(abCompare.toggle(captureParameters()), false, {});
    parameterUndo.setUndoManager(undoManagers[size_t(abCompare.active())]);
}
void WallProcessor::copyAB(int from, int to) {
    if (auto next = abCompare.copy(from, to, captureParameters()))
        restoreBlock(*next, false, {});
    // The copy replaces the destination setup, so its old undo steps no longer apply.
    undoManagers[size_t(to & 1)].clearUndoHistory();
}
void WallProcessor::matchLoudnessTarget() {
    const int mode = int(std::lround(parameters.getRawParameterValue("lufs_mode")->load()));
    const float custom = parameters.getRawParameterValue("lufs_target")->load();
    const auto target = wall::targetLufs(mode, custom);
    if (!target)
        return;
    const float integrated = integratedLufs.load();
    const float shortTerm = shortLufs.load();
    const float measured = integrated > -99.f ? integrated : shortTerm;
    auto *gain = parameters.getParameter("input_gain");
    if (gain == nullptr || measured <= -99.f)
        return;
    const float next = wall::matchedInputGain(parameters.getRawParameterValue("input_gain")->load(), measured, *target);
    suite::ParameterUndo::Group group(parameterUndo, "Match loudness");
    gain->beginChangeGesture();
    gain->setValueNotifyingHost(gain->convertTo0to1(next));
    gain->endChangeGesture();
    resetMeters();
}
void WallProcessor::getStateInformation(juce::MemoryBlock &dest) {
    abCompare.setSlot(abCompare.active(), captureParameters());
    auto state = parameters.copyState();
    for (const char *key : {"editorWidth", "editorHeight", "program", "abSlot"})
        state.removeProperty(key, nullptr);
    if (auto xml = state.createXml()) {
        xml->setAttribute("editorWidth", editorWidth);
        xml->setAttribute("editorHeight", editorHeight);
        xml->setAttribute("program", currentProgram);
        xml->setAttribute("abSlot", abCompare.active());
        for (int i = 0; i < 2; ++i) {
            auto *child = xml->createNewChildElement("AB");
            child->setAttribute("index", i);
            child->setAttribute("data", abCompare.slot(i).toBase64Encoding());
        }
        if (anyParamLocked()) {
            auto *locks = xml->createNewChildElement("LOCKS");
            for (const auto &[id, idx] : paramLockIndex)
                if (paramLocks[idx].load(std::memory_order_acquire))
                    locks->createNewChildElement("LOCK")->setAttribute("id", id);
        }
        copyXmlToBinary(*xml, dest);
    }
}
void WallProcessor::setStateInformation(const void *data, int size) {
    auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || !xml->hasTagName(parameters.state.getType()))
        return;
    setStoredEditorSize(xml->getIntAttribute("editorWidth", defaultEditorWidth),
                        xml->getIntAttribute("editorHeight", defaultEditorHeight));
    const int program = xml->getIntAttribute("program", 0);
    const int slot = juce::jlimit(0, 1, xml->getIntAttribute("abSlot", 0));
    juce::MemoryBlock ab0, ab1;
    const bool haveAB = xml->getChildByName("AB") != nullptr;
    for (int i = 0; i < xml->getNumChildElements(); ++i) {
        auto *child = xml->getChildElement(i);
        if (child == nullptr || !child->hasTagName("AB"))
            continue;
        juce::MemoryBlock block;
        block.fromBase64Encoding(child->getStringAttribute("data"));
        if (child->getIntAttribute("index", 0) == 0)
            ab0 = std::move(block);
        else
            ab1 = std::move(block);
    }
    unlockAllParams();
    if (auto *locks = xml->getChildByName("LOCKS")) {
        for (int i = 0; i < locks->getNumChildElements(); ++i)
            if (auto *entry = locks->getChildElement(i); entry != nullptr && entry->hasTagName("LOCK"))
                setParamLocked(entry->getStringAttribute("id"), true);
    }
    for (int i = xml->getNumChildElements() - 1; i >= 0; --i)
        if (auto *child = xml->getChildElement(i);
            child != nullptr && (child->hasTagName("AB") || child->hasTagName("LOCKS")))
            xml->removeChildElement(child, true);
    restoreTree(juce::ValueTree::fromXml(*xml), false, {});
    std::ranges::for_each(undoManagers, [](juce::UndoManager &history) { history.clearUndoHistory(); });
    if (program >= 0 && program < getNumPrograms())
        currentProgram = program;
    if (!haveAB) {
        abCompare.seed(captureParameters());
    } else {
        if (ab0.getSize() == 0)
            ab0 = captureParameters();
        if (ab1.getSize() == 0)
            ab1 = captureParameters();
        abCompare.setSlot(0, ab0);
        abCompare.setSlot(1, ab1);
        abCompare.setActive(slot);
        abCompare.setSlot(slot, captureParameters());
    }
    parameterUndo.setUndoManager(undoManagers[size_t(abCompare.active())]);
    if (auto *editor = getActiveEditor()) {
        const int width = editorWidth;
        const int height = editorHeight;
        if (auto *mm = juce::MessageManager::getInstanceWithoutCreating();
            mm != nullptr && mm->isThisTheMessageThread()) {
            if (editor->getWidth() != width || editor->getHeight() != height)
                editor->setSize(width, height);
        } else {
            juce::Component::SafePointer<juce::AudioProcessorEditor> safeEditor(editor);
            juce::MessageManager::callAsync([safeEditor, width, height]() mutable {
                if (safeEditor != nullptr && (safeEditor->getWidth() != width || safeEditor->getHeight() != height))
                    safeEditor->setSize(width, height);
            });
        }
    }
}
const std::vector<WallPreset> &WallProcessor::factoryPresets() {
    static const std::vector<WallPreset> list{
        // Uplifting (12) — index 1 keeps default lookahead_ms=5.0 and release_link=100.0 for unit tests
        {"Uplifting / Init Clean", {}},
        {"Uplifting / Anthem Master", {
            {"threshold", -6.5f}, {"ceiling", -0.3f}, {"mode", 1}, {"release", 1.35f},
            {"upward", 1.8f}, {"stereo_link", 85.0f}, {"oversampling", 3}
        }},
        {"Uplifting / Pure 138 Lift", {
            {"threshold", -7.5f}, {"ceiling", -0.3f}, {"mode", 1}, {"release", 1.2f},
            {"clip", 15.0f}, {"upward", 2.2f}, {"highpass_hz", 26.0f}, {"stereo_link", 84.0f},
            {"stereo_width", 106.0f}, {"tone_tilt", 0.4f}, {"oversampling", 3}
        }},
        {"Uplifting / Vocal Air Master", {
            {"threshold", -6.0f}, {"ceiling", -0.5f}, {"mode", 1}, {"release", 1.5f},
            {"upward", 1.6f}, {"stereo_link", 88.0f}, {"tone_tilt", 0.6f}, {"lowpass_hz", 21000.0f},
            {"oversampling", 3}
        }},
        {"Uplifting / Euphoria Wall", {
            {"threshold", -8.8f}, {"ceiling", -0.2f}, {"mode", 0}, {"release", 0.8f},
            {"clip", 30.0f}, {"upward", 2.4f}, {"highpass_hz", 28.0f}, {"stereo_width", 108.0f},
            {"oversampling", 2}
        }},
        {"Uplifting / Supersaw Wide", {
            {"threshold", -7.8f}, {"ceiling", -0.25f}, {"mode", 1}, {"release", 1.1f},
            {"upward", 2.0f}, {"stereo_link", 78.0f}, {"release_link", 75.0f}, {"stereo_width", 112.0f},
            {"oversampling", 3}
        }},
        {"Uplifting / Skyline Loudness", {
            {"threshold", -9.2f}, {"ceiling", -0.15f}, {"mode", 0}, {"release", 0.65f},
            {"clip", 40.0f}, {"upward", 2.2f}, {"input_gain", 1.2f}, {"highpass_hz", 30.0f},
            {"oversampling", 3}
        }},
        {"Uplifting / Safe Streaming", {
            {"threshold", -4.5f}, {"ceiling", -1.0f}, {"mode", 3}, {"release", 1.4f},
            {"upward", 1.0f}, {"oversampling", 3}
        }},
        {"Uplifting / Arena Breakdown", {
            {"threshold", -5.2f}, {"ceiling", -0.5f}, {"mode", 1}, {"release", 1.8f},
            {"upward", 2.6f}, {"stereo_width", 110.0f}, {"tone_tilt", -0.3f}, {"oversampling", 3}
        }},
        {"Uplifting / Pluck Transient", {
            {"threshold", -6.8f}, {"ceiling", -0.4f}, {"mode", 0}, {"release", 0.9f},
            {"clip", 20.0f}, {"upward", 1.2f}, {"lookahead_ms", 3.5f}, {"highpass_hz", 25.0f},
            {"oversampling", 2}
        }},
        {"Uplifting / Morning Energy", {
            {"threshold", -7.2f}, {"ceiling", -0.3f}, {"mode", 1}, {"release", 1.25f},
            {"upward", 1.7f}, {"tone_tilt", 0.5f}, {"stereo_link", 82.0f}, {"oversampling", 3}
        }},
        {"Uplifting / High Headroom Club", {
            {"threshold", -5.8f}, {"ceiling", -0.6f}, {"mode", 3}, {"release", 1.2f},
            {"upward", 1.3f}, {"highpass_hz", 26.0f}, {"oversampling", 3}
        }},

        // Rolling (6)
        {"Rolling / Club Punch", {
            {"threshold", -7.5f}, {"ceiling", -0.3f}, {"mode", 0}, {"clip", 25.0f},
            {"release", 0.8f}, {"upward", 1.2f}, {"highpass_hz", 28.0f}, {"highpass_slope", 1}
        }},
        {"Rolling / House Bus Glue", {
            {"threshold", -6.2f}, {"ceiling", -0.35f}, {"mode", 0}, {"clip", 18.0f},
            {"release", 0.95f}, {"upward", 1.0f}, {"stereo_link", 92.0f}, {"release_link", 85.0f}
        }},
        {"Rolling / Tight Kick Guard", {
            {"threshold", -8.0f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 32.0f},
            {"release", 0.6f}, {"lookahead_ms", 2.5f}, {"highpass_hz", 30.0f}, {"highpass_slope", 2}
        }},
        {"Rolling / Groove Ceiling", {
            {"threshold", -6.8f}, {"ceiling", -0.4f}, {"mode", 1}, {"release", 0.85f},
            {"upward", 1.5f}, {"stereo_link", 88.0f}, {"oversampling", 2}
        }},
        {"Rolling / Funky Top Sheen", {
            {"threshold", -7.0f}, {"ceiling", -0.3f}, {"mode", 0}, {"clip", 20.0f},
            {"release", 0.75f}, {"tone_tilt", 0.7f}, {"stereo_width", 104.0f}
        }},
        {"Rolling / Night Drive", {
            {"threshold", -8.4f}, {"ceiling", -0.25f}, {"mode", 2}, {"clip", 36.0f},
            {"release", 0.65f}, {"upward", 1.4f}, {"highpass_hz", 27.0f}
        }},

        // Progressive (6)
        {"Progressive / Deep Warm Glow", {
            {"threshold", -5.0f}, {"ceiling", -0.8f}, {"mode", 4}, {"release", 1.8f},
            {"upward", 0.8f}, {"lowpass_hz", 19000.0f}
        }},
        {"Progressive / Smooth & Full", {
            {"threshold", -6.0f}, {"ceiling", -1.0f}, {"mode", 1}, {"release", 1.6f},
            {"upward", 1.5f}
        }},
        {"Progressive / Melodic Precision", {
            {"threshold", -6.0f}, {"ceiling", -0.5f}, {"mode", 3}, {"release", 1.3f},
            {"upward", 1.0f}, {"oversampling", 3}, {"true_peak", 1.0f}
        }},
        {"Progressive / Sunset Master", {
            {"threshold", -5.5f}, {"ceiling", -0.6f}, {"mode", 1}, {"release", 1.5f},
            {"upward", 1.2f}, {"stereo_width", 108.0f}, {"tone_tilt", -0.2f}, {"oversampling", 3}
        }},
        {"Progressive / Safe Delivery", {
            {"threshold", -3.0f}, {"ceiling", -1.2f}, {"mode", 3}, {"oversampling", 3}
        }},
        {"Progressive / Organic Detail", {
            {"threshold", -4.8f}, {"ceiling", -0.7f}, {"mode", 3}, {"release", 1.7f},
            {"lookahead_ms", 4.5f}, {"lowpass_hz", 20500.0f}, {"oversampling", 3}
        }},

        // Psy (6)
        {"Psy / 140 Kinetic Peak", {
            {"threshold", -9.0f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 40.0f},
            {"release", 0.48f}, {"upward", 1.6f}, {"lookahead_ms", 2.0f}, {"highpass_hz", 30.0f},
            {"oversampling", 3}
        }},
        {"Psy / Laser Fast Recovery", {
            {"threshold", -8.5f}, {"ceiling", -0.2f}, {"mode", 2}, {"clip", 48.0f},
            {"release", 0.35f}, {"stereo_link", 82.0f}, {"tone_tilt", 0.5f}, {"oversampling", 3}
        }},
        {"Psy / Crisp Triplet Wall", {
            {"threshold", -9.5f}, {"ceiling", -0.15f}, {"mode", 2}, {"clip", 52.0f},
            {"release", 0.42f}, {"upward", 2.1f}, {"highpass_hz", 32.0f}, {"highpass_slope", 2}
        }},
        {"Psy / Sub Lock Limiter", {
            {"threshold", -8.2f}, {"ceiling", -0.3f}, {"mode", 0}, {"clip", 30.0f},
            {"release", 0.55f}, {"stereo_link", 95.0f}, {"release_link", 95.0f}, {"highpass_hz", 28.0f}
        }},
        {"Psy / Fullon Loudness", {
            {"threshold", -10.0f}, {"ceiling", -0.15f}, {"mode", 2}, {"clip", 58.0f},
            {"release", 0.40f}, {"upward", 2.4f}, {"input_gain", 1.5f}, {"oversampling", 3}
        }},
        {"Psy / Twilight Punch", {
            {"threshold", -8.8f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 42.0f},
            {"release", 0.50f}, {"tone_tilt", -0.4f}, {"lookahead_ms", 1.8f}
        }},

        // Tech (6)
        {"Tech / Future Rave Crush", {
            {"threshold", -9.5f}, {"ceiling", -0.15f}, {"mode", 2}, {"clip", 50.0f},
            {"release", 0.55f}, {"upward", 2.0f}
        }},
        {"Tech / Mainstage Slam", {
            {"threshold", -9.0f}, {"ceiling", -0.2f}, {"mode", 2}, {"clip", 45.0f},
            {"release", 0.5f}, {"upward", 1.8f}, {"oversampling", 2}
        }},
        {"Tech / Hardstyle Redline", {
            {"threshold", -11.0f}, {"ceiling", 0.0f}, {"mode", 2}, {"clip", 75.0f},
            {"release", 0.35f}, {"upward", 3.0f}, {"true_peak", 0.0f}
        }},
        {"Tech / Electro Buzz Clip", {
            {"threshold", -8.0f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 55.0f},
            {"release", 0.7f}, {"upward", 1.6f}, {"highpass_hz", 25.0f}
        }},
        {"Tech / Push the Wall", {
            {"threshold", -8.0f}, {"mode", 2}, {"clip", 40.0f}, {"release", 0.65f}
        }},
        {"Tech / Warehouse Dark", {
            {"threshold", -9.2f}, {"ceiling", -0.2f}, {"mode", 2}, {"clip", 48.0f},
            {"release", 0.58f}, {"tone_tilt", -0.8f}, {"highpass_hz", 28.0f}, {"highpass_slope", 2}
        }},

        // Bass & Breaks (6)
        {"Bass & Breaks / Dubstep Wall", {
            {"threshold", -10.5f}, {"ceiling", -0.1f}, {"mode", 2}, {"clip", 60.0f},
            {"release", 0.45f}, {"upward", 2.5f}, {"stereo_link", 90.0f}
        }},
        {"Bass & Breaks / DnB 174", {
            {"threshold", -8.5f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 35.0f},
            {"release", 0.6f}, {"upward", 1.5f}, {"highpass_hz", 30.0f}
        }},
        {"Bass & Breaks / Heavy Squeeze", {
            {"threshold", -9.0f}, {"ceiling", -0.3f}, {"mode", 2}, {"clip", 40.0f},
            {"release", 0.65f}, {"upward", 2.2f}, {"stereo_link", 85.0f}
        }},
        {"Bass & Breaks / Sub Boomer", {
            {"threshold", -7.0f}, {"ceiling", -0.2f}, {"mode", 0}, {"clip", 30.0f},
            {"release", 1.1f}, {"upward", 1.4f}, {"highpass_hz", 20.0f}, {"highpass_slope", 0}
        }},
        {"Bass & Breaks / Jungle Snap", {
            {"threshold", -7.8f}, {"ceiling", -0.3f}, {"mode", 0}, {"clip", 28.0f},
            {"release", 0.52f}, {"lookahead_ms", 2.0f}, {"highpass_hz", 28.0f}, {"stereo_width", 106.0f}
        }},
        {"Bass & Breaks / Neuro Smash", {
            {"threshold", -10.2f}, {"ceiling", -0.1f}, {"mode", 2}, {"clip", 65.0f},
            {"release", 0.42f}, {"upward", 2.6f}, {"input_gain", 1.0f}, {"oversampling", 3}
        }},

        // Classic (6)
        {"Classic / Modern Master", {
            {"threshold", -4.0f}, {"mode", 0}
        }},
        {"Classic / Classic Weight", {
            {"threshold", -5.0f}, {"mode", 4}, {"true_peak", 0.0f}, {"release", 1.2f}
        }},
        {"Classic / Warm Vinyl Cut", {
            {"threshold", -4.5f}, {"ceiling", -0.7f}, {"mode", 4}, {"release", 1.5f},
            {"lowpass_hz", 17500.0f}, {"highpass_hz", 30.0f}, {"tone_tilt", -0.5f}
        }},
        {"Classic / Broadcast Calm", {
            {"threshold", -3.5f}, {"ceiling", -1.0f}, {"mode", 3}, {"release", 1.6f},
            {"stereo_link", 100.0f}, {"oversampling", 3}
        }},
        {"Classic / CD Era Peak", {
            {"threshold", -6.5f}, {"ceiling", -0.2f}, {"mode", 4}, {"clip", 18.0f},
            {"release", 0.9f}, {"true_peak", 0.0f}
        }},
        {"Classic / Transparent Bus", {
            {"threshold", -3.8f}, {"ceiling", -0.5f}, {"mode", 1}, {"release", 1.3f},
            {"upward", 0.6f}, {"oversampling", 3}
        }}
    };
    return list;
}

bool WallProcessor::isParamLocked(const juce::String &paramID) const noexcept {
    const auto it = paramLockIndex.find(paramID);
    return it != paramLockIndex.end() && paramLocks[it->second].load(std::memory_order_acquire);
}
void WallProcessor::setParamLocked(const juce::String &paramID, bool locked) {
    if (const auto it = paramLockIndex.find(paramID); it != paramLockIndex.end())
        paramLocks[it->second].store(locked, std::memory_order_release);
}
void WallProcessor::unlockAllParams() {
    for (auto &flag : paramLocks)
        flag.store(false, std::memory_order_release);
}
bool WallProcessor::anyParamLocked() const noexcept {
    return std::ranges::any_of(paramLocks,
                               [](const std::atomic<bool> &flag) { return flag.load(std::memory_order_acquire); });
}
void WallProcessor::setCurrentProgram(int index) {
    if (index < 0 || index >= getNumPrograms() || index == currentProgram)
        return;
    applyPreset(index);
}
const juce::String WallProcessor::getProgramName(int index) {
    const auto &presets = factoryPresets();
    if (index < 0 || index >= int(presets.size()))
        return {};
    return presets[size_t(index)].name;
}
void WallProcessor::applyPreset(int index) {
    const auto &presets = factoryPresets();
    if (index < 0 || index >= static_cast<int>(presets.size()))
        return;
    currentProgram = index;
    suite::ParameterUndo::Group group(parameterUndo, "Preset");
    std::ranges::for_each(getParameters(), [this](juce::AudioProcessorParameter *parameter) {
        if (sessionParameter[size_t(parameter->getParameterIndex())])
            return;
        if (auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(parameter))
            if (isParamLocked(ranged->paramID))
                return;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
        parameter->endChangeGesture();
    });
    std::ranges::for_each(presets[static_cast<size_t>(index)].overrides, [this](const auto &entry) {
        if (isParamLocked(entry.first))
            return;
        auto *parameter = parameters.getParameter(entry.first);
        if (parameter == nullptr)
            return;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(entry.second));
        parameter->endChangeGesture();
    });
    auto &history = undoManagers[size_t(abCompare.active())];
    history.perform(new PropertySwap(parameters, presetNameId, presets[static_cast<size_t>(index)].name));
    history.perform(new PropertySwap(parameters, presetEditedId, false));
}
juce::AudioProcessorEditor *WallProcessor::createEditor() {
    return new WallEditor(*this);
}
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
    return new WallProcessor();
}
