#pragma once
#include "PluginProcessor.h"
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>
namespace checks {
inline void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
inline void set(WallProcessor &p, const char *id, float value) {
    auto *v = p.parameters.getParameter(id);
    require(v != nullptr, id);
    v->setValueNotifyingHost(v->convertTo0to1(value));
}
inline void neutral(WallProcessor &p) {
    set(p, "highpass_on", 0);
    set(p, "lowpass_on", 0);
    set(p, "oversampling", 0);
    set(p, "true_peak", 0);
    set(p, "ceiling", 0);
}
inline std::vector<float> render(WallProcessor &p, const std::vector<float> &signal, int block = 257,
                                 bool bypass = false) {
    juce::MidiBuffer midi;
    std::vector<float> out;
    out.reserve(signal.size());
    for (int offset = 0; offset < int(signal.size()); offset += block) {
        const int n = std::min(block, int(signal.size()) - offset);
        juce::AudioBuffer<float> b(p.getTotalNumInputChannels(), n);
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < n; ++i)
                b.setSample(c, i, signal[size_t(offset + i)]);
        if (bypass)
            p.processBlockBypassed(b, midi);
        else
            p.processBlock(b, midi);
        for (int i = 0; i < n; ++i) {
            require(std::isfinite(b.getSample(0, i)), "nonfinite plugin output");
            out.push_back(b.getSample(0, i));
        }
    }
    return out;
}
inline std::vector<float> tone(int samples, double sr, double hz, float amplitude) {
    std::vector<float> x(size_t(samples), 0.f);
    for (int i = 0; i < samples; ++i)
        x[size_t(i)] = amplitude * float(std::sin(2 * std::numbers::pi * hz * double(i) / sr + .73));
    return x;
}
inline double rmsTail(const std::vector<float> &x) {
    double e = 0;
    for (size_t i = x.size() / 2; i < x.size(); ++i)
        e += x[i] * x[i];
    return std::sqrt(e / double(x.size() - x.size() / 2));
}
inline float reconstructedPeak(const std::vector<float> &x) {
    // Independent 16-phase, 64-tap windowed-sinc reconstruction, distinct from JUCE's half-band cascade.
    std::array<std::array<double, 64>, 16> kernels{};
    for (int phase = 0; phase < 16; ++phase) {
        double sum = 0;
        for (int k = -31; k <= 32; ++k) {
            double t = double(phase) / 16. - k;
            double c = std::abs(t) < 1e-10 ? 1 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            c *= .5 + .5 * std::cos(std::numbers::pi * t / 33);
            kernels[size_t(phase)][size_t(k + 31)] = c;
            sum += c;
        }
        for (auto &c : kernels[size_t(phase)])
            c /= sum;
    }
    double peak = 0;
    for (size_t n = 32; n + 33 < x.size(); ++n)
        for (auto &kernel : kernels) {
            double v = 0;
            for (int k = -31; k <= 32; ++k)
                v += x[size_t(int(n) + k)] * kernel[size_t(k + 31)];
            peak = std::max(peak, std::abs(v));
        }
    return float(peak);
}
inline void routing() {
    WallProcessor p;
    neutral(p);
    p.prepareToPlay(48000, 256);
    const auto x = tone(12000, 48000, 777, .02f);
    const auto y = render(p, x);
    const int latency = p.getLatencySamples();
    for (size_t i = size_t(latency); i < y.size(); ++i)
        require(std::abs(y[i] - x[i - size_t(latency)]) < 1e-6f, "unity path latency mismatch");
    set(p, "delta", 1);
    p.prepareToPlay(48000, 256);
    const auto delta = render(p, x);
    for (float v : delta)
        require(std::abs(v) < 1e-6f, "delta does not null latency-aligned dry");
    set(p, "threshold", -24);
    set(p, "delta", 0);
    p.prepareToPlay(48000, 256);
    const auto bypass = render(p, x, 257, true);
    for (size_t i = size_t(latency + 1000); i < bypass.size(); ++i)
        require(std::abs(bypass[i] - x[i - size_t(latency)]) < 1e-6f, "host bypass not latency aligned");
    for (int slope = 0; slope < 5; ++slope) {
        WallProcessor hp;
        neutral(hp);
        set(hp, "highpass_on", 1);
        set(hp, "highpass_hz", 200);
        set(hp, "highpass_slope", float(slope));
        hp.prepareToPlay(48000, 256);
        require(rmsTail(render(hp, tone(24000, 48000, 20, .1f))) < .002,
                "high-pass slope does not reject sub bass");
        WallProcessor lp;
        neutral(lp);
        set(lp, "lowpass_on", 1);
        set(lp, "lowpass_hz", 8000);
        set(lp, "lowpass_slope", float(slope));
        lp.prepareToPlay(48000, 256);
        require(rmsTail(render(lp, tone(12000, 48000, 20000, .1f))) < .004,
                "low-pass slope does not reject treble");
    }
    for (bool highpass : {true, false}) {
        WallProcessor p;
        neutral(p);
        const char *on = highpass ? "highpass_on" : "lowpass_on";
        const char *hz = highpass ? "highpass_hz" : "lowpass_hz";
        const auto signal = tone(24000, 48000, highpass ? 40 : 18000, .1f);
        p.prepareToPlay(48000, 256);
        render(p, signal);
        set(p, hz, highpass ? 200 : 8000);
        render(p, signal);
        set(p, on, 1);
        require(rmsTail(render(p, signal)) < .001,
                "frequency changed while bypassed is not applied on enable");
    }
    // A strong DC offset is removed before the limiter detector can duck the small wanted tone.
    WallProcessor filtered;
    neutral(filtered);
    set(filtered, "highpass_on", 1);
    set(filtered, "highpass_hz", 100);
    set(filtered, "highpass_slope", 1);
    filtered.prepareToPlay(48000, 256);
    auto dc = tone(48000, 48000, 1000, .1f);
    for (auto &v : dc)
        v += 8;
    require(rmsTail(render(filtered, dc)) > .065, "high-pass filter sits after limiter");
    std::cout << "PASS unity/delta/host bypass delay, five filter slopes, pre-limiter routing\n";
}
inline void quality() {
    for (double sr : {44100., 48000., 96000., 192000.}) {
        WallProcessor p;
        neutral(p);
        p.prepareToPlay(sr, 64);
        const int latency = p.getLatencySamples();
        for (int quality = 0; quality < 5; ++quality) {
            set(p, "oversampling", float(quality));
            render(p, tone(12000, sr, 2000, .02f), 4097);
            require(p.getLatencySamples() == latency, "quality change alters latency");
        }
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::mono());
        layout.outputBuses.add(juce::AudioChannelSet::mono());
        p.releaseResources();
        require(p.setBusesLayout(layout), "mono rejected");
        p.prepareToPlay(sr, 256);
        render(p, tone(4096, sr, 1000, .1f));
    }
    for (double rate : {44100., 48000., 96000.})
        for (int mode = 0; mode < 5; ++mode) {
            WallProcessor p;
            neutral(p);
            set(p, "threshold", -12);
            set(p, "ceiling", -1);
            set(p, "mode", float(mode));
            set(p, "clip", 100);
            set(p, "true_peak", 1);
            p.prepareToPlay(rate, 256);
            for (double hz : {997., rate * .25, rate * .285, rate * .29, rate * .295, rate * .38,
                              rate * .40625, rate * .48}) {
                auto y = render(p, tone(8192, rate, hz, 1.4f));
                float peak = reconstructedPeak(y);
                std::cout << "true-peak mode=" << mode << " hz=" << hz << " dBTP=" << 20 * std::log10(peak)
                          << '\n';
                require(peak <= std::pow(10.f, -1.f / 20.f) * 1.001f,
                        "reconstructed output exceeds true-peak ceiling");
            }
        }
    for (int pattern = 0; pattern < 8; ++pattern) {
        WallProcessor p;
        neutral(p);
        set(p, "true_peak", 1);
        set(p, "ceiling", -1);
        set(p, "threshold", -12);
        set(p, "clip", 100);
        set(p, "mode", 2);
        p.prepareToPlay(48000, 256);
        std::vector<float> signal(16384);
        std::mt19937 random(103);
        std::uniform_real_distribution<float> noise(-1.4f, 1.4f);
        for (size_t i = 0; i < signal.size(); ++i) {
            const double phase = 2 * std::numbers::pi * double(i);
            switch (pattern) {
            case 0:
                signal[i] = noise(random);
                break;
            case 1:
                signal[i] = i % 2 ? 1.4f : -1.4f;
                break;
            case 2:
                signal[i] = i % 65 == 0 ? 10.f : 0.f;
                break;
            case 3:
                signal[i] = i % 300 < 100 ? float(1.4 * std::sin(phase * .48 + .73)) : 0.f;
                break;
            case 4:
                signal[i] = float(.7 * (std::sin(phase * .23) + std::sin(phase * .47 + .73)));
                break;
            case 5:
                signal[i] = i % 500 < 200 ? float(1.4 * std::sin(phase * .38 + .73)) : 0.f;
                break;
            case 6:
                signal[i] = float(1.4 * std::sin(phase * (.01 + .48 * double(i) / double(signal.size()))));
                break;
            case 7:
                signal[i] = i % 300 < 100 ? float(1.4 * std::sin(phase * .29 + .73)) : 0.f;
                break;
            }
        }
        require(reconstructedPeak(render(p, signal)) <= std::pow(10.f, -.05f) * 1.001f,
                "transient/noise/chirp exceeds true-peak ceiling");
    }
    std::cout << "PASS sample rates, oversized blocks, mono, quality switching, independent true-peak "
                 "reconstruction\n";
}
inline juce::Component *find(juce::Component &root, const juce::String &id) {
    if (root.getComponentID() == id)
        return &root;
    for (auto *c : root.getChildren())
        if (auto *found = find(*c, id))
            return found;
    return nullptr;
}
inline void controls(WallProcessor &p, juce::AudioProcessorEditor &editor) {
    for (auto id :
         {"threshold", "ceiling", "release", "clip", "upward", "stereo_link", "highpass_hz", "lowpass_hz"}) {
        auto *s = dynamic_cast<juce::Slider *>(find(editor, id));
        require(s, id);
        s->setValue(s->getMinimum() + (s->getMaximum() - s->getMinimum()) * .37, juce::sendNotificationSync);
        require(std::abs(p.parameters.getRawParameterValue(id)->load() - float(s->getValue())) < .02f,
                "slider disconnected");
    }
    for (auto id : {"highpass_slope", "lowpass_slope", "oversampling", "dither"}) {
        auto *c = dynamic_cast<juce::ComboBox *>(find(editor, id));
        require(c, id);
        c->setSelectedId(2, juce::sendNotificationSync);
        require(int(p.parameters.getRawParameterValue(id)->load()) == 1, "selector disconnected");
    }
    for (auto id : {"highpass_on", "lowpass_on", "true_peak", "gain_match", "delta", "bypass"}) {
        auto *b = dynamic_cast<juce::TextButton *>(find(editor, id));
        require(b, id);
        b->setToggleState(true, juce::sendNotificationSync);
        require(p.parameters.getRawParameterValue(id)->load() > .5f, "toggle disconnected");
    }
    for (int i = 0; i < 5; ++i) {
        auto *b = dynamic_cast<juce::TextButton *>(find(editor, "mode" + juce::String(i)));
        require(b, "mode button missing");
        b->onClick();
        require(int(p.parameters.getRawParameterValue("mode")->load()) == i, "mode disconnected");
    }
    std::cout << "PASS native control attachments\n";
}
} // namespace checks
