#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr std::array<const char *, 19> ids{
    "threshold",      "ceiling",     "mode",       "release",       "clip",      "upward", "stereo_link",
    "oversampling",   "true_peak",   "dither",     "gain_match",    "delta",     "bypass", "highpass_hz",
    "highpass_slope", "highpass_on", "lowpass_hz", "lowpass_slope", "lowpass_on"};
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
    const bool tp = value(8) > .5f;
    wall::Settings settings;
    settings.thresholdDb = value(0);
    settings.ceilingDb = value(1);
    settings.mode = static_cast<wall::Mode>(int(value(2)));
    settings.release = value(3);
    settings.clip = value(4) * .01f;
    settings.upwardDb = value(5);
    settings.stereoLink = value(6) * .01f;
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
void WallProcessor::applyPreset(int index) {
    const auto set = [&](const char *id, float v) {
        auto *p = parameters.getParameter(id);
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(v));
        p->endChangeGesture();
    };
    for (auto *p : getParameters()) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->getDefaultValue());
        p->endChangeGesture();
    }
    if (index == 1) {
        set("threshold", -4);
        set("mode", 0);
    }
    if (index == 2) {
        set("threshold", -6);
        set("mode", 1);
        set("release", 1.6f);
        set("upward", 1.5f);
    }
    if (index == 3) {
        set("threshold", -8);
        set("mode", 2);
        set("clip", 40);
        set("release", .65f);
    }
    if (index == 4) {
        set("threshold", -3);
        set("mode", 3);
        set("ceiling", -1.2f);
        set("oversampling", 3);
    }
    if (index == 5) {
        set("threshold", -5);
        set("mode", 4);
        set("true_peak", 0);
        set("release", 1.2f);
    }
}
juce::AudioProcessorEditor *WallProcessor::createEditor() {
    return new WallEditor(*this);
}
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
    return new WallProcessor();
}
