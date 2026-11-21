#include "PluginEditor.h"
#include "BinaryData.h"
#include "LoudnessTarget.h"
#include "suite/PresetBrowser.h"
#include "suite/PresetLibrary.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace {
const juce::Colour ink{0xff0d1317}, panel{0xff152026}, brass{0xffffbe3b}, paper{0xffe6f0f5},
    muted{0xff869ca8}, blue{0xff48f0c8}, red{0xffef7353};
juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
}
void text(juce::Graphics &g, const juce::String &s, juce::Rectangle<int> r, float size,
          juce::Colour color = paper, int justify = juce::Justification::centredLeft) {
    g.setColour(color);
    g.setFont(font(size));
    g.drawText(s, r, justify);
}
juce::String reading(float v, const char *unit) {
    return std::isfinite(v) && v > -99 ? wall::fixed(v, 1) + unit : juce::String("--") + unit;
}
void box(juce::Graphics &g, juce::Rectangle<float> r) {
    g.setColour(panel.withAlpha(.94f));
    g.fillRoundedRectangle(r, 6);
    g.setColour(juce::Colour(0xff283842));
    g.drawRoundedRectangle(r, 6, 1);
}
} // namespace
WallLook::WallLook() {
    setColour(juce::Slider::textBoxTextColourId, paper);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, panel);
    setColour(juce::ComboBox::textColourId, paper);
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff283842));
    setColour(juce::ComboBox::arrowColourId, brass);
    setColour(juce::PopupMenu::backgroundColourId, panel);
    setColour(juce::PopupMenu::textColourId, paper);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, brass);
    setColour(juce::PopupMenu::highlightedTextColourId, ink);
    setColour(juce::TextButton::textColourOffId, muted);
    setColour(juce::TextButton::textColourOnId, ink);
    setColour(juce::TooltipWindow::backgroundColourId, panel);
    setColour(juce::TooltipWindow::textColourId, paper);
    setColour(juce::TooltipWindow::outlineColourId, brass);
}
juce::Font WallLook::getTextButtonFont(juce::TextButton&, int) { return font(14, true); }
void WallLook::drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &, bool hover,
                                    bool down) {
    const bool on = b.getToggleState();
    auto r = b.getLocalBounds().toFloat().reduced(.5f);
    g.setColour(on ? brass : juce::Colour(hover || down ? 0xff1e2c33 : 0xff152026));
    g.fillRoundedRectangle(r, 4);
    g.setColour(on ? brass.brighter(.1f) : juce::Colour(0xff283842));
    g.drawRoundedRectangle(r, 4, 1);
    if (b.hasKeyboardFocus(true)) {
        g.setColour(paper);
        g.drawRoundedRectangle(r.reduced(2), 3, 1);
    }
}
void WallLook::drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float pos, float start,
                                float end, juce::Slider &) {
    auto r = juce::Rectangle<float>(float(x), float(y), float(w), float(h)).reduced(9);
    auto centre = r.getCentre();
    float radius = std::min(r.getWidth(), r.getHeight()) * .5f;
    juce::Path track;
    track.addCentredArc(centre.x, centre.y, radius, radius, 0, start, end, true);
    g.setColour(juce::Colour(0xff283842));
    g.strokePath(track, juce::PathStrokeType(3));
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, radius, radius, 0, start, start + pos * (end - start), true);
    g.setColour(brass);
    g.strokePath(arc, juce::PathStrokeType(3));
    auto circle = juce::Rectangle<float>(radius * 1.62f, radius * 1.62f).withCentre(centre);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff30424c), circle.getTopLeft(),
                                           juce::Colour(0xff10181d), circle.getBottomRight(), false));
    g.fillEllipse(circle);
    g.setColour(juce::Colour(0xff3c525f));
    g.drawEllipse(circle, 1);
    g.setColour(ink);
    g.drawEllipse(circle.reduced(3), 1);
    float angle = start + pos * (end - start);
    auto a = centre.getPointOnCircumference(radius * .38f, angle);
    auto b = centre.getPointOnCircumference(radius * .68f, angle);
    g.setColour(paper);
    g.drawLine({a, b}, 2.5f);
}
void WallLook::drawLinearSlider(juce::Graphics &g, int x, int y, int w, int h, float position, float, float,
                                juce::Slider::SliderStyle style, juce::Slider &) {
    bool vertical = style == juce::Slider::LinearVertical;
    float cx = float(x) + float(w) * .5f, cy = float(y) + float(h) * .5f;
    g.setColour(juce::Colour(0xff06090a));
    if (vertical) {
        g.fillRoundedRectangle(cx - 4, float(y), 8, float(h), 4);
        g.setColour(brass.withAlpha(.6f));
        g.fillRoundedRectangle(cx - 1, position, 2, float(y + h) - position, 1);
        juce::Rectangle<float> thumb(cx - 18, position - 10, 36, 20);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffe6c58e), thumb.getTopLeft(),
                                               juce::Colour(0xff7b6240), thumb.getBottomLeft(), false));
        g.fillRoundedRectangle(thumb, 3);
        g.setColour(ink);
        g.drawHorizontalLine(int(position), cx - 12, cx + 12);
    } else {
        g.fillRoundedRectangle(float(x), cy - 3, float(w), 6, 3);
        g.setColour(brass);
        g.fillRoundedRectangle(float(x), cy - 2, position - float(x), 4, 2);
        g.fillEllipse(position - 6, cy - 6, 12, 12);
    }
}
void WallEditor::slider(const char *id, bool rotary) {
    auto s = std::make_unique<juce::Slider>();
    s->setComponentID(id);
    s->setName(id);
    s->setSliderStyle(rotary ? juce::Slider::RotaryHorizontalVerticalDrag : juce::Slider::LinearVertical);
    s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 96, 25);
    s->setDoubleClickReturnValue(true, wallProcessor.parameters.getParameter(id)->convertFrom0to1(
                                           wallProcessor.parameters.getParameter(id)->getDefaultValue()));
    s->setTooltip(juce::String(id) + ": drag to adjust, double-click to reset, or type a value.");
    content.addAndMakeVisible(*s);
    sliderAttachments.push_back(std::make_unique<SliderAttachment>(wallProcessor.parameters, id, *s));
    sliders.emplace(id, std::move(s));
}
void WallEditor::combo(const char *id, const juce::StringArray &items) {
    auto c = std::make_unique<juce::ComboBox>();
    c->setComponentID(id);
    c->addItemList(items, 1);
    content.addAndMakeVisible(*c);
    comboAttachments.push_back(std::make_unique<ComboAttachment>(wallProcessor.parameters, id, *c));
    combos.emplace(id, std::move(c));
}
void WallEditor::toggle(const char *id, const char *label) {
    auto b = std::make_unique<juce::TextButton>(label);
    b->setComponentID(id);
    b->setClickingTogglesState(true);
    content.addAndMakeVisible(*b);
    buttonAttachments.push_back(std::make_unique<ButtonAttachment>(wallProcessor.parameters, id, *b));
    buttons.emplace(id, std::move(b));
}
WallEditor::WallEditor(WallProcessor &p) : AudioProcessorEditor(p), wallProcessor(p) {
    setLookAndFeel(&look);
    content.setComponentID("content");
    content.setBounds(0, 0, WallProcessor::defaultEditorWidth, WallProcessor::defaultEditorHeight);
    addAndMakeVisible(content);
    ceilingMark = juce::Drawable::createFromImageData(BinaryData::ceilingmeter_svg, BinaryData::ceilingmeter_svgSize);
    fortress = juce::ImageCache::getFromMemory(BinaryData::fortress_png, BinaryData::fortress_pngSize);
    inputHistory.fill(-60);
    outputHistory.fill(-60);
    reductionHistory.fill(0);
    slider("threshold", false);
    slider("ceiling", false);
    slider("release", true);
    slider("clip", true);
    slider("upward", true);
    slider("stereo_link", true);
    slider("lookahead_ms", true);
    slider("release_link", true);
    slider("input_gain", true);
    slider("stereo_width", true);
    slider("tone_tilt", true);
    slider("highpass_hz", false);
    slider("lowpass_hz", false);
    for (auto id : {"highpass_hz", "lowpass_hz"})
        sliders[id]->setSliderStyle(juce::Slider::LinearHorizontal);
    combo("highpass_slope", wall::slopeNames);
    combo("lowpass_slope", wall::slopeNames);
    combo("oversampling", {"Off", "2x", "4x", "8x", "16x"});
    combo("dither", {"Off", "24-bit Shaped", "16-bit Shaped"});
    toggle("highpass_on", "High pass");
    toggle("lowpass_on", "Low pass");
    toggle("true_peak", "True peak");
    toggle("gain_match", "Gain match");
    toggle("delta", "Delta");
    toggle("bypass", "Bypass");
    buttons["true_peak"]->setTooltip("Uses at least 4x processing and a brickwall 16-phase reconstructed-peak "
                                     "lookahead guard with zero ceiling reserve. Meter is a 4x true-peak estimate.");
    buttons["delta"]->setTooltip("Listen to processed output minus the latency-aligned original. This "
                                 "monitoring signal can exceed the output ceiling.");
    buttons["gain_match"]->setTooltip("Remove threshold makeup gain for level-matched comparison.");
    sliders["stereo_link"]->setTooltip("Transient Link blends peak detection across channels before gain reduction.");
    sliders["release_link"]->setTooltip("Release Link blends each channel's recovery envelope toward shared recovery.");
    sliders["lookahead_ms"]->setTooltip(
        "Maximum transient anticipation. Each limiter mode's attack window is capped by this value. "
        "Reported host latency does not follow this knob: it is the limiter delay, the longest oversampling "
        "filter delay, and the 128-sample true-peak guard. The editor shows that latency in milliseconds and samples.");
    sliders["input_gain"]->setTooltip("Pre-limiter input drive/trim (-12 dB to +12 dB) before peak detection.");
    sliders["stereo_width"]->setTooltip("Pre-limiter Mid/Side stereo width (0% mono to 200% wide).");
    sliders["tone_tilt"]->setTooltip("Pre-limiter spectral tilt (-6 dB warm to +6 dB bright) around 1.2 kHz.");
    const std::array<const char *, 5> hints{
        "2.2 ms attack, 20 ms hold. Transients release faster than sustained limiting.",
        "4.5 ms attack, 24 ms hold, and a rounded release stage for transparent dynamics.",
        "1.2 ms attack, 18 ms hold, punchy fast release, and transient clipping.",
        "4.5 ms attack, 26 ms hold, slow release, and no intentional clipping.",
        "1.8 ms attack, 20 ms hold, and a vintage dual-stage opto recovery curve."};
    for (size_t i = 0; i < modes.size(); ++i) {
        modes[i].setButtonText(wall::modeNames[int(i)]);
        modes[i].setComponentID("mode" + juce::String(int(i)));
        modes[i].setTooltip(hints[i]);
        modes[i].onClick = [this, i] {
            auto *m = wallProcessor.parameters.getParameter("mode");
            m->beginChangeGesture();
            m->setValueNotifyingHost(m->convertTo0to1(float(i)));
            m->endChangeGesture();
            timerCallback();
        };
        content.addAndMakeVisible(modes[i]);
    }
    std::vector<juce::String> presetGroups;
    std::vector<juce::PopupMenu> presetMenus;
    std::ranges::for_each(WallProcessor::factoryPresets(), [&, id = 1](const WallPreset &preset) mutable {
        const juce::String full(preset.name);
        const auto group = full.upToFirstOccurrenceOf(" / ", false, false);
        const auto it = std::ranges::find(presetGroups, group);
        const auto idx = static_cast<size_t>(std::distance(presetGroups.begin(), it));
        if (it == presetGroups.end()) {
            presetGroups.push_back(group);
            presetMenus.emplace_back();
        }
        presetMenus[idx].addItem(id++, full);
    });
    auto *rootMenu = presets.getRootMenu();
    std::ranges::for_each(presetGroups, [&, i = size_t{0}](const juce::String &group) mutable {
        rootMenu->addSubMenu(group, presetMenus[i++]);
    });
    presets.setSelectedId(1, juce::dontSendNotification);
    presets.setComponentID("presets");
    presets.setTooltip("Choose a factory limiter preset. These 48 presets are also host programs. Use Library for saved presets.");
    presets.onChange = [this] { wallProcessor.applyPreset(presets.getSelectedItemIndex()); };
    content.addAndMakeVisible(presets);
    prevPreset.setComponentID("prev_preset");
    prevPreset.setTooltip("Previous preset");
    prevPreset.onClick = [this] {
        const int numItems = presets.getNumItems();
        if (numItems <= 0) return;
        const int cur = presets.getSelectedItemIndex();
        const int next = (cur <= 0 ? numItems : cur) - 1;
        presets.setSelectedItemIndex(next, juce::sendNotificationSync);
    };
    content.addAndMakeVisible(prevPreset);
    nextPreset.setComponentID("next_preset");
    nextPreset.setTooltip("Next preset");
    nextPreset.onClick = [this] {
        const int numItems = presets.getNumItems();
        if (numItems <= 0) return;
        const int cur = presets.getSelectedItemIndex();
        const int next = (cur + 1) % numItems;
        presets.setSelectedItemIndex(next, juce::sendNotificationSync);
    };
    content.addAndMakeVisible(nextPreset);
    reset.setComponentID("reset_meters");
    reset.onClick = [this] { wallProcessor.resetMeters(); };
    content.addAndMakeVisible(reset);
    link.setClickingTogglesState(true);
    link.setTooltip("Move threshold and ceiling together when dragging either fader.");
    content.addAndMakeVisible(link);
    combo("lufs_mode", wall::lufsModeNames);
    combos["lufs_mode"]->setTooltip("Loudness target from the BS.1770 meter. Streaming is -14 LUFS. Club is -9 LUFS.");
    slider("lufs_target", true);
    sliders["lufs_target"]->setSliderStyle(juce::Slider::LinearHorizontal);
    sliders["lufs_target"]->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    sliders["lufs_target"]->setTooltip("Custom loudness target, used when the target mode is Custom.");
    matchLoudness.setComponentID("lufs_match");
    matchLoudness.setTooltip("Add the difference between the current loudness and the target to input drive.");
    matchLoudness.onClick = [this] { wallProcessor.matchLoudnessTarget(); };
    content.addAndMakeVisible(matchLoudness);
    undoButton.setComponentID("undo");
    redoButton.setComponentID("redo");
    abA.setComponentID("ab_a");
    abB.setComponentID("ab_b");
    abAToB.setComponentID("ab_a_to_b");
    abBToA.setComponentID("ab_b_to_a");
    libraryButton.setComponentID("preset_library");
    undoButton.setTooltip("Undo the last parameter edit (Cmd/Ctrl+Z).");
    redoButton.setTooltip("Redo the last undone edit (Shift+Cmd/Ctrl+Z).");
    abA.setTooltip("Hear setup A.");
    abB.setTooltip("Hear setup B.");
    abAToB.setTooltip("Copy A onto B.");
    abBToA.setTooltip("Copy B onto A.");
    libraryButton.setTooltip("Search factory and saved presets. Factory presets stay read-only.");
    libraryButton.setClickingTogglesState(true);
    undoButton.onClick = [this] { wallProcessor.undo(); };
    redoButton.onClick = [this] { wallProcessor.redo(); };
    abA.onClick = [this] {
        if (wallProcessor.activeAB() != 0)
            wallProcessor.toggleAB();
    };
    abB.onClick = [this] {
        if (wallProcessor.activeAB() != 1)
            wallProcessor.toggleAB();
    };
    abAToB.onClick = [this] { wallProcessor.copyAB(0, 1); };
    abBToA.onClick = [this] { wallProcessor.copyAB(1, 0); };
    for (auto *button : {&undoButton, &redoButton, &abA, &abB, &abAToB, &abBToA, &libraryButton})
        content.addAndMakeVisible(button);
    juce::StringArray factoryNames;
    for (const auto &preset : WallProcessor::factoryPresets())
        factoryNames.add(preset.name);
    presetLibrary = std::make_unique<suite::PresetLibrary>(suite::PresetLibrary::defaultDirectory("openWallLimiter"),
                                                           factoryNames);
    presetBrowser = std::make_unique<suite::PresetBrowser>(*presetLibrary);
    presetBrowser->onFactory = [this](int index) { presets.setSelectedItemIndex(index, juce::sendNotificationSync); };
    presetBrowser->onUser = [this](const juce::MemoryBlock &block) { wallProcessor.loadParameterState(block); };
    presetBrowser->capture = [this] { return wallProcessor.captureParameters(); };
    content.addChildComponent(*presetBrowser);
    libraryButton.onClick = [this] {
        presetBrowser->setVisible(libraryButton.getToggleState());
        if (presetBrowser->isVisible()) {
            presetBrowser->refresh();
            presetBrowser->toFront(false);
        }
    };
    setWantsKeyboardFocus(true);
    lastThreshold = sliders["threshold"]->getValue();
    lastCeiling = sliders["ceiling"]->getValue();
    for (auto id : {"threshold", "ceiling"})
        sliders[id]->onValueChange = [this, id] {
            const bool threshold = juce::String(id) == "threshold";
            double now = sliders[id]->getValue();
            double difference = now - (threshold ? lastThreshold : lastCeiling);
            if (threshold)
                lastThreshold = now;
            else
                lastCeiling = now;
            if (link.getToggleState() && !linking && sliders[id]->isMouseButtonDown()) {
                linking = true;
                sliders[threshold ? "ceiling" : "threshold"]->setValue(
                    sliders[threshold ? "ceiling" : "threshold"]->getValue() + difference,
                    juce::sendNotificationSync);
                linking = false;
            }
        };
    const int width = wallProcessor.storedEditorWidth();
    const int height = wallProcessor.storedEditorHeight();
    setResizable(true, true);
    setResizeLimits(WallProcessor::minEditorWidth, WallProcessor::minEditorHeight, WallProcessor::maxEditorWidth,
                    WallProcessor::maxEditorHeight);
    setSize(width, height);
    timerCallback();
    startTimerHz(30);
}
bool WallEditor::keyPressed(const juce::KeyPress &key) {
    const auto command = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress('z', command | juce::ModifierKeys::shiftModifier, 0)) {
        wallProcessor.redo();
        return true;
    }
    if (key == juce::KeyPress('z', command, 0)) {
        wallProcessor.undo();
        return true;
    }
    return false;
}
WallEditor::~WallEditor() {
    stopTimer();
    setLookAndFeel(nullptr);
}
void WallEditor::timerCallback() {
    inputHistory[size_t(cursor)] = wallProcessor.inputDb.load();
    outputHistory[size_t(cursor)] = wallProcessor.outputDb.load();
    reductionHistory[size_t(cursor)] = wallProcessor.reductionDb.load();
    cursor = (cursor + 1) % int(inputHistory.size());
    const int active = int(wallProcessor.parameters.getRawParameterValue("mode")->load());
    for (size_t i = 0; i < modes.size(); ++i)
        modes[i].setToggleState(int(i) == active, juce::dontSendNotification);
    undoButton.setEnabled(wallProcessor.canUndo());
    redoButton.setEnabled(wallProcessor.canRedo());
    abA.setToggleState(wallProcessor.activeAB() == 0, juce::dontSendNotification);
    abB.setToggleState(wallProcessor.activeAB() == 1, juce::dontSendNotification);
    const int lufsMode = int(std::lround(wallProcessor.parameters.getRawParameterValue("lufs_mode")->load()));
    sliders["lufs_target"]->setEnabled(lufsMode == 3);
    matchLoudness.setEnabled(lufsMode != 0);
    if (!presets.isPopupActive() && presets.getSelectedItemIndex() != wallProcessor.getCurrentProgram())
        presets.setSelectedItemIndex(wallProcessor.getCurrentProgram(), juce::dontSendNotification);
    content.repaint(200, 260, 700, 265);
    content.repaint(40, 590, 140, 32);
    content.repaint(948, 590, 140, 32);
    content.repaint(20, 10, 56, 56);
    content.repaint(590, 688, 510, 40);
    content.repaint(24, 800, 1072, 24);
}
void WallEditor::resized() {
    const int width = std::clamp(getWidth(), WallProcessor::minEditorWidth, WallProcessor::maxEditorWidth);
    const int height = std::clamp(getHeight(), WallProcessor::minEditorHeight, WallProcessor::maxEditorHeight);
    if (width != getWidth() || height != getHeight()) {
        setSize(width, height);
        return;
    }
    wallProcessor.setStoredEditorSize(width, height);
    // Scale x and y by one factor and centre the layout. The window aspect ratio stays free.
    const float scale = std::min(float(width) / float(WallProcessor::defaultEditorWidth),
                                 float(height) / float(WallProcessor::defaultEditorHeight));
    content.setTransform(juce::AffineTransform::scale(scale).translated(
        (float(width) - float(WallProcessor::defaultEditorWidth) * scale) * .5f,
        (float(height) - float(WallProcessor::defaultEditorHeight) * scale) * .5f));
    undoButton.setBounds(336, 18, 64, 40);
    redoButton.setBounds(404, 18, 64, 40);
    abA.setBounds(476, 18, 40, 40);
    abB.setBounds(520, 18, 40, 40);
    abAToB.setBounds(568, 18, 52, 40);
    abBToA.setBounds(624, 18, 52, 40);
    prevPreset.setBounds(684, 18, 70, 40);
    presets.setBounds(758, 18, 210, 40);
    nextPreset.setBounds(972, 18, 48, 40);
    libraryButton.setBounds(1024, 18, 76, 40);
    buttons["highpass_on"]->setBounds(40, 101, 104, 40);
    sliders["highpass_hz"]->setBounds(152, 94, 202, 58);
    combos["highpass_slope"]->setBounds(368, 101, 136, 40);
    buttons["lowpass_on"]->setBounds(615, 101, 104, 40);
    sliders["lowpass_hz"]->setBounds(727, 94, 202, 58);
    combos["lowpass_slope"]->setBounds(940, 101, 136, 40);
    for (size_t i = 0; i < modes.size(); ++i)
        modes[i].setBounds(240 + int(i) * 132, 201, 124, 40);
    sliders["threshold"]->setBounds(53, 300, 110, 190);
    sliders["ceiling"]->setBounds(959, 300, 110, 190);
    link.setBounds(62, 502, 98, 40);
    sliders["release"]->setBounds(218, 560, 74, 96);
    sliders["clip"]->setBounds(294, 560, 74, 96);
    sliders["upward"]->setBounds(370, 560, 74, 96);
    sliders["stereo_link"]->setBounds(446, 560, 74, 96);
    sliders["release_link"]->setBounds(522, 560, 74, 96);
    sliders["lookahead_ms"]->setBounds(598, 560, 74, 96);
    sliders["input_gain"]->setBounds(674, 560, 74, 96);
    sliders["stereo_width"]->setBounds(750, 560, 74, 96);
    sliders["tone_tilt"]->setBounds(826, 560, 74, 96);
    combos["lufs_mode"]->setBounds(88, 688, 168, 40);
    sliders["lufs_target"]->setBounds(264, 688, 220, 40);
    matchLoudness.setBounds(492, 688, 88, 40);
    combos["oversampling"]->setBounds(122, 756, 88, 40);
    buttons["true_peak"]->setBounds(222, 756, 104, 40);
    combos["dither"]->setBounds(398, 756, 138, 40);
    buttons["gain_match"]->setBounds(640, 756, 115, 40);
    buttons["delta"]->setBounds(765, 756, 75, 40);
    buttons["bypass"]->setBounds(850, 756, 84, 40);
    reset.setBounds(949, 756, 143, 40);
    if (presetBrowser)
        presetBrowser->setBounds(690, 88, 406, 560);
    juce::AudioProcessorEditor::resized();
}
void WallEditor::drawGraph(juce::Graphics &g) {
    juce::Rectangle<float> r{240, 323, 640, 173};
    g.setColour(juce::Colour(0xff0d1214));
    g.fillRoundedRectangle(r, 4);
    const auto y = [&](float v) {
        return r.getBottom() - std::clamp((v + 48.f) / 48.f, 0.f, 1.f) * r.getHeight();
    };
    for (int dbLine = -36; dbLine <= 0; dbLine += 12) {
        g.setColour(juce::Colour(0xff283232));
        g.drawHorizontalLine(int(y(float(dbLine))), r.getX(), r.getRight());
        text(g, juce::String(dbLine), {208, int(y(float(dbLine))) - 8, 28, 16}, 10, muted,
             juce::Justification::centredRight);
    }
    for (int i = 1; i < 8; ++i) {
        g.setColour(juce::Colour(0xff20292a));
        g.drawVerticalLine(int(r.getX() + float(i) * 80), r.getY(), r.getBottom());
    }
    const auto draw = [&](const auto &history, juce::Colour colour, bool reduction) {
        juce::Path line;
        for (size_t i = 0; i < history.size(); ++i) {
            float v = history[(size_t(cursor) + i) % history.size()];
            float xx = r.getX() + float(i) * r.getWidth() / float(history.size() - 1);
            float yy = reduction ? r.getY() + std::clamp(v / 24.f, 0.f, 1.f) * r.getHeight() : y(v);
            if (i == 0)
                line.startNewSubPath(xx, yy);
            else
                line.lineTo(xx, yy);
        }
        g.setColour(colour);
        g.strokePath(line, juce::PathStrokeType(reduction ? 1.4f : 1.8f));
    };
    draw(inputHistory, blue.withAlpha(.6f), false);
    draw(outputHistory, brass, false);
    draw(reductionHistory, red, true);
    const float ceiling = wallProcessor.parameters.getRawParameterValue("ceiling")->load();
    g.setColour(brass.withAlpha(.3f));
    const float dash[]{4, 4};
    g.drawDashedLine({r.getX(), y(ceiling), r.getRight(), y(ceiling)}, dash, 2, 1);
    text(g, "Input", {240, 505, 60, 20}, 12, blue);
    text(g, "Output", {310, 505, 60, 20}, 12, brass);
    text(g, "Reduction", {390, 505, 100, 20}, 12, red);
    text(g, "8 seconds", {786, 505, 94, 20}, 12, muted, juce::Justification::centredRight);
}
void WallEditor::paint(juce::Graphics &g) {
    g.fillAll(ink);
}
void WallEditor::paintContent(juce::Graphics &g) {
    g.fillAll(ink);
    g.drawImage(fortress, {0, 62, 1120, 374});
    g.setGradientFill(juce::ColourGradient(ink.withAlpha(.05f), 0, 100, ink, 0, 410, false));
    g.fillRect(0, 62, 1120, 374);
    g.setColour(juce::Colour(0xff090c0d));
    g.fillRect(0, 0, WallProcessor::defaultEditorWidth, 73);
    g.setColour(brass.withAlpha(.3f));
    g.drawHorizontalLine(72, 0, float(WallProcessor::defaultEditorWidth));
    if (ceilingMark) {
        const float decay = 1.0f - wallProcessor.artworkPhase();
        const float pulse = decay * decay;
        const float size = 42.0f + 9.0f * pulse;
        const auto bounds = juce::Rectangle<float>(size, size).withCentre({46.0f, 36.0f});
        ceilingMark->drawWithin(g, bounds, juce::RectanglePlacement::centred, 0.76f + 0.24f * pulse);
    }
    text(g, "openWallLimiter", {80, 14, 250, 32}, 27, paper);
    text(g, "TRUE PEAK BRICKWALL LIMITER", {82, 44, 280, 18}, 10, brass);
    box(g, {24, 89, 496, 75});
    box(g, {599, 89, 497, 75});
    text(g, "Pre-limiter high-pass", {34, 170, 220, 18}, 11, muted);
    text(g, "Pre-limiter low-pass", {866, 170, 220, 18}, 11, muted, juce::Justification::centredRight);
    text(g, "Character", {35, 204, 180, 25}, 17, brass);
    box(g, {24, 255, 166, 290});
    box(g, {930, 255, 166, 290});
    text(g, "Threshold", {40, 270, 134, 25}, 17, paper, juce::Justification::centred);
    text(g, "Ceiling", {946, 270, 134, 25}, 17, paper, juce::Justification::centred);
    text(g, "Maximum output", {943, 504, 140, 20}, 11, muted, juce::Justification::centred);
    const std::array<juce::String, 4> values{reading(wallProcessor.reductionDb.load(), " dB"),
                                             reading(wallProcessor.shortLufs.load(), " LUFS"),
                                             reading(wallProcessor.integratedLufs.load(), " LUFS"),
                                             reading(wallProcessor.truePeakDb.load(), " dBTP")};
    const std::array<const char *, 4> labels{"Gain reduction", "Short-term", "Integrated", "True peak"};
    for (size_t i = 0; i < 4; ++i) {
        int x = 240 + int(i) * 168;
        text(g, labels[i], {x, 265, 164, 19}, 12, muted);
        text(g, values[i], {x, 286, 164, 28}, 22, i == 0 ? red : paper);
    }
    const float tp = wallProcessor.truePeakDb.load();
    const bool tpClip = tp > 0.0f;
    g.setColour(tpClip ? red : (tp > -1.0f ? brass : juce::Colour(0xff2d3a33)));
    g.fillEllipse(886.0f, 292.0f, 8.0f, 8.0f);
    if (tpClip) {
        g.setColour(red.withAlpha(0.4f));
        g.drawEllipse(884.0f, 290.0f, 12.0f, 12.0f, 1.2f);
    }
    drawGraph(g);
    const std::array<const char *, 9> names{"Release", "Clip", "Upward", "Peak link", "Rel link",
                                            "Lookahead", "In drive", "Width", "Tilt"};
    const std::array<const char *, 9> desc{"Recovery", "Soft clip", "Low lift", "Peak link",
                                           "Env link", "Attack cap", "Input trim", "Stereo M/S", "Tone balance"};
    for (size_t i = 0; i < names.size(); ++i) {
        const int x = 218 + int(i) * 76;
        text(g, names[i], {x, 537, 74, 20}, 12, paper, juce::Justification::centred);
        text(g, desc[i], {x, 656, 74, 17}, 9, muted, juce::Justification::centred);
    }
    text(g, "Input", {40, 570, 70, 20}, 12, muted);
    text(g, reading(wallProcessor.inputDb.load(), " dB"), {40, 592, 140, 28}, 23, blue);
    text(g, "Output", {948, 570, 100, 20}, 12, muted);
    text(g, reading(wallProcessor.outputDb.load(), " dB"), {948, 592, 140, 28}, 23, brass);
    text(g, "Target", {24, 695, 60, 24}, 12, muted);
    const int lufsMode = int(std::lround(wallProcessor.parameters.getRawParameterValue("lufs_mode")->load()));
    const float custom = wallProcessor.parameters.getRawParameterValue("lufs_target")->load();
    text(g,
         wall::loudnessGuidance(lufsMode, custom, wallProcessor.integratedLufs.load(), wallProcessor.shortLufs.load()),
         {588, 695, 510, 24}, 12, paper);
    text(g, "Oversampling", {24, 763, 98, 20}, 12, muted);
    text(g, "Dither", {348, 763, 50, 20}, 12, muted);
    g.setColour(juce::Colour(0xff283842));
    g.drawHorizontalLine(744, 24, 1096);
    text(g,
         juce::String(wallProcessor.effectiveFactor.load()) + "x active  |  "
             + wall::latencyReadout(wallProcessor.getLatencySamples(), wallProcessor.preparedSampleRate()),
         {24, 804, 620, 18}, 11, muted);
    text(g, "Alan Marcero   /   v" JucePlugin_VersionString, {760, 804, 336, 18}, 11, muted,
         juce::Justification::centredRight);
}
