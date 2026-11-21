#pragma once
#include "HostChecks.h"
#include "LoudnessTarget.h"
#include "TruePeakGuard.h"
#include "suite/PresetBrowser.h"
#include "suite/PresetLibrary.h"
#include <cmath>
#include <juce_dsp/juce_dsp.h>
namespace checks {
inline int referenceLatency(double sampleRate, int block) {
    if (std::isfinite(sampleRate)) sampleRate = std::clamp(sampleRate, 8000.0, 192000.0);
    if (!std::isfinite(sampleRate)) sampleRate = 48000.0;
    const int chunk = std::clamp(block, 32, 4096);
    analog_master::core::MultiPoleFilter subCut{analog_master::core::FilterType::HighPass};
    subCut.setLinearPhaseSubCut(true);
    subCut.prepare(sampleRate);
    int latency = 0;
    int i = 0;
    while (i < 5) {
        juce::dsp::Oversampling<float> oversampler(2, size_t(i),
                                                   juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                   true, true);
        oversampler.initProcessing(size_t(chunk));
        wall::Limiter limiter;
        limiter.prepare(sampleRate * double(1 << i), 2);
        const float native = float(limiter.latencySamples()) / float(1 << i) + oversampler.getLatencyInSamples();
        latency = std::max(latency, int(std::ceil(native)));
        ++i;
    }
    return subCut.latencySamples() + latency + WallProcessor::sincPadHalfSpan + wall::TruePeakGuard::delaySamples;
}
inline void latencyAndTargets() {
    require(wall::TruePeakGuard::delaySamples == 128, "true-peak guard is not 128 samples");
    const auto readme = juce::File(OWL_SOURCE_DIR).getChildFile("README.md").loadFileAsString();
    require(readme.contains("128-sample true-peak guard"), "README omits the true-peak guard");
    require(readme.contains("longest oversampling"), "README omits oversampling delay");
    require(readme.contains("milliseconds and samples"), "README omits the latency readout");
    require(!readme.contains("fixed at 5 ms"), "README still says latency is fixed at 5 ms");
    require(!readme.contains("stays fixed at 5 ms"), "README still says latency stays fixed at 5 ms");
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        WallProcessor processor;
        processor.prepareToPlay(sr, 256);
        const int expected = referenceLatency(sr, 256);
        const int samples = processor.getLatencySamples();
        require(std::abs(processor.preparedSampleRate() - sr) < 1.0, "prepared sample rate");
        require(samples == expected, "reported latency does not match limiter, oversampler, and guard");
        require(samples > int(sr * 0.005) + wall::TruePeakGuard::delaySamples, "reported latency is only the 5 ms core");
        const auto text = wall::latencyReadout(samples, sr);
        require(text.contains(juce::String(samples) + " samples"), "readout omits samples");
        require(text.startsWith(juce::String(samples * 1000.0 / sr, 2) + " ms"), "readout omits milliseconds");
        const int fixed = samples;
        set(processor, "lookahead_ms", 0.f);
        set(processor, "oversampling", 4.f);
        set(processor, "true_peak", 0.f);
        require(processor.getLatencySamples() == fixed, "lookahead, oversampling, or true peak changed host latency");
    }
    require(!wall::targetLufs(0, -14.f).has_value(), "off mode still has a target");
    require(wall::targetLufs(1, 0.f).value_or(0.f) == wall::lufsStreaming, "streaming target is not -14");
    require(wall::targetLufs(2, 0.f).value_or(0.f) == wall::lufsClub, "club target is not -9");
    require(std::abs(wall::targetLufs(3, -11.5f).value_or(0.f) + 11.5f) < 1.0e-4f, "custom target ignored");
    require(std::abs(wall::matchedInputGain(0.f, -16.f, -14.f) - 2.f) < 1.0e-4f, "match gain delta");
    require(std::abs(wall::matchedInputGain(11.f, -30.f, -9.f) - 12.f) < 1.0e-4f, "match gain clamp");
    require(std::abs(wall::matchedInputGain(0.f, -100.f, -14.f)) < 1.0e-4f, "invalid loudness changed gain");
    const auto guidance = wall::loudnessGuidance(1, -14.f, -16.2f, -20.f);
    require(guidance.contains("2.2 LU under") && guidance.contains("-14.0"), "guidance text");
    require(wall::loudnessGuidance(0, -14.f, -10.f, -10.f) == "Loudness target off", "off guidance");
    WallProcessor loud;
    set(loud, "highpass_on", 0);
    set(loud, "lowpass_on", 0);
    set(loud, "true_peak", 0);
    set(loud, "oversampling", 0);
    loud.prepareToPlay(48000, 256);
    render(loud, tone(48000, 48000, 1000, .1f));
    require(loud.integratedLufs.load() > -99.f, "integrated loudness was not measured");
    set(loud, "lufs_mode", 1);
    const float measured = loud.integratedLufs.load();
    loud.matchLoudnessTarget();
    const float gain = loud.parameters.getRawParameterValue("input_gain")->load();
    const float expectedGain = wall::matchedInputGain(0.f, measured, wall::lufsStreaming);
    require(std::abs(gain - expectedGain) < .15f, "match did not move input drive toward -14 LUFS");
    std::cout << "PASS latency reporting, loudness targets\n";
}
inline void suiteBehavior() {
    WallProcessor programs;
    const auto &factory = WallProcessor::factoryPresets();
    require(programs.getNumPrograms() == 48 && int(factory.size()) == 48, "host program count is not 48");
    for (int i = 0; i < programs.getNumPrograms(); ++i)
        require(programs.getProgramName(i) == factory[size_t(i)].name, "program name drifted from factory preset");
    require(programs.getProgramName(-1).isEmpty() && programs.getProgramName(48).isEmpty(), "program name bounds");
    programs.setCurrentProgram(4);
    require(programs.getCurrentProgram() == 4, "current program was not stored");
    require(std::abs(programs.parameters.getRawParameterValue("threshold")->load() + 8.8f) < .02f,
            "program did not load Euphoria Wall");
    programs.changeProgramName(4, "renamed");
    require(programs.getProgramName(4) == "Uplifting / Euphoria Wall", "factory program was renamed");
    set(programs, "threshold", -3.f);
    programs.setStoredEditorSize(1400, 900);
    juce::MemoryBlock state;
    programs.getStateInformation(state);
    WallProcessor recalled;
    recalled.setStateInformation(state.getData(), int(state.getSize()));
    require(recalled.getCurrentProgram() == 4, "program index was not recalled");
    require(std::abs(recalled.parameters.getRawParameterValue("threshold")->load() + 3.f) < .02f,
            "program recall overwrote the saved threshold");
    require(recalled.storedEditorWidth() == 1400 && recalled.storedEditorHeight() == 900, "editor size was not recalled");
    programs.setCurrentProgram(99);
    require(programs.getCurrentProgram() == 4, "out-of-range program changed the selection");

    WallProcessor legacy;
    auto tree = legacy.parameters.copyState();
    for (int i = tree.getNumChildren() - 1; i >= 0; --i) {
        const auto id = tree.getChild(i).getProperty("id").toString();
        if (id == "lufs_mode" || id == "lufs_target")
            tree.removeChild(i, nullptr);
    }
    juce::MemoryBlock oldState;
    if (auto xml = tree.createXml())
        legacy.copyXmlToBinary(*xml, oldState);
    WallProcessor migrated;
    migrated.setStoredEditorSize(1500, 1000);
    migrated.setStateInformation(oldState.getData(), int(oldState.getSize()));
    require(std::abs(migrated.parameters.getRawParameterValue("lufs_mode")->load()) < .02f, "old session target mode");
    require(std::abs(migrated.parameters.getRawParameterValue("lufs_target")->load() + 14.f) < .02f,
            "old session custom target");
    require(migrated.storedEditorWidth() == WallProcessor::defaultEditorWidth, "old session editor width");

    WallProcessor undo;
    auto *threshold = undo.parameters.getParameter("threshold");
    auto *ceiling = undo.parameters.getParameter("ceiling");
    threshold->beginChangeGesture();
    threshold->setValueNotifyingHost(threshold->convertTo0to1(-3.f));
    threshold->setValueNotifyingHost(threshold->convertTo0to1(-6.f));
    threshold->endChangeGesture();
    require(undo.canUndo() && !undo.canRedo(), "drag was not one undo step");
    undo.undo();
    require(std::abs(undo.parameters.getRawParameterValue("threshold")->load()) < .02f, "undo did not restore the drag");
    require(!undo.canUndo(), "a drag became more than one undo step");
    undo.redo();
    require(std::abs(undo.parameters.getRawParameterValue("threshold")->load() + 6.f) < .02f, "redo lost the drag");
    undo.undo();
    threshold->beginChangeGesture();
    threshold->setValueNotifyingHost(threshold->convertTo0to1(-3.f));
    threshold->endChangeGesture();
    ceiling->beginChangeGesture();
    ceiling->setValueNotifyingHost(ceiling->convertTo0to1(-4.f));
    ceiling->endChangeGesture();
    undo.undo();
    require(std::abs(undo.parameters.getRawParameterValue("ceiling")->load() + 1.f) < .02f, "undo skipped ceiling");
    require(std::abs(undo.parameters.getRawParameterValue("threshold")->load() + 3.f) < .02f, "undo reverted both gestures");
    undo.applyPreset(1);
    require(std::abs(undo.parameters.getRawParameterValue("threshold")->load() + 6.5f) < .02f, "preset threshold");
    undo.undo();
    require(std::abs(undo.parameters.getRawParameterValue("threshold")->load() + 3.f) < .02f,
            "preset apply was not a single undo step");

    WallProcessor ab;
    set(ab, "threshold", -5.f);
    ab.toggleAB();
    require(ab.activeAB() == 1, "toggle did not leave A");
    require(std::abs(ab.parameters.getRawParameterValue("threshold")->load()) < .02f, "B did not start from the other slot");
    ab.toggleAB();
    require(ab.activeAB() == 0, "toggle did not return to A");
    require(std::abs(ab.parameters.getRawParameterValue("threshold")->load() + 5.f) < .02f, "A was overwritten");
    set(ab, "threshold", -2.f);
    ab.copyAB(0, 1);
    require(ab.activeAB() == 0, "copy changed the audible slot");
    ab.toggleAB();
    require(std::abs(ab.parameters.getRawParameterValue("threshold")->load() + 2.f) < .02f, "A was not copied to B");
    set(ab, "threshold", -8.f);
    ab.copyAB(1, 0);
    ab.toggleAB();
    require(std::abs(ab.parameters.getRawParameterValue("threshold")->load() + 8.f) < .02f, "B was not copied to A");
    juce::MemoryBlock abState;
    ab.getStateInformation(abState);
    WallProcessor abRestored;
    abRestored.setStateInformation(abState.getData(), int(abState.getSize()));
    require(abRestored.activeAB() == ab.activeAB(), "A/B slot was not saved");
    require(std::abs(abRestored.parameters.getRawParameterValue("threshold")->load() + 8.f) < .02f, "A/B state lost the live slot");

    auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("openWallLimiter-suite-test");
    root.deleteRecursively();
    auto dir = root.getChildFile("Presets");
    suite::PresetLibrary library(dir, {"Factory A", "Factory B"});
    const auto home = suite::PresetLibrary::defaultDirectory("openWallLimiter");
    require(home.getFileName() == "Presets", "preset folder name");
    require(home.getParentDirectory().getFileName() == "openWallLimiter", "plugin preset folder");
    require(home.getParentDirectory().getParentDirectory().getFileName() == "Alan Marcero", "documents folder");
    WallProcessor presetProcessor;
    set(presetProcessor, "threshold", -4.f);
    const auto saved = presetProcessor.captureParameters();
    require(!library.save("Factory A", saved), "factory preset was overwritten");
    require(library.save("Mine", saved), "user preset was not saved");
    require(!library.remove("Factory A") && !library.rename("Factory A", "Other"), "factory preset was edited");
    library.setFavorite("Factory A", true);
    library.setFavorite("Mine", true);
    const auto all = library.entries("");
    require(all.size() == 3 && all[0].name == "Factory A" && all[0].favorite, "favourite order");
    require(all[1].name == "Mine" && all[1].favorite && !all[1].factory, "user favourite");
    require(all[2].name == "Factory B" && !all[2].favorite, "remaining factory preset");
    require(library.entries("mine").size() == 1, "preset search");
    require(library.rename("Mine", "Yours") && library.loadUser("Yours").getSize() > 0, "rename failed");
    require(library.remove("Yours") && library.entries("you").empty(), "delete failed");
    require(library.save("Club", saved), "second user preset");
    suite::PresetBrowser browser(library);
    browser.refresh();
    require(browser.rowCount() == 3, "browser row count");
    browser.setSearchText("club");
    require(browser.rowCount() == 1, "browser search");
    set(presetProcessor, "threshold", -1.f);
    require(presetProcessor.loadParameterState(library.loadUser("Club")), "user preset did not load");
    require(std::abs(presetProcessor.parameters.getRawParameterValue("threshold")->load() + 4.f) < .02f,
            "user preset lost its threshold");
    presetProcessor.undo();
    require(std::abs(presetProcessor.parameters.getRawParameterValue("threshold")->load() + 1.f) < .02f,
            "user preset load was not undoable");
    root.deleteRecursively();
    std::cout << "PASS programs, undo, A/B, user presets\n";
}
inline void typedValues() {
    WallProcessor p;
    const auto text = [&](const char *id, float value) {
        auto *parameter = p.parameters.getParameter(id);
        return parameter->getText(parameter->convertTo0to1(value), 0);
    };
    const auto typed = [&](const char *id, const juce::String &entry) {
        auto *parameter = p.parameters.getParameter(id);
        return parameter->convertFrom0to1(parameter->getValueForText(entry));
    };
    int floats = 0;
    std::ranges::for_each(p.getParameters(), [&](juce::AudioProcessorParameter *parameter) {
        auto *number = dynamic_cast<juce::AudioParameterFloat *>(parameter);
        if (number == nullptr)
            return;
        ++floats;
        const auto exact = number->paramID.toStdString() + ": typed display text does not return the value";
        std::ranges::for_each(std::array{0.f, 1.f, parameter->getDefaultValue()}, [&](float position) {
            const float back = parameter->getValueForText(parameter->getText(position, 0));
            require(std::abs(back - position) < 1.0e-4f, exact.c_str());
        });
        const auto stable = number->paramID.toStdString() + ": display text changes after typing it";
        std::ranges::for_each(std::array{.25f, .5f, .75f}, [&](float position) {
            const auto shown = parameter->getText(position, 0);
            require(parameter->getText(parameter->getValueForText(shown), 0) == shown, stable.c_str());
        });
    });
    require(floats == 14, "float parameter count changed");
    require(text("threshold", 0.f) == "0.0 dB" && text("threshold", -.02f) == "0.0 dB", "negative zero dB");
    require(text("input_gain", 3.f) == "+3.0 dB" && text("tone_tilt", -2.5f) == "-2.5 dB", "signed dB");
    require(text("ceiling", -1.f) == "-1.0 dB" && text("ceiling", -.15f) == "-0.15 dB", "ceiling dB");
    require(text("highpass_hz", 20.f) == "20 Hz" && text("lowpass_hz", 20000.f) == "20.00 kHz", "frequency");
    require(text("clip", 55.f) == "55%" && text("stereo_width", 125.f) == "125%", "whole percent");
    require(text("release", 1.f) == "1.0x" && text("release", 1.35f) == "1.35x", "release multiple");
    require(text("lookahead_ms", 5.f) == "5.0 ms" && text("lookahead_ms", 10.f) == "10 ms", "lookahead time");
    require(text("lufs_target", -14.f) == "-14.0 LUFS", "loudness target");
    require(std::abs(typed("clip", "50") - 50.f) < .01f && std::abs(typed("clip", "50%") - 50.f) < .01f, "percent");
    require(std::abs(typed("lowpass_hz", "20k") - 20000.f) < 1.f, "k suffix");
    require(std::abs(typed("lowpass_hz", "12.5 kHz") - 12500.f) < 1.f, "kHz suffix");
    require(std::abs(typed("highpass_hz", "35 Hz") - 35.f) < .01f, "Hz suffix");
    require(std::abs(typed("lookahead_ms", "0.0025 s") - 2.5f) < .01f, "s suffix");
    require(std::abs(typed("lookahead_ms", "2.5 ms") - 2.5f) < .01f, "ms suffix");
    require(std::abs(typed("threshold", "-6.5 dB") - typed("threshold", "-6.5")) < .001f, "dB suffix");
    std::cout << "PASS typed values and display formats\n";
}
inline void editorSuite(WallProcessor &processor, juce::AudioProcessorEditor &editor) {
    auto *lookahead = dynamic_cast<juce::Slider *>(find(editor, "lookahead_ms"));
    require(lookahead != nullptr, "lookahead slider missing");
    require(lookahead->getTooltip().contains("128-sample true-peak guard"), "tooltip omits the true-peak guard");
    require(!lookahead->getTooltip().contains("fixed at 5 ms"), "tooltip still says latency is fixed at 5 ms");
    require(find(editor, "undo") && find(editor, "redo") && find(editor, "ab_a") && find(editor, "ab_b"),
            "suite buttons missing");
    require(find(editor, "ab_a_to_b") && find(editor, "ab_b_to_a") && find(editor, "preset_library"),
            "A/B copy or library button missing");
    require(find(editor, "lufs_mode") && find(editor, "lufs_target") && find(editor, "lufs_match"),
            "loudness target controls missing");
    const float before = processor.parameters.getRawParameterValue("threshold")->load();
    auto *threshold = processor.parameters.getParameter("threshold");
    threshold->beginChangeGesture();
    threshold->setValueNotifyingHost(threshold->convertTo0to1(-7.5f));
    threshold->endChangeGesture();
    const auto command = juce::ModifierKeys::commandModifier;
    require(editor.keyPressed(juce::KeyPress('z', command, 0)), "Cmd/Ctrl+Z was ignored");
    require(std::abs(processor.parameters.getRawParameterValue("threshold")->load() - before) < .02f,
            "keyboard undo missed");
    require(editor.keyPressed(juce::KeyPress('z', command | juce::ModifierKeys::shiftModifier, 0)),
            "Shift+Cmd/Ctrl+Z was ignored");
    require(std::abs(processor.parameters.getRawParameterValue("threshold")->load() + 7.5f) < .02f,
            "keyboard redo missed");
    require(!editor.keyPressed(juce::KeyPress('a', command, 0)), "unrelated shortcut was consumed");
    editor.setSize(1500, 900);
    require(editor.getWidth() == 1500 && editor.getHeight() == 900, "editor rejected a free aspect ratio");
    auto *undo = find(editor, "undo");
    require(undo->getX() > 336 && undo->getRight() < editor.getWidth(), "resized layout did not scale");
    editor.setSize(100, 100);
    require(editor.getWidth() == WallProcessor::minEditorWidth && editor.getHeight() == WallProcessor::minEditorHeight,
            "editor shrank below the readable minimum");
    editor.setSize(WallProcessor::defaultEditorWidth, WallProcessor::defaultEditorHeight);
    require(processor.storedEditorWidth() == WallProcessor::defaultEditorWidth
                && processor.storedEditorHeight() == WallProcessor::defaultEditorHeight,
            "editor size was not stored");
    std::cout << "PASS editor latency tooltip, keys, resize\n";
}
} // namespace checks
