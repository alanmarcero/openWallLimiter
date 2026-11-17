#include "HostChecks.h"
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
        set(p, "lookahead_ms", 5.0f);
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
        auto legacy = p.parameters.copyState();
        for (int i = legacy.getNumChildren() - 1; i >= 0; --i) {
            auto child = legacy.getChild(i);
            const auto id = child.getProperty("id").toString();
            if (id == "lookahead_ms" || id == "release_link")
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
        for (auto* control : editor->getChildren())
            if (dynamic_cast<juce::Button*>(control) || dynamic_cast<juce::ComboBox*>(control))
                require(control->getHeight() >= 40 && control->getWidth() >= 40, "action target is smaller than 40 px");
        checks::controls(p, *editor);
        p.applyPreset(1);
        require(std::abs(p.parameters.getRawParameterValue("lookahead_ms")->load() - 5.0f) < .02f,
                "preset restores default lookahead");
        require(std::abs(p.parameters.getRawParameterValue("release_link")->load() - 100.0f) < .02f,
                "preset restores default release link");
        juce::File output =
            juce::File::getCurrentWorkingDirectory().getChildFile(argc > 1 ? argv[1] : "docs/screenshots");
        output.createDirectory();
        for (int tick = 0; tick < 240; ++tick) {
            auto signal =
                checks::tone(1600, 48000, 997, .2f + .6f * float(.5 + .5 * std::sin(double(tick) * .08)));
            checks::render(p, signal, 256);
            juce::Thread::sleep(34);
            juce::Timer::callPendingTimersSynchronously();
        }
        auto img = editor->createComponentSnapshot(editor->getLocalBounds());
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
