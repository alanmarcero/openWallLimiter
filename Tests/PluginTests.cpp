#include "HostChecks.h"
#include "SuiteChecks.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
static void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
static void set(WallProcessor &p, const char *id, float value) {
    auto *v = p.parameters.getParameter(id);
    require(v != nullptr, id);
    v->setValueNotifyingHost(v->convertTo0to1(value));
}
int main(int argc, char **argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    try {
        checks::latencyAndTargets();
        checks::suiteBehavior();
        checks::typedValues();
        checks::presetLabels();
        checks::slotHistories();
        checks::confirmations();
        checks::meters();
        checks::monitoringMeters();
        checks::routing();
        checks::quality();
        WallProcessor p;
        set(p, "highpass_on", 0);
        set(p, "lowpass_on", 0);
        set(p, "oversampling", 0);
        set(p, "true_peak", 0);
        p.prepareToPlay(48000, 256);
        require(p.getLatencySamples() > 0, "lookahead latency missing");
        const int fixedLatency = p.getLatencySamples();
        set(p, "lookahead_ms", 0.0f);
        require(p.getLatencySamples() == fixedLatency, "lookahead automation keeps host latency fixed");
        set(p, "lookahead_ms", 10.0f);
        require(std::abs(p.parameters.getRawParameterValue("lookahead_ms")->load() - 10.0f) < .02f,
                "lookahead parameter supports 10 ms range");
        require(p.getLatencySamples() == fixedLatency, "10 ms lookahead keeps host latency fixed");
        set(p, "lookahead_ms", 5.0f);

        // Verify linear-phase pre-limiter HPF avoids phase-rotation peak overshoot on 55 Hz square bass.
        {
            WallProcessor linHp;
            checks::neutral(linHp);
            set(linHp, "highpass_on", 1);
            set(linHp, "highpass_hz", 30.0f);
            set(linHp, "highpass_slope", 3.0f);
            linHp.prepareToPlay(48000, 256);
            std::vector<float> sq(16000, 0.0f);
            float maxIn = 0.0f;
            int idx = 0;
            while (idx < static_cast<int>(sq.size())) {
                float sum = 0.0f;
                int harm = 1;
                while (harm <= 23) {
                    sum += std::sin(float(2.0 * 3.141592653589793 * 55.0 * harm * idx / 48000.0)) / float(harm);
                    harm += 2;
                }
                sq[static_cast<size_t>(idx)] = sum;
                maxIn = std::max(maxIn, std::abs(sum));
                ++idx;
            }
            std::ranges::for_each(sq, [maxIn](float &v) { v *= .70f / maxIn; });
            const auto filteredSq = checks::render(linHp, sq, 256);
            float maxOut = 0.0f;
            size_t tailIdx = 8000;
            while (tailIdx < filteredSq.size()) {
                maxOut = std::max(maxOut, std::abs(filteredSq[tailIdx]));
                ++tailIdx;
            }
            require(maxOut < .72f, "pre-limiter HPF phase rotation causes crest overshoot");
        }

        // Verify TruePeakGuard wastes 0 dB ceiling reserve at 0.0 dBTP.
        {
            require(std::abs(wall::TruePeakGuard::reserveDb) < 1e-6f, "true-peak guard still reserves static headroom");
            WallProcessor tpExact;
            checks::neutral(tpExact);
            set(tpExact, "true_peak", 1);
            set(tpExact, "ceiling", 0.0f);
            tpExact.prepareToPlay(48000, 256);
            const auto loudOut = checks::render(tpExact, checks::tone(16000, 48000, 1000, .992f), 256);
            require(checks::rmsTail(loudOut) > .695, "true-peak guard wastes ceiling loudness");
        }

        // Verify F-weighted psychoacoustic noise-shaped dither pushes quantization energy above 15 kHz.
        {
            WallProcessor shapedDither;
            checks::neutral(shapedDither);
            set(shapedDither, "dither", 2.0f);
            shapedDither.prepareToPlay(48000, 256);
            const auto ditherOut = checks::render(shapedDither, std::vector<float>(16384, 0.0f), 256);
            double earBand = 0.0, ultrasonicBand = 0.0;
            int bin = 1;
            while (bin < 64) {
                const double freqMid = 2000.0 + 35.0 * bin;
                const double freqHi = 16500.0 + 95.0 * bin;
                double reM = 0.0, imM = 0.0, reH = 0.0, imH = 0.0;
                size_t nIdx = 8192;
                while (nIdx < 12288) {
                    const double sampleVal = ditherOut[nIdx];
                    const double phM = 2.0 * 3.141592653589793 * freqMid * double(nIdx) / 48000.0;
                    const double phH = 2.0 * 3.141592653589793 * freqHi * double(nIdx) / 48000.0;
                    reM += sampleVal * std::cos(phM);
                    imM += sampleVal * std::sin(phM);
                    reH += sampleVal * std::cos(phH);
                    imH += sampleVal * std::sin(phH);
                    ++nIdx;
                }
                earBand += reM * reM + imM * imM;
                ultrasonicBand += reH * reH + imH * imH;
                ++bin;
            }
            require(ultrasonicBand > 8.0 * earBand, "dither is flat TPDF rather than psychoacoustically noise-shaped");
        }

        // Verify matched-phase oversampling crossfade avoids high-frequency comb cancellation.
        {
            WallProcessor xfade;
            checks::neutral(xfade);
            set(xfade, "oversampling", 0.0f);
            xfade.prepareToPlay(48000, 256);
            const auto hfTone = checks::tone(24000, 48000, 14000, .1f);
            std::vector<float> firstHalf(hfTone.begin(), hfTone.begin() + 10000);
            std::vector<float> secondHalf(hfTone.begin() + 10000, hfTone.end());
            checks::render(xfade, firstHalf, 256);
            set(xfade, "oversampling", 3.0f);
            const auto faded = checks::render(xfade, secondHalf, 256);
            double minWindowRms = 1.0;
            size_t winStart = 0;
            while (winStart + 256 <= faded.size()) {
                double e = 0.0;
                size_t k = 0;
                while (k < 256) {
                    e += double(faded[winStart + k]) * faded[winStart + k];
                    ++k;
                }
                minWindowRms = std::min(minWindowRms, std::sqrt(e / 256.0));
                winStart += 128;
            }
            require(minWindowRms > .063, "oversampling crossfade phase mismatch causes HF comb dip");
        }
        juce::AudioBuffer<float> b(2, 256);
        juce::MidiBuffer midi;
        for (int j = 0; j < 100; j++) {
            for (int i = 0; i < 256; i++) {
                b.setSample(0, i, 2.f * std::sin(float(j * 256 + i) * .15f));
                b.setSample(1, i, .5f * b.getSample(0, i));
            }
            p.processBlock(b, midi);
            for (int c = 0; c < 2; c++)
                for (int i = 0; i < 256; i++)
                    require(std::isfinite(b.getSample(c, i)) && std::abs(b.getSample(c, i)) <= .892f,
                            "output exceeds ceiling");
        }
        set(p, "threshold", -13);
        set(p, "highpass_hz", 37);
        set(p, "lowpass_slope", 4);
        set(p, "mode", 3);
        set(p, "lookahead_ms", 2.5f);
        set(p, "release_link", 62.0f);
        set(p, "input_gain", 3.5f);
        set(p, "stereo_width", 125.0f);
        set(p, "tone_tilt", 2.0f);
        juce::MemoryBlock state;
        p.getStateInformation(state);
        WallProcessor restored;
        restored.setStateInformation(state.getData(), int(state.getSize()));
        for (auto *param : p.getParameters()) {
            auto *identified = dynamic_cast<juce::AudioProcessorParameterWithID *>(param);
            require(identified &&
                        std::abs(param->getValue() -
                                 restored.parameters.getParameter(identified->paramID)->getValue()) < 1e-6f,
                    "state recall mismatch");
        }
        require(std::abs(restored.parameters.getRawParameterValue("lookahead_ms")->load() - 2.5f) < .02f,
                "lookahead state recall");
        require(std::abs(restored.parameters.getRawParameterValue("release_link")->load() - 62.0f) < .02f,
                "release link state recall");
        require(std::abs(restored.parameters.getRawParameterValue("input_gain")->load() - 3.5f) < .02f,
                "input gain state recall");
        require(std::abs(restored.parameters.getRawParameterValue("stereo_width")->load() - 125.0f) < .02f,
                "stereo width state recall");
        require(std::abs(restored.parameters.getRawParameterValue("tone_tilt")->load() - 2.0f) < .02f,
                "tone tilt state recall");
        auto legacy = p.parameters.copyState();
        for (int i = legacy.getNumChildren() - 1; i >= 0; --i) {
            auto child = legacy.getChild(i);
            const auto id = child.getProperty("id").toString();
            if (id == "lookahead_ms" || id == "release_link" || id == "input_gain" || id == "stereo_width" || id == "tone_tilt")
                legacy.removeChild(i, nullptr);
        }
        juce::MemoryBlock oldState;
        if (auto xml = legacy.createXml())
            p.copyXmlToBinary(*xml, oldState);
        else
            throw std::runtime_error("legacy state serialization");
        WallProcessor migrated;
        migrated.setStateInformation(oldState.getData(), int(oldState.getSize()));
        require(std::abs(migrated.parameters.getRawParameterValue("lookahead_ms")->load() - 5.0f) < .02f,
                "legacy state uses full lookahead");
        require(std::abs(migrated.parameters.getRawParameterValue("release_link")->load()) < .02f,
                "legacy state keeps independent release envelopes");
        require(std::abs(migrated.parameters.getRawParameterValue("input_gain")->load()) < .02f,
                "legacy state defaults input gain to 0 dB");
        require(std::abs(migrated.parameters.getRawParameterValue("stereo_width")->load() - 100.0f) < .02f,
                "legacy state defaults stereo width to 100%");
        require(std::abs(migrated.parameters.getRawParameterValue("tone_tilt")->load()) < .02f,
                "legacy state defaults tone tilt to 0 dB");
        auto editor = std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
        require(editor->getWidth() >= 1000, "editor size");
        struct Host : juce::AudioPlayHead {
            PositionInfo position;
            juce::Optional<PositionInfo> getPosition() const override { return position; }
        } host;
        p.setPlayHead(&host);
        juce::AudioBuffer<float> clockAudio(2, 256);
        juce::MidiBuffer clockMidi;
        clockAudio.clear();
        host.position.setIsPlaying(true);
        host.position.setBpm(140.0);
        host.position.setPpqPosition(-1.75);
        p.processBlock(clockAudio, clockMidi);
        require(std::abs(p.artworkPhase() - 0.25f) < 0.0001f, "SVG follows a negative host position");
        const auto onbeat = editor->createComponentSnapshot(juce::Rectangle<int>(17, 8, 58, 58));
        host.position.setPpqPosition(22.875);
        p.processBlock(clockAudio, clockMidi);
        require(std::abs(p.artworkPhase() - 0.875f) < 0.0001f, "SVG follows a host seek");
        const auto offbeat = editor->createComponentSnapshot(juce::Rectangle<int>(17, 8, 58, 58));
        int changed = 0;
        for (int y = 0; y < onbeat.getHeight(); ++y)
            for (int x = 0; x < onbeat.getWidth(); ++x)
                if (onbeat.getPixelAt(x, y) != offbeat.getPixelAt(x, y)) ++changed;
        require(changed > 30, "Native SVG pixels move with beat phase");
        host.position.setIsPlaying(false);
        host.position.setPpqPosition(0);
        p.processBlock(clockAudio, clockMidi);
        require(std::abs(p.artworkPhase() - 0.875f) < 0.0001f, "SVG holds phase when the host stops");
        p.setPlayHead(nullptr);
        p.processBlock(clockAudio, clockMidi);
        p.processBlock(clockAudio, clockMidi);
        require(p.artworkPhase() > 0.875f, "SVG advances on audio time without host transport");
        checks::layout(*editor);
        checks::accessibility(*editor);
        checks::tooltips(p, *editor);
        checks::inactiveControls(p, *editor);
        checks::controls(p, *editor);
        checks::editorSuite(p, *editor);
        auto *presetsBox = dynamic_cast<juce::ComboBox *>(checks::find(*editor, "presets"));
        require(presetsBox != nullptr && presetsBox->getNumItems() == 48, "expected 48 factory presets");
        require(presetsBox->getRootMenu()->getNumItems() == 7, "expected 7 dance-style preset submenus");
        p.applyPreset(1);
        require(std::abs(p.parameters.getRawParameterValue("lookahead_ms")->load() - 5.0f) < .02f,
                "preset restores default lookahead");
        require(std::abs(p.parameters.getRawParameterValue("release_link")->load() - 100.0f) < .02f,
                "preset restores default release link");
        {
            WallProcessor defaults;
            const auto userPresetBlock = defaults.captureParameters();
            WallProcessor lockProc;
            std::vector<std::pair<juce::String, float>> expectedValues;
            for (auto *param : lockProc.getParameters()) {
                auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(param);
                require(ranged != nullptr, "expected RangedAudioParameter");
                require(!lockProc.isParamLocked(ranged->paramID), "parameter starts unlocked");
                const float targetNorm = ranged->getDefaultValue() < 0.5f ? 1.0f : 0.0f;
                ranged->setValueNotifyingHost(targetNorm);
                const float lockedNorm = ranged->getValue();
                lockProc.setParamLocked(ranged->paramID, true);
                require(lockProc.isParamLocked(ranged->paramID), "parameter did not lock");
                expectedValues.emplace_back(ranged->paramID, lockedNorm);
            }
            require(lockProc.anyParamLocked(), "anyParamLocked should be true");
            juce::MemoryBlock lockState;
            lockProc.getStateInformation(lockState);
            WallProcessor recalledLock;
            recalledLock.setStateInformation(lockState.getData(), int(lockState.getSize()));
            recalledLock.setCurrentProgram(1);
            recalledLock.setCurrentProgram(2);
            recalledLock.loadParameterState(userPresetBlock, "UserLockTest");
            for (const auto &[id, expectedNorm] : expectedValues) {
                require(recalledLock.isParamLocked(id), "parameter lock did not survive state recall");
                auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(recalledLock.parameters.getParameter(id));
                require(ranged != nullptr && std::abs(ranged->getValue() - expectedNorm) < 1e-4f,
                        "locked parameter was overwritten by preset load");
            }
            recalledLock.unlockAllParams();
            require(!recalledLock.anyParamLocked(), "unlockAllParams did not clear all locks");
            for (const auto &[id, _] : expectedValues)
                require(!recalledLock.isParamLocked(id), "parameter remained locked after unlockAllParams");
        }
        juce::File output =
            juce::File::getCurrentWorkingDirectory().getChildFile(argc > 1 ? argv[1] : "docs/screenshots");
        output.createDirectory();
        // The checks above load a factory preset and move parameters. The published
        // preview is a separate editor on the factory default patch at the default size.
        WallProcessor preview;
        preview.prepareToPlay(48000, 256);
        auto previewEditor = std::unique_ptr<juce::AudioProcessorEditor>(preview.createEditor());
        require(previewEditor->getWidth() == WallProcessor::defaultEditorWidth, "preview width");
        require(previewEditor->getHeight() == WallProcessor::defaultEditorHeight, "preview height");
        require(preview.getCurrentProgram() == 0, "preview program");
        require(std::abs(preview.parameters.getRawParameterValue("threshold")->load()) < .02f, "preview threshold");
        require(std::abs(preview.parameters.getRawParameterValue("ceiling")->load() + 1.f) < .02f, "preview ceiling");
        require(std::abs(preview.parameters.getRawParameterValue("mode")->load()) < .02f, "preview mode");
        require(std::lround(preview.parameters.getRawParameterValue("oversampling")->load()) == 2, "preview oversampling");
        for (int tick = 0; tick < 240; ++tick) {
            auto signal =
                checks::tone(1600, 48000, 997, .2f + .6f * float(.5 + .5 * std::sin(double(tick) * .08)));
            checks::render(preview, signal, 256);
            juce::Thread::sleep(34);
            juce::Timer::callPendingTimersSynchronously();
        }
        auto img = previewEditor->createComponentSnapshot(previewEditor->getLocalBounds());
        require(img.getWidth() == WallProcessor::defaultEditorWidth &&
                    img.getHeight() == WallProcessor::defaultEditorHeight,
                "preview snapshot size");
        juce::MemoryOutputStream png;
        juce::PNGImageFormat().writeImageToStream(img, png);
        require(output.getChildFile("openWallLimiter.png").replaceWithData(png.getData(), png.getDataSize()),
                "screenshot write failed");
        std::cout << "PASS ceiling, host parameters, state recall, native editor screenshot\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
