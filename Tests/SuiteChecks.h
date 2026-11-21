#pragma once
#include "HostChecks.h"
#include "LoudnessTarget.h"
#include "PluginEditor.h"
#include "TruePeakGuard.h"
#include "suite/PresetBrowser.h"
#include "suite/PresetLibrary.h"
#include <cmath>
#include <juce_dsp/juce_dsp.h>
#include <ranges>
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
    require(presetProcessor.loadParameterState(library.loadUser("Club"), "Club"), "user preset did not load");
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
    require(std::abs(typed("clip", "50") - 50.f) < .01f && std::abs(typed("clip", "50%") - 50.f) < .01f,
            "percent");
    require(std::abs(typed("lowpass_hz", "20k") - 20000.f) < 1.f, "k suffix");
    require(std::abs(typed("lowpass_hz", "12.5 kHz") - 12500.f) < 1.f, "kHz suffix");
    require(std::abs(typed("highpass_hz", "35 Hz") - 35.f) < .01f, "Hz suffix");
    require(std::abs(typed("lookahead_ms", "0.0025 s") - 2.5f) < .01f, "s suffix");
    require(std::abs(typed("lookahead_ms", "2.5 ms") - 2.5f) < .01f, "ms suffix");
    require(std::abs(typed("threshold", "-6.5 dB") - typed("threshold", "-6.5")) < .001f, "dB suffix");
    std::cout << "PASS typed values and display formats\n";
}
inline void meters() {
    WallProcessor p;
    neutral(p);
    set(p, "threshold", -12.f);
    p.prepareToPlay(48000, 256);
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
    auto burst = tone(48000, 48000, 1000, .9f);
    std::fill(burst.begin() + 2048, burst.end(), 0.f);
    render(p, burst, 256);
    require(p.reductionDb.load() > 6.f, "gain reduction meter lost the burst before the editor read it");
    const auto tick = [&] { dynamic_cast<juce::Timer &>(*editor).timerCallback(); };
    const auto shown = [&] {
        const auto handler = find(*editor, "gain_reduction")->createAccessibilityHandler();
        return handler->getValueInterface()->getCurrentValueAsString().getFloatValue();
    };
    tick();
    const float held = shown();
    require(held > 6.f, "gain reduction readout missed the burst");
    require(p.reductionDb.load() <= WallProcessor::noReading, "editor read did not reset the peak");
    std::ranges::for_each(std::views::iota(0, 5), [&](int) {
        render(p, std::vector<float>(512, 0.f), 256);
        tick();
    });
    require(std::abs(shown() - held) < .05f, "gain reduction readout did not hold its peak");
    std::ranges::for_each(std::views::iota(0, 30), [&](int) {
        render(p, std::vector<float>(512, 0.f), 256);
        tick();
    });
    require(shown() < 1.f, "gain reduction readout held its peak for more than one second");
    std::cout << "PASS meter peaks between editor reads, one-second hold\n";
}
inline void monitoringMeters() {
    struct Reading {
        float integrated, truePeak, output;
    };
    const auto measure = [](const char *monitor) {
        WallProcessor p;
        neutral(p);
        set(p, "threshold", -9.f);
        set(p, "ceiling", -1.f);
        if (monitor != nullptr)
            set(p, monitor, 1.f);
        p.prepareToPlay(48000, 256);
        render(p, tone(48000, 48000, 1000, .5f));
        return Reading{p.integratedLufs.load(), p.truePeakDb.load(), p.outputDb.load()};
    };
    const auto processed = measure(nullptr);
    std::ranges::for_each(std::array{"delta", "bypass", "gain_match"}, [&](const char *monitor) {
        const auto monitored = measure(monitor);
        const auto message = std::string(monitor) + " changed what the meters read";
        require(std::abs(monitored.integrated - processed.integrated) < .01f
                    && std::abs(monitored.truePeak - processed.truePeak) < .01f
                    && std::abs(monitored.output - processed.output) < .01f,
                message.c_str());
    });
    WallProcessor p;
    neutral(p);
    p.prepareToPlay(48000, 256);
    render(p, tone(24000, 48000, 1000, .5f));
    render(p, std::vector<float>(4800, 0.f));
    require(p.truePeakDb.load() > -10.f, "true peak max did not hold after the audio stopped");
    p.resetTruePeak();
    render(p, std::vector<float>(512, 0.f));
    require(p.truePeakDb.load() < -99.f, "true peak max did not reset");
    require(p.integratedLufs.load() > -99.f, "true peak reset also cleared loudness");
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
    auto *banner = dynamic_cast<juce::Label *>(find(*editor, "monitoring_banner"));
    const auto tick = [&] { dynamic_cast<juce::Timer &>(*editor).timerCallback(); };
    require(!banner->isVisible(), "monitoring banner shows with monitoring off");
    set(p, "delta", 1.f);
    tick();
    require(banner->isVisible() && banner->getText().contains("Delta"), "monitoring banner missing for Delta");
    set(p, "delta", 0.f);
    tick();
    require(!banner->isVisible(), "monitoring banner stayed after Delta turned off");
    require(find(*editor, "delta")->findColour(juce::TextButton::buttonOnColourId)
                != find(*editor, "highpass_on")->findColour(juce::TextButton::buttonOnColourId),
            "monitoring toggles light like processing toggles");
    std::cout << "PASS meters ignore monitoring, true peak reset, monitoring banner\n";
}
inline void layout(juce::AudioProcessorEditor &editor) {
    auto *content = find(editor, "content");
    require(content != nullptr, "content component missing");
    std::vector<juce::Component *> shown;
    std::ranges::copy_if(content->getChildren(), std::back_inserter(shown),
                         [](juce::Component *child) { return child->isVisible(); });
    std::ranges::for_each(shown, [&](juce::Component *child) {
        const auto id = child->getComponentID().toStdString();
        const auto overlap = id + " overlaps another control";
        require(std::ranges::none_of(shown, [&](juce::Component *other) {
                    return other != child && other->getBounds().intersects(child->getBounds());
                }),
                overlap.c_str());
        const auto small = id + " is smaller than 40 px";
        if (dynamic_cast<juce::Button *>(child) || dynamic_cast<juce::ComboBox *>(child))
            require(child->getHeight() >= 40 && child->getWidth() >= 40, small.c_str());
        const auto dial = id + " dial is smaller than 64 px";
        if (auto *slider = dynamic_cast<juce::Slider *>(child); slider != nullptr && slider->isRotary())
            require(slider->getWidth() >= 64 && slider->getHeight() - slider->getTextBoxHeight() >= 64,
                    dial.c_str());
    });
    auto *library = dynamic_cast<juce::Button *>(find(editor, "preset_library"));
    library->setToggleState(true, juce::sendNotificationSync);
    auto *browser = find(editor, "preset_browser");
    require(browser->isVisible(), "library did not open");
    std::ranges::for_each(browser->getChildren(), [](juce::Component *child) {
        if (dynamic_cast<juce::Button *>(child))
            require(child->getHeight() >= 36, "library button is shorter than 36 px");
    });
    library->setToggleState(false, juce::sendNotificationSync);
    std::cout << "PASS layout: no overlaps, 40 px targets, 64 px dials, 36 px library buttons\n";
}
inline void accessibility(juce::AudioProcessorEditor &editor) {
    std::ranges::for_each(find(editor, "content")->getChildren(), [](juce::Component *child) {
        const auto id = child->getComponentID().toStdString();
        const auto untitled = id + " has no spoken title";
        if (dynamic_cast<juce::Slider *>(child) || dynamic_cast<juce::ComboBox *>(child)
            || dynamic_cast<juce::Button *>(child))
            require(child->getTitle().isNotEmpty(), untitled.c_str());
        const auto unfocusable = id + " cannot take keyboard focus";
        if (dynamic_cast<juce::Slider *>(child) && child->isEnabled())
            require(child->getWantsKeyboardFocus(), unfocusable.c_str());
    });
    require(find(editor, "threshold")->getTitle() == "Threshold", "slider title is not the host name");
    require(find(editor, "ab_a_to_b")->getTitle() == "Copy A to B"
                && find(editor, "ab_b_to_a")->getTitle() == "Copy B to A",
            "A/B copy buttons have no spoken title");
    std::ranges::for_each(std::array{"gain_reduction", "short_term", "integrated", "true_peak_reading",
                                     "input_level", "output_level"},
                          [&](const char *id) {
                              const auto handler = find(editor, id)->createAccessibilityHandler();
                              const auto *value = handler->getValueInterface();
                              require(handler->getRole() == juce::AccessibilityRole::staticText
                                          && handler->getTitle().isNotEmpty() && value != nullptr
                                          && value->getCurrentValueAsString().isNotEmpty(),
                                      "meter readout has no accessible title or value");
                          });
    std::cout << "PASS spoken titles, focusable sliders, accessible readouts\n";
}
inline void tooltips(WallProcessor &processor, juce::AudioProcessorEditor &editor) {
    std::ranges::for_each(find(editor, "content")->getChildren(), [](juce::Component *child) {
        auto *slider = dynamic_cast<juce::Slider *>(child);
        if (slider == nullptr)
            return;
        const auto id = slider->getComponentID();
        const auto tip = slider->getTooltip();
        const bool boxMatches = std::ranges::all_of(slider->getChildren(), [&](juce::Component *part) {
            auto *box = dynamic_cast<juce::Label *>(part);
            return box == nullptr || box->getTooltip() == tip;
        });
        const auto message = id.toStdString() + " tooltip lacks its name and range, or the value box differs";
        require(tip.startsWith(slider->getTitle() + ": ") && tip.contains(" to ") && !tip.contains(id + ":")
                    && boxMatches,
                message.c_str());
    });
    set(processor, "mode", 0.f);
    set(processor, "lookahead_ms", 10.f);
    dynamic_cast<juce::Timer &>(editor).timerCallback();
    auto *modern = dynamic_cast<juce::Button *>(find(editor, "mode0"));
    require(modern->getTooltip().contains("Attack 4.4 ms (44% of Lookahead)"),
            "style tooltip ignores Lookahead");
    auto *lookahead = dynamic_cast<juce::Slider *>(find(editor, "lookahead_ms"));
    require(lookahead->getTooltip().contains("4.4 ms in Modern"), "Lookahead tooltip ignores the style");
    set(processor, "lookahead_ms", 5.f);
    std::cout << "PASS tooltips from host names, ranges and DSP times\n";
}
inline void inactiveControls(WallProcessor &processor, juce::AudioProcessorEditor &editor) {
    const auto tick = [&] { dynamic_cast<juce::Timer &>(editor).timerCallback(); };
    const std::array dependent{"highpass_hz", "highpass_slope", "clip", "lufs_target", "oversampling"};
    set(processor, "highpass_on", 0.f);
    set(processor, "mode", float(wall::Mode::Safe));
    set(processor, "lufs_mode", 0.f);
    set(processor, "true_peak", 1.f);
    set(processor, "oversampling", 0.f);
    tick();
    std::ranges::for_each(dependent, [&](const char *id) {
        auto *control = find(editor, id);
        const auto message = std::string(id) + " does not look inactive while it has no effect";
        require(control->isEnabled() && std::abs(control->getAlpha() - .4f) < .01f, message.c_str());
    });
    auto *highpass = dynamic_cast<juce::Slider *>(find(editor, "highpass_hz"));
    require(highpass->getTooltip().contains("Turn on High pass"),
            "inactive filter tooltip does not say what turns it on");
    require(dynamic_cast<juce::Slider *>(find(editor, "clip"))->getTooltip().contains("No effect in Safe"),
            "Clip tooltip does not explain Safe");
    auto *oversampling = dynamic_cast<juce::ComboBox *>(find(editor, "oversampling"));
    require(oversampling->getText() == "4x (True peak)", "Oversampling hides the rate True peak forces");
    set(processor, "highpass_on", 1.f);
    set(processor, "mode", 0.f);
    set(processor, "lufs_mode", 3.f);
    set(processor, "true_peak", 0.f);
    tick();
    std::ranges::for_each(dependent, [&](const char *id) {
        const auto message = std::string(id) + " stayed dimmed after it took effect";
        require(std::abs(find(editor, id)->getAlpha() - 1.f) < .01f, message.c_str());
    });
    require(oversampling->getText() == "Off", "Oversampling kept the True peak text");
    set(processor, "lufs_mode", 0.f);
    set(processor, "true_peak", 1.f);
    set(processor, "oversampling", 2.f);
    const auto coverage = [](juce::Component &control) {
        const auto image = control.createComponentSnapshot(control.getLocalBounds());
        double total = 0;
        std::ranges::for_each(std::views::iota(0, image.getHeight()), [&](int y) {
            std::ranges::for_each(std::views::iota(0, image.getWidth()),
                                  [&](int x) { total += image.getPixelAt(x, y).getFloatAlpha(); });
        });
        return total;
    };
    WallLook look;
    juce::Slider knob(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
    juce::TextButton button("Undo");
    juce::ComboBox choices;
    choices.addItem("Off", 1);
    choices.setSelectedId(1);
    const std::array<juce::Component *, 3> disabled{&knob, &button, &choices};
    std::ranges::for_each(disabled, [&](juce::Component *control) {
        control->setLookAndFeel(&look);
        control->setBounds(0, 0, 80, 80);
        const double enabled = coverage(*control);
        control->setEnabled(false);
        require(coverage(*control) < .6 * enabled, "the look-and-feel does not dim a disabled control");
        control->setLookAndFeel(nullptr);
    });
    std::cout << "PASS inactive controls dimmed and explained, disabled controls dimmed\n";
}
inline void presetLabels() {
    const juce::StringArray sessionIds{"dither", "gain_match", "delta", "bypass", "lufs_mode", "lufs_target"};
    WallProcessor fresh, scrambled, untouched;
    const auto scramble = [](juce::AudioProcessorParameter *parameter) {
        parameter->setValueNotifyingHost(.37f);
    };
    std::ranges::for_each(scrambled.getParameters(), scramble);
    std::ranges::for_each(untouched.getParameters(), scramble);
    std::ranges::for_each(std::views::iota(0, fresh.getNumPrograms()), [&](int index) {
        fresh.applyPreset(index);
        scrambled.applyPreset(index);
        const auto parameters = fresh.getParameters();
        const bool same = std::ranges::all_of(parameters, [&](juce::AudioProcessorParameter *parameter) {
            const auto &id = dynamic_cast<juce::AudioProcessorParameterWithID &>(*parameter).paramID;
            const float other = scrambled.parameters.getParameter(id)->getValue();
            if (sessionIds.contains(id))
                return std::abs(other - untouched.parameters.getParameter(id)->getValue()) < 1.0e-6f;
            return std::abs(parameter->getValue() - other) < 1.0e-6f;
        });
        const auto message = std::string(WallProcessor::factoryPresets()[size_t(index)].name)
                           + " depends on earlier settings or reset a session setting";
        require(same, message.c_str());
    });
    WallProcessor p;
    require(p.presetLabel() == "Uplifting / Init Clean", "a new instance does not name its preset");
    p.applyPreset(4);
    require(p.presetLabel() == "Uplifting / Euphoria Wall", "the label does not name the loaded preset");
    const auto edit = [&](const char *id, float value) {
        auto *parameter = p.parameters.getParameter(id);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        parameter->endChangeGesture();
    };
    edit("bypass", 1.f);
    require(p.presetLabel() == "Uplifting / Euphoria Wall", "a session setting marked the preset edited");
    edit("threshold", -3.f);
    require(p.presetLabel() == "Uplifting / Euphoria Wall *", "an edit did not mark the preset");
    juce::MemoryBlock session;
    p.getStateInformation(session);
    WallProcessor restored;
    restored.setStateInformation(session.getData(), int(session.getSize()));
    require(restored.presetLabel() == "Uplifting / Euphoria Wall *", "the session lost the preset label");
    p.undo();
    require(p.presetLabel() == "Uplifting / Euphoria Wall", "undoing the only edit kept the mark");
    p.undo();
    p.undo();
    require(p.presetLabel() == "Uplifting / Init Clean", "undoing a preset load kept its name");
    p.redo();
    require(p.presetLabel() == "Uplifting / Euphoria Wall", "redoing a preset load lost its name");
    p.toggleAB();
    require(p.presetLabel() == "Uplifting / Init Clean", "slot B shows the preset of slot A");
    p.toggleAB();
    require(p.presetLabel() == "Uplifting / Euphoria Wall", "slot A lost its preset label");
    WallProcessor source;
    set(p, "dither", 2.f);
    require(p.loadParameterState(source.captureParameters(), "Club"), "user preset did not load");
    require(p.presetLabel() == "Club", "the label does not name the user preset");
    require(std::lround(p.parameters.getRawParameterValue("dither")->load()) == 2,
            "a user preset reset Dither");
    p.markPresetSaved("Mine");
    require(p.presetLabel() == "Mine", "saving did not rename the label");
    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(p.createEditor());
    auto *box = dynamic_cast<juce::ComboBox *>(find(*editor, "presets"));
    p.applyPreset(4);
    edit("threshold", -3.f);
    dynamic_cast<juce::Timer &>(*editor).timerCallback();
    require(box->getText() == "Uplifting / Euphoria Wall *", "the preset box does not show the edit mark");
    box->setSelectedId(5, juce::sendNotificationSync);
    require(std::abs(p.parameters.getRawParameterValue("threshold")->load() + 8.8f) < .02f,
            "picking the shown preset again did not reload it");
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    auto root = temp.getChildFile("openWallLimiter-favourites");
    root.deleteRecursively();
    const juce::StringArray factoryNames{"Uplifting / Init Clean", "Bass & Breaks / DnB 174"};
    suite::PresetLibrary library(root.getChildFile("Presets"), factoryNames);
    library.setFavorite("Bass & Breaks / DnB 174", true);
    const auto entries = library.entries("");
    require(entries.front().name == "Bass & Breaks / DnB 174" && entries.front().favorite,
            "Favorite does nothing on a factory preset whose name has / or &");
    root.deleteRecursively();
    std::cout << "PASS preset label per slot and session, session settings kept, factory favourites\n";
}
inline void slotHistories() {
    WallProcessor p;
    const auto edit = [&](float value) {
        auto *threshold = p.parameters.getParameter("threshold");
        threshold->beginChangeGesture();
        threshold->setValueNotifyingHost(threshold->convertTo0to1(value));
        threshold->endChangeGesture();
    };
    const auto threshold = [&] { return p.parameters.getRawParameterValue("threshold")->load(); };
    edit(-4.f);
    p.toggleAB();
    require(!p.canUndo(), "slot B shows the undo history of slot A");
    edit(-7.f);
    p.toggleAB();
    require(p.canUndo() && std::abs(threshold() + 4.f) < .02f, "slot A lost its undo history");
    p.undo();
    require(std::abs(threshold()) < .02f, "undo in slot A applied a state from slot B");
    p.redo();
    p.toggleAB();
    p.undo();
    require(std::abs(threshold()) < .02f && p.canRedo(), "slot B lost its own undo history");
    p.copyAB(0, 1);
    require(!p.canUndo() && !p.canRedo(), "copying into a slot kept its old undo history");
    std::cout << "PASS one undo history per A/B slot\n";
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
    auto *ceiling = dynamic_cast<juce::Slider *>(find(editor, "ceiling"));
    require(std::abs(ceiling->valueToProportionOfLength(-3.0) - .5) < .01,
            "Ceiling travel is not centred on -3 dB");
    require(ceiling->getPositionOfValue(-1.0) - ceiling->getPositionOfValue(0.0) >= 20.f,
            "the top 1 dB of Ceiling travel is shorter than 20 px");
    require(processor.parameters.getParameter("ceiling")->getNormalisableRange().skew == 1.f,
            "Ceiling parameter skew changed");
    require(ceiling->getVelocityModeIsSwappable() && !ceiling->getVelocityBasedMode(),
            "Shift-drag fine mode missing");
    const auto hostName = [&](const char *id) { return processor.parameters.getParameter(id)->getName(64); };
    std::ranges::for_each(std::array{"release", "clip", "upward", "stereo_link", "release_link", "lookahead_ms",
                                     "input_gain", "stereo_width", "tone_tilt"},
                          [&](const char *id) {
                              const auto width = juce::GlyphArrangement::getStringWidth(
                                  juce::Font(juce::FontOptions(12.f)), hostName(id));
                              require(width <= 74.f, "knob caption is wider than its 74 px column");
                          });
    std::ranges::for_each(std::array{"highpass_on", "lowpass_on", "true_peak", "gain_match", "delta", "bypass"},
                          [&](const char *id) {
                              auto *toggle = dynamic_cast<juce::Button *>(find(editor, id));
                              require(toggle->getButtonText() == hostName(id), "toggle text is not the host name");
                          });
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
    auto *content = find(editor, "content");
    const float scale = 900.f / float(WallProcessor::defaultEditorHeight);
    const auto scaledUndo = editor.getLocalArea(undo, undo->getLocalBounds().toFloat());
    require(std::abs(scaledUndo.getWidth() - 64.f * scale) < .5f
                && std::abs(scaledUndo.getHeight() - 40.f * scale) < .5f,
            "resized layout did not scale x and y by one factor");
    const auto scaledContent = editor.getLocalArea(content, content->getLocalBounds().toFloat());
    require(std::abs(scaledContent.getX() - (float(editor.getWidth()) - scaledContent.getRight())) < .5f,
            "scaled layout is not centred");
    editor.setSize(100, 100);
    require(editor.getWidth() == WallProcessor::minEditorWidth && editor.getHeight() == WallProcessor::minEditorHeight,
            "editor shrank below the readable minimum");
    require(editor.getLocalArea(undo, undo->getLocalBounds().toFloat()).getHeight() >= 35.99f,
            "buttons are shorter than 36 px at the minimum size");
    editor.setSize(WallProcessor::defaultEditorWidth, WallProcessor::defaultEditorHeight);
    require(processor.storedEditorWidth() == WallProcessor::defaultEditorWidth
                && processor.storedEditorHeight() == WallProcessor::defaultEditorHeight,
            "editor size was not stored");
    std::cout << "PASS editor latency tooltip, keys, resize\n";
}
} // namespace checks
