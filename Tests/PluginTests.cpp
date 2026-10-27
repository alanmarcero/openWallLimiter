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
        auto editor = std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
        require(editor->getWidth() >= 1000, "editor size");
        checks::controls(p, *editor);
        p.applyPreset(1);
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
