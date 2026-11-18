#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

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
} // namespace
WallProcessor::WallProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "OpenWallLimiter", wall::parameterLayout()) {
    for (size_t i = 0; i < ids.size(); ++i)
        values[i] = parameters.getRawParameterValue(ids[i]);
}
bool WallProcessor::isBusesLayoutSupported(const BusesLayout &l) const {
    return (l.getMainInputChannelSet() == juce::AudioChannelSet::mono() ||
            l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()) &&
           l.getMainInputChannelSet() == l.getMainOutputChannelSet();
}
void WallProcessor::prepareToPlay(double sr, int block) {
    artworkClock.reset();
    ready = false;
    sr = std::isfinite(sr) ? std::clamp(sr, 8000., 192000.) : 48000.;
    preparedRate = sr;
    channels = std::clamp(getTotalNumInputChannels(), 1, 2);
    chunkSize = std::clamp(block, 32, 4096);
    input.setSize(channels, chunkSize);
    dry.setSize(channels, chunkSize);
    metering.setSize(channels, chunkSize);
    latency = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto &p = paths[i];
        p.oversampler = std::make_unique<Oversampler>(size_t(channels), i,
                                                      Oversampler::filterHalfBandFIREquiripple, true, true);
        p.oversampler->initProcessing(size_t(chunkSize));
        p.limiter.prepare(sr * double(1 << i), channels);
        p.nativeLatency =
            float(p.limiter.latencySamples()) / float(1 << i) + p.oversampler->getLatencyInSamples();
        latency = std::max(latency, int(std::ceil(p.nativeLatency)));
        p.buffer.setSize(channels, chunkSize);
    }
    const juce::dsp::ProcessSpec spec{sr, juce::uint32(chunkSize), juce::uint32(channels)};
    for (auto &p : paths) {
        p.pad.setMaximumDelayInSamples(latency + 4);
        p.pad.prepare(spec);
        p.pad.setDelay(float(latency) - p.nativeLatency);
    }
    latency += wall::TruePeakGuard::delaySamples;
    peakGuard.prepare(sr);
    dryDelay.setMaximumDelayInSamples(latency + 4);
    dryDelay.prepare(spec);
    dryDelay.setDelay(float(latency));
    filters[0].setCutoff(value(13));
    filters[0].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(14))));
    filters[0].setBypassed(value(15) < .5f);
    filters[1].setCutoff(value(16));
    filters[1].setSlope(static_cast<analog_master::core::FilterSlope>(int(value(17))));
    filters[1].setBypassed(value(18) < .5f);
    for (auto &f : filters)
        f.prepare(sr);
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
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i) {
            p.pad.pushSample(c, p.buffer.getSample(c, i));
            p.buffer.setSample(c, i, p.pad.popSample(c));
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
    settings.ceilingDb = value(1) + (tp ? wall::TruePeakGuard::reserveDb : 0.f);
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
        warmup = latency + fadeLength;
        fade = 0;
    }
    runPath(currentPath, settings, n);
    if (nextPath >= 0)
        runPath(nextPath, settings, n);
    effectiveFactor.store(1 << (nextPath >= 0 ? nextPath : currentPath), std::memory_order_relaxed);
    bypassMix.setTargetValue(hostBypass || value(12) > .5f ? 1.f : 0.f);
    matchGain.setTargetValue(value(10) > .5f ? juce::Decibels::decibelsToGain(value(0)) : 1.f);
    deltaMix.setTargetValue(value(11) > .5f ? 1.f : 0.f);
    const float ceiling = juce::Decibels::decibelsToGain(value(1));
    const int dither = int(value(9));
    const float scale = dither == 1 ? 8388608.f : 32768.f;
    const float quantCeiling = std::floor(ceiling * scale) / scale;
    for (int i = 0; i < n; ++i) {
        float cross = 0;
        if (nextPath >= 0) {
            if (warmup > 0)
                --warmup;
            else
                cross = std::min(1.f, float(++fade) / float(fadeLength));
        }
        const float bypass = bypassMix.getNextValue(), match = matchGain.getNextValue(),
                    delta = deltaMix.getNextValue();
        std::array<float, 2> protectedSample{};
        for (int c = 0; c < channels; ++c) {
            float wet = paths[size_t(currentPath)].buffer.getSample(c, i);
            if (nextPath >= 0)
                wet += cross * (paths[size_t(nextPath)].buffer.getSample(c, i) - wet);
            protectedSample[size_t(c)] = finite(wet);
        }
        peakGuard.process(protectedSample[0], protectedSample[1], ceiling, tp);
        for (int c = 0; c < channels; ++c) {
            float wet = std::clamp(protectedSample[size_t(c)], -ceiling, ceiling);
            if (dither > 0) {
                wet = std::round(wet * scale + randomUnit() - randomUnit()) / scale;
                wet = std::clamp(wet, -quantCeiling, quantCeiling);
            }
            wet *= match;
            const float original = dry.getSample(c, i);
            const float monitor = wet - delta * original;
            const float output = monitor + bypass * (original - monitor);
            buffer.setSample(c, offset + i, output);
            metering.setSample(c, i, output);
            outPeak = std::max(outPeak, std::abs(output));
        }
        meter.process(buffer.getSample(0, offset + i), channels > 1 ? buffer.getSample(1, offset + i) : 0.f);
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
    for (size_t c = 0; c < up.getNumChannels(); ++c)
        for (size_t i = 0; i < up.getNumSamples(); ++i)
            truePeak = std::max(truePeak, std::abs(up.getSample(int(c), int(i))));
    const float decay = float(20. * n / preparedRate);
    inputDb.store(std::max(db(inPeak), inputDb.load() - decay));
    outputDb.store(std::max(db(outPeak), outputDb.load() - decay));
    reductionDb.store(reduction);
    truePeakDb.store(std::max(db(truePeak), truePeakDb.load()));
    shortLufs.store(meter.shortTermLufs());
    integratedLufs.store(meter.integratedLufs());
}
void WallProcessor::getStateInformation(juce::MemoryBlock &dest) {
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, dest);
}
void WallProcessor::setStateInformation(const void *data, int size) {
    if (auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName(parameters.state.getType())) {
        auto incoming = juce::ValueTree::fromXml(*xml);
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
        parameters.replaceState(incoming);
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

void WallProcessor::applyPreset(int index) {
    const auto &presets = factoryPresets();
    if (index < 0 || index >= static_cast<int>(presets.size()))
        return;
    std::ranges::for_each(getParameters(), [](juce::AudioProcessorParameter *parameter) {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->getDefaultValue());
        parameter->endChangeGesture();
    });
    std::ranges::for_each(presets[static_cast<size_t>(index)].overrides, [this](const auto &entry) {
        auto *parameter = parameters.getParameter(entry.first);
        if (parameter == nullptr)
            return;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(entry.second));
        parameter->endChangeGesture();
    });
}
juce::AudioProcessorEditor *WallProcessor::createEditor() {
    return new WallEditor(*this);
}
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
    return new WallProcessor();
}
