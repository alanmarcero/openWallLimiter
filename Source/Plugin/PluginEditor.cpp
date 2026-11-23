#include "PluginEditor.h"
#include "BinaryData.h"
#include "LoudnessTarget.h"
#include "suite/PresetBrowser.h"
#include "suite/PresetLibrary.h"
#include <algorithm>
#include <cmath>
#include <ranges>
#include <tuple>
#include <vector>
namespace {
const juce::Colour ink{0xff0d1317}, panel{0xff152026}, brass{0xffffbe3b}, paper{0xffe6f0f5},
    muted{0xff869ca8}, blue{0xff48f0c8}, red{0xffef7353}, ledOff{0xff2d3a33};
juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
}
void text(juce::Graphics &g, const juce::String &s, juce::Rectangle<int> r, float size,
          juce::Colour color = paper, int justify = juce::Justification::centredLeft) {
    g.setColour(color);
    g.setFont(font(size));
    g.drawText(s, r, justify);
}
void pillText(juce::Graphics &g, const juce::String &s, juce::Rectangle<int> r, float size,
              juce::Colour color, int justify) {
    const float textWidth = juce::GlyphArrangement::getStringWidth(font(size), s);
    const int pillW = std::min(r.getWidth(), juce::roundToInt(textWidth) + 16);
    const int pillX = (justify & juce::Justification::right) != 0 ? r.getRight() - pillW : r.getX();
    const juce::Rectangle<float> pill(float(pillX), float(r.getY()), float(pillW), float(r.getHeight()));
    g.setColour(ink.withAlpha(.78f));
    g.fillRoundedRectangle(pill, 4.f);
    text(g, s, pill.toNearestInt(), size, color, juce::Justification::centred);
}
// Matches the style table in README.md.
constexpr std::array<const char *, 5> modeCharacters{
    "Balanced, with fast recovery for transients.", "Transparent, with a slower attack and rounded recovery.",
    "Fast recovery with more transient clipping.", "Slow recovery without intentional clipping.",
    "Steady recovery."};
// Knob order, left to right.
constexpr std::array<const char *, 9> knobIds{"release", "clip", "upward", "stereo_link", "release_link",
                                              "lookahead_ms", "input_gain", "stereo_width", "tone_tilt"};
juce::String reading(float v, const char *unit) {
    return std::isfinite(v) && v > -99 ? wall::fixed(v, 1) + unit : juce::String("--") + unit;
}
// Inactive and disabled controls draw at this opacity.
constexpr float dimAlpha = .4f;
// Draws the rest of a look-and-feel call at dimAlpha when the control is disabled.
struct DimWhenDisabled {
    DimWhenDisabled(juce::Graphics &graphics, const juce::Component &control)
        : g(graphics), dim(!control.isEnabled()) {
        if (dim)
            g.beginTransparencyLayer(dimAlpha);
    }
    ~DimWhenDisabled() {
        if (dim)
            g.endTransparencyLayer();
    }
    juce::Graphics &g;
    const bool dim;
};
void box(juce::Graphics &g, juce::Rectangle<float> r) {
    g.setColour(panel.withAlpha(.94f));
    g.fillRoundedRectangle(r, 6);
    g.setColour(juce::Colour(0xff283842));
    g.drawRoundedRectangle(r, 6, 1);
}
void drawFocusRing(juce::Graphics &g, juce::Rectangle<float> bounds) {
    g.setColour(paper);
    g.drawRoundedRectangle(bounds.reduced(1.f), 4.f, 1.f);
}
juce::String withLockSuffix(juce::String text, bool locked) {
    constexpr auto suffix = " (Locked)";
    if (text.endsWith(suffix))
        text = text.dropLastCharacters(9);
    return locked ? text + suffix : text;
}
void drawPadlock(juce::Graphics &g, juce::Rectangle<float> badge) {
    g.setColour(ink.withAlpha(.88f));
    g.fillRoundedRectangle(badge, 3.f);
    g.setColour(brass.withAlpha(.5f));
    g.drawRoundedRectangle(badge, 3.f, .8f);
    const float cx = badge.getCentreX();
    const float top = badge.getY() + 2.5f;
    juce::Path shackle;
    shackle.addArc(cx - 2.6f, top, 5.2f, 5.2f, -juce::MathConstants<float>::halfPi,
                   juce::MathConstants<float>::halfPi, true);
    g.setColour(brass);
    g.strokePath(shackle, juce::PathStrokeType(1.3f));
    const juce::Rectangle<float> body(cx - 3.5f, top + 3.8f, 7.f, 5.2f);
    g.fillRoundedRectangle(body, 1.2f);
    g.setColour(ink);
    g.fillEllipse(cx - .8f, body.getCentreY() - .8f, 1.6f, 1.6f);
}
} // namespace
Readout::Readout(const char *id, const juce::String &caption, float valueSize, const char *ledLabel)
    : ledCaption(ledLabel), size(valueSize) {
    setComponentID(id);
    setTitle(caption);
}
void Readout::show(const juce::String &shown, juce::Colour shownColour, juce::Colour shownLed) {
    if (shown == value && shownColour == colour && shownLed == led)
        return;
    value = shown;
    colour = shownColour;
    led = shownLed;
    repaint();
}
void Readout::paint(juce::Graphics &g) {
    text(g, getTitle(), getLocalBounds().withHeight(20), 14.f, paper.withAlpha(.9f));
    text(g, value, getLocalBounds().withTrimmedTop(20), size, colour);
    if (ledCaption.isNotEmpty())
        text(g, ledCaption, {getWidth() - 44, 2, 38, 16}, 11.f, muted.brighter(.15f),
             juce::Justification::centredRight);
    g.setColour(led);
    g.fillEllipse(float(getWidth()) - 14.f, 27.f, 8.f, 8.f);
}
void Readout::mouseEnter(const juce::MouseEvent &) {
    setMouseCursor(onClick != nullptr ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}
void Readout::mouseUp(const juce::MouseEvent &e) {
    if (onClick != nullptr && contains(e.getPosition()))
        onClick();
}
std::unique_ptr<juce::AccessibilityHandler> Readout::createAccessibilityHandler() {
    struct Value final : juce::AccessibilityTextValueInterface {
        explicit Value(const Readout &owner) : readout(owner) {}
        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return readout.value; }
        void setValueAsString(const juce::String &) override {}
        const Readout &readout;
    };
    juce::AccessibilityActions actions;
    if (onClick != nullptr)
        actions.addAction(juce::AccessibilityActionType::press, [this] { onClick(); });
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::staticText, std::move(actions),
        juce::AccessibilityHandler::Interfaces{std::make_unique<Value>(*this)});
}
WallLook::WallLook() {
    setColour(juce::Slider::textBoxTextColourId, paper);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineWhenEditingColourId, juce::Colour(0xff3c525f));
    setColour(juce::ComboBox::backgroundColourId, panel);
    setColour(juce::ComboBox::textColourId, paper);
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff283842));
    setColour(juce::ComboBox::arrowColourId, brass);
    setColour(juce::PopupMenu::backgroundColourId, panel);
    setColour(juce::PopupMenu::textColourId, paper);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, brass);
    setColour(juce::PopupMenu::highlightedTextColourId, ink);
    setColour(juce::TextButton::buttonOnColourId, brass);
    setColour(juce::TextButton::textColourOffId, muted);
    setColour(juce::TextButton::textColourOnId, ink);
    setColour(juce::TooltipWindow::backgroundColourId, panel);
    setColour(juce::TooltipWindow::textColourId, paper);
    setColour(juce::TooltipWindow::outlineColourId, brass);
}
juce::Font WallLook::getTextButtonFont(juce::TextButton&, int) { return font(14, true); }
void WallLook::drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &colour,
                                    bool hover, bool down) {
    const DimWhenDisabled dimmed(g, b);
    const bool on = b.getToggleState();
    auto r = b.getLocalBounds().toFloat().reduced(.5f);
    g.setColour(on ? colour : juce::Colour(hover || down ? 0xff1e2c33 : 0xff152026));
    g.fillRoundedRectangle(r, 4);
    g.setColour(on ? colour.brighter(.1f) : juce::Colour(0xff283842));
    g.drawRoundedRectangle(r, 4, 1);
    if (b.hasKeyboardFocus(true)) {
        g.setColour(paper);
        g.drawRoundedRectangle(r.reduced(2), 3, 1);
    }
}
void WallLook::drawButtonText(juce::Graphics &g, juce::TextButton &b, bool, bool) {
    const auto buttonFont = getTextButtonFont(b, b.getHeight());
    g.setFont(buttonFont);
    auto colour = b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId
                                                  : juce::TextButton::textColourOffId);
    if (!b.isEnabled())
        colour = muted.withAlpha(.75f);
    g.setColour(colour);
    const int yIndent = juce::jmin(4, b.proportionOfHeight(0.3f));
    const int cornerSize = juce::jmin(b.getHeight(), b.getWidth()) / 2;
    const int fontHeight = juce::roundToInt(buttonFont.getHeight() * 0.6f);
    const int leftIndent = juce::jmin(fontHeight, 2 + cornerSize / (b.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = juce::jmin(fontHeight, 2 + cornerSize / (b.isConnectedOnRight() ? 4 : 2));
    const int textWidth = b.getWidth() - leftIndent - rightIndent;
    if (textWidth > 0)
        g.drawFittedText(b.getButtonText(), leftIndent, yIndent, textWidth, b.getHeight() - yIndent * 2,
                         juce::Justification::centred, 2);
}
void WallLook::drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float pos, float start,
                                float end, juce::Slider &slider) {
    const DimWhenDisabled dimmed(g, slider);
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
    if (slider.hasKeyboardFocus(false))
        drawFocusRing(g, juce::Rectangle<float>(float(x), float(y), float(w), float(h)));
}
void WallLook::drawLinearSlider(juce::Graphics &g, int x, int y, int w, int h, float position, float, float,
                                juce::Slider::SliderStyle style, juce::Slider &slider) {
    const DimWhenDisabled dimmed(g, slider);
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
    if (slider.hasKeyboardFocus(false))
        drawFocusRing(g, juce::Rectangle<float>(float(x), float(y), float(w), float(h)));
}
void WallLook::drawComboBox(juce::Graphics &g, int width, int height, bool down, int buttonX, int buttonY,
                            int buttonW, int buttonH, juce::ComboBox &box) {
    const DimWhenDisabled dimmed(g, box);
    juce::LookAndFeel_V4::drawComboBox(g, width, height, down, buttonX, buttonY, buttonW, buttonH, box);
    if (box.hasKeyboardFocus(true))
        drawFocusRing(g, juce::Rectangle<float>(0.f, 0.f, float(width), float(height)));
}
void WallEditor::slider(const char *id, bool rotary) {
    auto *parameter = wallProcessor.parameters.getParameter(id);
    auto s = std::make_unique<LockableControl<juce::Slider>>();
    const juce::String paramId(id);
    auto *raw = s.get();
    bindLockHandlers(*s, paramId);
    auto listener = std::make_unique<ChildPopupListener>();
    listener->onPopup = [this, paramId, raw](const juce::MouseEvent &e) {
        if (e.eventComponent != raw)
            showLockMenu(paramId, *raw);
    };
    listener->onBadgeClick = [raw](const juce::MouseEvent &e) {
        return e.eventComponent != raw && raw->onBadgeClick && raw->onBadgeClick(e);
    };
    s->addMouseListener(listener.get(), true);
    childPopupListeners.push_back(std::move(listener));
    s->setComponentID(id);
    s->setName(id);
    s->setTitle(parameter->getName(64));
    s->setWantsKeyboardFocus(true);
    content.addAndMakeVisible(*s);
    s->setSliderStyle(rotary ? juce::Slider::RotaryHorizontalVerticalDrag : juce::Slider::LinearVertical);
    s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 96, 25);
    s->setDoubleClickReturnValue(true, parameter->convertFrom0to1(parameter->getDefaultValue()));
    // Shift-drag uses JUCE's velocity mode as a fine drag, about one fifth of the normal speed.
    s->setVelocityModeParameters(1.0, 1, .05, true, juce::ModifierKeys::shiftModifier);
    sliderAttachments.push_back(std::make_unique<SliderAttachment>(wallProcessor.parameters, id, *s));
    sliders.emplace(id, std::move(s));
}
void WallEditor::combo(const char *id, const juce::StringArray &items) {
    auto c = std::make_unique<LockableControl<juce::ComboBox>>();
    bindLockHandlers(*c, id);
    c->setComponentID(id);
    c->setTitle(wallProcessor.parameters.getParameter(id)->getName(64));
    c->addItemList(items, 1);
    content.addAndMakeVisible(*c);
    comboAttachments.push_back(std::make_unique<ComboAttachment>(wallProcessor.parameters, id, *c));
    combos.emplace(id, std::move(c));
}
void WallEditor::toggle(const char *id) {
    const auto name = wallProcessor.parameters.getParameter(id)->getName(64);
    auto b = std::make_unique<LockableControl<juce::TextButton>>(name);
    bindLockHandlers(*b, id);
    b->setComponentID(id);
    b->setTitle(name);
    b->setClickingTogglesState(true);
    content.addAndMakeVisible(*b);
    buttonAttachments.push_back(std::make_unique<ButtonAttachment>(wallProcessor.parameters, id, *b));
    buttons.emplace(id, std::move(b));
}
void WallEditor::describe(const char *id, const juce::String &description) {
    auto *parameter = wallProcessor.parameters.getParameter(id);
    const auto range = parameter->getText(0.f, 0) + " to " + parameter->getText(1.f, 0);
    const auto tip = withLockSuffix(parameter->getName(64) + ": " + range + ". " + description
                                        + " Shift-drag for fine steps, double-click to reset, or type a value.",
                                    wallProcessor.isParamLocked(id));
    auto &target = *sliders.at(id);
    if (target.getTooltip() == tip)
        return;
    // The value box copies the tooltip only when JUCE creates it, so set it here too.
    target.setTooltip(tip);
    std::ranges::for_each(target.getChildren(), [&](juce::Component *child) {
        if (auto *box = dynamic_cast<juce::Label *>(child))
            box->setTooltip(tip);
    });
}
WallEditor::WallEditor(WallProcessor &p) : AudioProcessorEditor(p), wallProcessor(p) {
    setLookAndFeel(&look);
    content.setComponentID("content");
    content.setBounds(0, 0, WallProcessor::defaultEditorWidth, WallProcessor::defaultEditorHeight);
    addAndMakeVisible(content);
    ceilingMark = juce::Drawable::createFromImageData(BinaryData::ceilingmeter_svg, BinaryData::ceilingmeter_svgSize);
    fortress = juce::ImageCache::getFromMemory(BinaryData::fortress_png, BinaryData::fortress_pngSize);
    inputHistory.fill(-100);
    outputHistory.fill(-100);
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
    for (auto id : {"highpass_hz", "lowpass_hz"}) {
        sliders[id]->setSliderStyle(juce::Slider::LinearHorizontal);
        sliders[id]->setTextBoxStyle(juce::Slider::TextBoxRight, false, 68, 22);
    }
    // A UI-only skew puts -3 dB at the middle of the Ceiling travel. The parameter range stays linear.
    juce::NormalisableRange<double> ceilingTravel(-30.0, 0.0, 0.01);
    ceilingTravel.setSkewForCentre(-3.0);
    sliders["ceiling"]->setNormalisableRange(ceilingTravel);
    combo("highpass_slope", wall::slopeNames);
    combo("lowpass_slope", wall::slopeNames);
    combo("oversampling", {"Off", "2x", "4x", "8x", "16x"});
    combo("dither", {"Off", "24-bit Shaped", "16-bit Shaped"});
    toggle("highpass_on");
    toggle("lowpass_on");
    toggle("true_peak");
    toggle("gain_match");
    toggle("delta");
    toggle("bypass");
    // Monitoring toggles change only what you hear, so they light in red.
    std::ranges::for_each(std::array{"gain_match", "delta", "bypass"}, [this](const char *id) {
        buttons[id]->setColour(juce::TextButton::buttonOnColourId, red);
    });
    buttons["true_peak"]->setTooltip("Uses at least 4x processing and a brickwall 16-phase reconstructed-peak "
                                     "lookahead guard with zero ceiling reserve. Meter is a 4x true-peak estimate.");
    buttons["delta"]->setTooltip("Listen to processed output minus the latency-aligned original. This "
                                 "monitoring signal can exceed the output ceiling.");
    buttons["gain_match"]->setTooltip("Remove threshold makeup gain for level-matched comparison.");
    buttons["bypass"]->setTooltip("Plays the original audio. The meters keep reading the limiter output.");
    buttons["highpass_on"]->setTooltip("Turns on the high pass filter before the limiter.");
    buttons["lowpass_on"]->setTooltip("Turns on the low pass filter before the limiter.");
    combos["dither"]->setTooltip("Adds noise-shaped TPDF dither for 24-bit or 16-bit delivery.");
    for (size_t i = 0; i < modes.size(); ++i) {
        modes[i].setButtonText(wall::modeNames[int(i)]);
        modes[i].setComponentID("mode" + juce::String(int(i)));
        modes[i].setTitle(wall::modeNames[int(i)]);
        modes[i].onClick = [this, i] {
            auto *m = wallProcessor.parameters.getParameter("mode");
            m->beginChangeGesture();
            m->setValueNotifyingHost(m->convertTo0to1(float(i)));
            m->endChangeGesture();
            timerCallback();
        };
        bindLockHandlers(modes[i], "mode");
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
    presets.setTitle("Preset");
    presets.setTooltip("Choose a factory limiter preset. These 48 presets are also host programs. Use Library for saved presets.");
    presets.onChange = [this] {
        wallProcessor.applyPreset(presets.getSelectedItemIndex());
        if (presetBrowser && presetBrowser->isVisible())
            presetBrowser->refresh();
        else if (presetBrowser)
            presetBrowser->disarm();
    };
    content.addAndMakeVisible(presets);
    prevPreset.setComponentID("prev_preset");
    prevPreset.setTitle("Previous preset");
    prevPreset.setTooltip("Previous preset");
    prevPreset.onClick = [this] { stepPreset(-1); };
    content.addAndMakeVisible(prevPreset);
    nextPreset.setComponentID("next_preset");
    nextPreset.setTitle("Next preset");
    nextPreset.setTooltip("Next preset");
    nextPreset.onClick = [this] { stepPreset(1); };
    content.addAndMakeVisible(nextPreset);
    reset.setComponentID("reset_meters");
    reset.setTitle("Reset meters");
    reset.setTooltip("Clears loudness, true peak max and the clip dots.");
    reset.onClick = [this] {
        wallProcessor.resetMeters();
        inputClipped = outputClipped = false;
    };
    content.addAndMakeVisible(reset);
    link.setTitle("Link levels");
    link.setClickingTogglesState(true);
    link.setTooltip("Move threshold and ceiling together when dragging either fader.");
    content.addAndMakeVisible(link);
    combo("lufs_mode", wall::lufsModeNames);
    combos["lufs_mode"]->setTooltip("Loudness target from the BS.1770 meter. Streaming is -14 LUFS. Club is -9 LUFS.");
    slider("lufs_target", true);
    sliders["lufs_target"]->setSliderStyle(juce::Slider::LinearHorizontal);
    sliders["lufs_target"]->setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    describe("threshold", "Lowering it drives the audio into the limiter by the same amount.");
    describe("ceiling", "The highest output level. True peak keeps peaks between samples under it too.");
    describe("input_gain", "Changes the level before the limiter detects peaks.");
    describe("tone_tilt", "Tilts the spectrum around 1.2 kHz before the limiter. +6 dB raises the highs "
                          "by 3 dB and lowers the lows by 3 dB.");
    matchLoudness.setComponentID("lufs_match");
    matchLoudness.setTitle("Drive to target");
    matchLoudness.setTooltip("Adds the difference between the measured loudness and the target to "
                             "Input drive.");
    matchLoudness.onClick = [this] { wallProcessor.matchLoudnessTarget(); };
    content.addAndMakeVisible(matchLoudness);
    undoButton.setComponentID("undo");
    redoButton.setComponentID("redo");
    abA.setComponentID("ab_a");
    abB.setComponentID("ab_b");
    abAToB.setComponentID("ab_a_to_b");
    abBToA.setComponentID("ab_b_to_a");
    libraryButton.setComponentID("preset_library");
    undoButton.setTitle("Undo");
    redoButton.setTitle("Redo");
    abA.setTitle("Setup A");
    abB.setTitle("Setup B");
    abAToB.setTitle("Copy A to B");
    abBToA.setTitle("Copy B to A");
    libraryButton.setTitle("Library");
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
    const auto syncBrowserOnAB = [this] {
        if (presetBrowser && presetBrowser->isVisible())
            presetBrowser->refresh();
        else if (presetBrowser)
            presetBrowser->disarm();
    };
    abA.onClick = [this, syncBrowserOnAB] {
        if (wallProcessor.activeAB() != 0)
            wallProcessor.toggleAB();
        syncBrowserOnAB();
    };
    abB.onClick = [this, syncBrowserOnAB] {
        if (wallProcessor.activeAB() != 1)
            wallProcessor.toggleAB();
        syncBrowserOnAB();
    };
    abAToB.onClick = [this, syncBrowserOnAB] {
        wallProcessor.copyAB(0, 1);
        syncBrowserOnAB();
    };
    abBToA.onClick = [this, syncBrowserOnAB] {
        wallProcessor.copyAB(1, 0);
        syncBrowserOnAB();
    };
    for (auto *button : {&undoButton, &redoButton, &abA, &abB, &abAToB, &abBToA, &libraryButton})
        content.addAndMakeVisible(button);
    juce::StringArray factoryNames;
    for (const auto &preset : WallProcessor::factoryPresets())
        factoryNames.add(preset.name);
    presetLibrary = std::make_unique<suite::PresetLibrary>(suite::PresetLibrary::defaultDirectory("openWallLimiter"),
                                                           factoryNames);
    presetBrowser = std::make_unique<suite::PresetBrowser>(*presetLibrary);
    presetBrowser->onFactory = [this](int index) { wallProcessor.applyPreset(index); };
    presetBrowser->onUser = [this](const juce::String &name, const juce::MemoryBlock &block) {
        wallProcessor.loadParameterState(block, name);
    };
    presetBrowser->onSaved = [this](const juce::String &name) { wallProcessor.markPresetSaved(name); };
    presetBrowser->onRenamed = [this](const juce::String &name) { wallProcessor.markPresetSaved(name); };
    presetBrowser->activePreset = [this] {
        auto name = wallProcessor.presetLabel();
        return name.endsWith(" *") ? name.dropLastCharacters(2) : name;
    };
    presetBrowser->onClose = [this] { libraryButton.setToggleState(false, juce::sendNotificationSync); };
    presetBrowser->capture = [this] { return wallProcessor.captureParameters(); };
    backdrop.setComponentID("preset_backdrop");
    backdrop.onDismiss = [this] { libraryButton.setToggleState(false, juce::sendNotificationSync); };
    content.addChildComponent(backdrop);
    content.addChildComponent(*presetBrowser);
    const std::array readouts{&gainReduction, &shortTerm, &integrated, &truePeak, &inputLevel, &outputLevel};
    std::ranges::for_each(readouts, [this](Readout *readout) { content.addAndMakeVisible(readout); });
    gainReduction.setTooltip("Highest gain reduction in the last second.");
    shortTerm.setTooltip("BS.1770 short-term loudness over the last 3 seconds.");
    integrated.setTooltip("BS.1770 integrated loudness since the last reset.");
    truePeak.setTooltip("Highest true peak since the last reset. "
                        "The dot turns amber above the Ceiling and red above 0 dBTP. Click to reset it.");
    truePeak.onClick = [this] { wallProcessor.resetTruePeak(); };
    truePeak.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    inputLevel.setTooltip("Highest input peak in the last second. "
                          "The dot stays red after a peak above 0 dBFS. Click to clear it.");
    outputLevel.setTooltip("Highest output peak in the last second. "
                           "The dot stays red after a peak above 0 dBFS. Click to clear it.");
    inputLevel.onClick = [this] { inputClipped = false; };
    outputLevel.onClick = [this] { outputClipped = false; };
    inputLevel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    outputLevel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    monitoring.setComponentID("monitoring_banner");
    monitoring.setFont(font(11.5f, true));
    monitoring.setColour(juce::Label::backgroundColourId, red);
    monitoring.setColour(juce::Label::textColourId, ink);
    monitoring.setJustificationType(juce::Justification::centred);
    content.addChildComponent(monitoring);
    guidance.setComponentID("loudness_guidance");
    guidance.setFont(font(13));
    guidance.setColour(juce::Label::textColourId, paper);
    guidance.setBorderSize({});
    content.addAndMakeVisible(guidance);
    libraryButton.onClick = [this] {
        const bool open = libraryButton.getToggleState();
        backdrop.setVisible(open);
        presetBrowser->setVisible(open);
        if (open) {
            backdrop.toFront(false);
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
            const char *otherId = threshold ? "ceiling" : "threshold";
            if (link.getToggleState() && !linking && sliders[id]->isMouseButtonDown()
                && !wallProcessor.isParamLocked(otherId)) {
                linking = true;
                sliders[otherId]->setValue(sliders[otherId]->getValue() + difference, juce::sendNotificationSync);
                linking = false;
            }
        };
    const int width = wallProcessor.storedEditorWidth();
    const int height = wallProcessor.storedEditorHeight();
    setResizable(true, true);
    setResizeLimits(WallProcessor::minEditorWidth, WallProcessor::minEditorHeight, WallProcessor::maxEditorWidth,
                    WallProcessor::maxEditorHeight);
    setSize(width, height);
    updateLockAccessibility();
    timerCallback();
    startTimerHz(refreshHz);
}
bool WallEditor::keyPressed(const juce::KeyPress &key) {
    if (key == juce::KeyPress::escapeKey && presetBrowser && presetBrowser->isVisible()) {
        libraryButton.setToggleState(false, juce::sendNotificationSync);
        return true;
    }
    if (dynamic_cast<juce::TextEditor *>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;
    const auto command = juce::ModifierKeys::commandModifier;
    if (key == juce::KeyPress('z', command | juce::ModifierKeys::shiftModifier, 0)
        || key == juce::KeyPress('y', command, 0)) {
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
    sliderAttachments.clear();
    sliders.clear();
    setLookAndFeel(nullptr);
}
void WallEditor::timerCallback() {
    const auto now = size_t(cursor);
    const auto last = (now + inputHistory.size() - 1) % inputHistory.size();
    const float input = wallProcessor.inputDb.exchange(WallProcessor::noReading);
    const float output = wallProcessor.outputDb.exchange(WallProcessor::noReading);
    const float reduction = wallProcessor.reductionDb.exchange(WallProcessor::noReading);
    // A tick without a new audio block repeats the last point.
    inputHistory[now] = input > WallProcessor::noReading ? input : inputHistory[last];
    outputHistory[now] = output > WallProcessor::noReading ? output : outputHistory[last];
    reductionHistory[now] = reduction > WallProcessor::noReading ? reduction : reductionHistory[last];
    inputHold.push(inputHistory[now]);
    outputHold.push(outputHistory[now]);
    reductionHold.push(reductionHistory[now]);
    inputClipped = inputClipped || inputHistory[now] > 0.f;
    outputClipped = outputClipped || outputHistory[now] > 0.f;
    cursor = (cursor + 1) % int(inputHistory.size());
    const int active = int(wallProcessor.parameters.getRawParameterValue("mode")->load());
    for (size_t i = 0; i < modes.size(); ++i)
        modes[i].setToggleState(int(i) == active, juce::dontSendNotification);

    // These tooltips quote times and levels from the current style and settings.
    const auto &profile = wall::modeProfiles[size_t(active)];
    const auto style = wall::modeNames[active];
    const float release = wallProcessor.parameters.getRawParameterValue("release")->load();
    const float lookahead = wallProcessor.parameters.getRawParameterValue("lookahead_ms")->load();
    const float clip = wallProcessor.parameters.getRawParameterValue("clip")->load();
    const float upward = wallProcessor.parameters.getRawParameterValue("upward")->load();
    const auto fast = wall::milliseconds(float(profile.fast) * release);
    const auto slow = wall::milliseconds(float(profile.slow) * release);
    describe("release", "Scales recovery time. In " + style + ", transients recover in " + fast
                            + " and sustained limiting in " + slow + ".");
    describe("upward", "Raises quiet passages between about -60 and -18 LUFS by up to "
                           + wall::fixed(upward, 1) + " dB. Below -60 LUFS it lowers noise by up to "
                           + wall::fixed(std::min(6.f, .6f * upward), 1) + " dB.");
    describe("lookahead_ms", "Looks ahead for peaks. Attack is a share of this time: "
                                 + wall::milliseconds(float(profile.attackRatio) * lookahead) + " in " + style
                                 + ". Host latency does not follow this control. It is the linear-phase "
                                   "sub-cut delay, the limiter delay, the longest oversampling filter delay, "
                                   "and the 128-sample true-peak guard. The editor shows that latency in "
                                   "milliseconds and samples.");
    std::ranges::for_each(std::views::iota(size_t{0}, modes.size()), [&](size_t i) {
        const auto &each = wall::modeProfiles[i];
        const auto share = juce::String(juce::roundToInt(each.attackRatio * 100.0)) + "% of Lookahead";
        modes[i].setTooltip(withLockSuffix(juce::String(modeCharacters[i]) + " Attack "
                                               + wall::milliseconds(float(each.attackRatio) * lookahead) + " (" + share
                                               + "), hold " + wall::milliseconds(float(each.hold)) + ", recovery "
                                               + wall::milliseconds(float(each.fast) * release) + " to "
                                               + wall::milliseconds(float(each.slow) * release) + ".",
                                           wallProcessor.isParamLocked("mode")));
    });

    undoButton.setEnabled(wallProcessor.canUndo());
    redoButton.setEnabled(wallProcessor.canRedo());
    abA.setToggleState(wallProcessor.activeAB() == 0, juce::dontSendNotification);
    abB.setToggleState(wallProcessor.activeAB() == 1, juce::dontSendNotification);
    const int lufsMode = int(std::lround(wallProcessor.parameters.getRawParameterValue("lufs_mode")->load()));
    const float custom = wallProcessor.parameters.getRawParameterValue("lufs_target")->load();
    gainReduction.show(reading(reductionHold.value, " dB"), red);
    shortTerm.show(reading(wallProcessor.shortLufs.load(), " LUFS"), paper);
    integrated.show(reading(wallProcessor.integratedLufs.load(), " LUFS"), paper);
    const float tp = wallProcessor.truePeakDb.load();
    auto tpLed = ledOff;
    // A 0.01 dB margin keeps a guarded ceiling from lighting the dot.
    if (tp > wallProcessor.parameters.getRawParameterValue("ceiling")->load() + .01f)
        tpLed = brass;
    if (tp > 0.f)
        tpLed = red;
    truePeak.show(reading(tp, " dBTP"), paper, tpLed);
    inputLevel.show(reading(inputHold.value, " dB"), blue, inputClipped ? red : ledOff);
    outputLevel.show(reading(outputHold.value, " dB"), brass, outputClipped ? red : ledOff);
    juce::StringArray monitors;
    std::ranges::for_each(std::array{"bypass", "delta", "gain_match"}, [&](const char *id) {
        if (wallProcessor.parameters.getRawParameterValue(id)->load() > .5f)
            monitors.add(wallProcessor.parameters.getParameter(id)->getName(64));
    });
    monitoring.setText(monitors.joinIntoString(", ") + " on. Meters read the limiter output.",
                       juce::dontSendNotification);
    monitoring.setVisible(!monitors.isEmpty());
    guidance.setText(wall::loudnessGuidance(lufsMode, custom, wallProcessor.integratedLufs.load(),
                                            wallProcessor.shortLufs.load()),
                     juce::dontSendNotification);
    const float loudest = std::max(wallProcessor.integratedLufs.load(), wallProcessor.shortLufs.load());
    const bool measured = loudest > -99.f;
    matchLoudness.setEnabled(lufsMode != 0 && measured);

    // Controls with no effect right now draw dimmed, stay editable, and say what turns them on.
    const bool highpassOn = wallProcessor.parameters.getRawParameterValue("highpass_on")->load() > .5f;
    const bool lowpassOn = wallProcessor.parameters.getRawParameterValue("lowpass_on")->load() > .5f;
    const bool safe = active == int(wall::Mode::Safe);
    const bool mono = wallProcessor.getTotalNumInputChannels() < 2;
    auto &oversampling = *combos["oversampling"];
    const int rate = int(wallProcessor.parameters.getRawParameterValue("oversampling")->load());
    const bool forced = wallProcessor.parameters.getRawParameterValue("true_peak")->load() > .5f && rate < 2;
    const std::array<std::pair<juce::Component *, bool>, 10> live{{
        {sliders["highpass_hz"].get(), highpassOn}, {combos["highpass_slope"].get(), highpassOn},
        {sliders["lowpass_hz"].get(), lowpassOn}, {combos["lowpass_slope"].get(), lowpassOn},
        {sliders["clip"].get(), !safe}, {sliders["lufs_target"].get(), lufsMode == 3},
        {sliders["stereo_link"].get(), !mono}, {sliders["release_link"].get(), !mono},
        {sliders["stereo_width"].get(), !mono}, {&oversampling, !forced}}};
    std::ranges::for_each(live, [](const auto &control) {
        control.first->setAlpha(control.second ? 1.f : dimAlpha);
    });
    const juce::String needsHighpass = highpassOn ? "" : " Turn on High pass to use it.";
    const juce::String needsLowpass = lowpassOn ? "" : " Turn on Low pass to use it.";
    const juce::String needsStereo = mono ? " Works on stereo tracks only." : "";
    describe("highpass_hz", "Sets the high pass cutoff before the limiter." + needsHighpass);
    describe("lowpass_hz", "Sets the low pass cutoff before the limiter." + needsLowpass);
    combos["highpass_slope"]->setTooltip(withLockSuffix(
        "Sets the high pass slope, 12 to 96 dB per octave." + needsHighpass,
        wallProcessor.isParamLocked("highpass_slope")));
    combos["lowpass_slope"]->setTooltip(withLockSuffix(
        "Sets the low pass slope, 12 to 96 dB per octave." + needsLowpass,
        wallProcessor.isParamLocked("lowpass_slope")));
    describe("stereo_link",
             "Shares peak detection above 120 Hz between the channels. Bass below 120 Hz is always linked."
                 + needsStereo);
    describe("release_link",
             "Shares recovery between the channels. At 0% each channel recovers alone." + needsStereo);
    describe("stereo_width",
             "Sets mid/side width before the limiter. 0% is mono; 200% doubles the side." + needsStereo);
    const juce::String needsCustom = lufsMode == 3 ? "" : " Choose Custom in Loudness target to use it.";
    describe("lufs_target", "Sets the target that Custom uses." + needsCustom);
    auto clipNote = "Transient peaks up to " + wall::fixed(float(profile.clipDb) * clip * .01f, 1)
                  + " dB over the Ceiling go to a soft clipper instead of the limiter. " + style
                  + " allows up to " + wall::fixed(float(profile.clipDb), 1) + " dB at 100%.";
    if (safe)
        clipNote = "No effect in Safe. Choose another style to use it.";
    describe("clip", clipNote);
    oversampling.setTooltip(withLockSuffix("Runs the limiter at a higher rate. True peak uses at least 4x."
                                               + juce::String(forced ? " Turn off True peak to use Off or 2x." : ""),
                                           wallProcessor.isParamLocked("oversampling")));
    // Keep the ComboBox item ID matched to the saved choice so the popup menu retains its tick mark.
    static constexpr std::array<const char *, 2> baseRates{"Off", "2x"};
    for (int i = 0; i < 2; ++i) {
        const juce::String target = (forced && i == rate) ? "4x (True peak)" : baseRates[size_t(i)];
        if (oversampling.getItemText(i) != target)
            oversampling.changeItemText(i + 1, target);
    }
    const auto shown = oversampling.getItemText(rate);
    if (oversampling.getText() != shown)
        oversampling.setText(shown, juce::dontSendNotification);
    // A label that matches no menu item clears the selection, so picking that preset again reloads it.
    const auto label = wallProcessor.presetLabel();
    if (!presets.isPopupActive() && presets.getText() != label)
        presets.setText(label, juce::dontSendNotification);
    content.repaint(240, 323, 640, 162);
    const float phase = wallProcessor.artworkPhase();
    if (std::abs(phase - lastArtworkPhase) > 1e-4f) {
        lastArtworkPhase = phase;
        content.repaint(20, 10, 56, 56);
    }
    const int factor = wallProcessor.effectiveFactor.load();
    const int latency = wallProcessor.getLatencySamples();
    const double sampleRate = wallProcessor.preparedSampleRate();
    if (factor != lastEffectiveFactor || latency != lastLatencySamples || std::abs(sampleRate - lastSampleRate) > 1e-6) {
        lastEffectiveFactor = factor;
        lastLatencySamples = latency;
        lastSampleRate = sampleRate;
        content.repaint(24, 800, 1072, 24);
    }
    uint32_t lockMask = 0;
    for (int i = 0; auto *parameter : wallProcessor.getParameters()) {
        if (auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(parameter))
            if (wallProcessor.isParamLocked(ranged->paramID))
                lockMask |= (uint32_t{1} << i);
        ++i;
    }
    if (lockMask != lastLockMask) {
        lastLockMask = lockMask;
        updateLockAccessibility();
        content.repaint();
    }
}
void WallEditor::stepPreset(int delta) {
    if (!presetLibrary)
        return;
    const auto entries = presetLibrary->entries("");
    if (entries.empty())
        return;
    auto current = wallProcessor.presetLabel();
    if (current.endsWith(" *"))
        current = current.dropLastCharacters(2);
    int index = -1;
    for (int i = 0; i < int(entries.size()); ++i) {
        if (entries[size_t(i)].name == current) {
            index = i;
            break;
        }
    }
    const int count = int(entries.size());
    const int next = index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count;
    const auto &target = entries[size_t(next)];
    if (target.factory) {
        wallProcessor.applyPreset(target.factoryIndex);
    } else {
        const auto block = presetLibrary->loadUser(target.name);
        if (block.getSize() > 0)
            wallProcessor.loadParameterState(block, target.name);
    }
    if (presetBrowser && presetBrowser->isVisible())
        presetBrowser->refresh();
    else if (presetBrowser)
        presetBrowser->disarm();
}
void WallEditor::updateLockAccessibility() {
    const auto update = [&](const juce::String &id, juce::Component &control) {
        const bool locked = wallProcessor.isParamLocked(id);
        control.setTitle(withLockSuffix(control.getTitle(), locked));
        if (auto *tipClient = dynamic_cast<juce::SettableTooltipClient *>(&control))
            tipClient->setTooltip(withLockSuffix(tipClient->getTooltip(), locked));
    };
    for (const auto &[id, s] : sliders)
        update(id, *s);
    for (const auto &[id, c] : combos)
        update(id, *c);
    for (const auto &[id, b] : buttons)
        update(id, *b);
    for (auto &modeButton : modes)
        update("mode", modeButton);
}
juce::Rectangle<int> WallEditor::padlockBoundsFor(const juce::String &id, const juce::Component &control) const {
    const auto bounds = control.getBounds();
    if (id == "ceiling")
        return {bounds.getX() + 2, bounds.getY() + 2, 13, 14};
    if (combos.contains(id))
        return {bounds.getRight() - 37, bounds.getY() + 2, 13, 14};
    return {bounds.getRight() - 15, bounds.getY() + 2, 13, 14};
}
void WallEditor::showLockMenu(const juce::String &paramID, juce::Component &target) {
    auto *parameter = wallProcessor.parameters.getParameter(paramID);
    const auto name = parameter != nullptr ? parameter->getName(64) : paramID;
    const bool locked = wallProcessor.isParamLocked(paramID);
    juce::PopupMenu menu;
    menu.addItem(1, (locked ? "Unlock " : "Lock ") + name);
    if (wallProcessor.anyParamLocked()) {
        menu.addSeparator();
        menu.addItem(2, "Unlock all parameters");
    }
    juce::Component::SafePointer<WallEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [safeThis, paramID, locked](int result) {
                           if (safeThis == nullptr)
                               return;
                           if (result == 1)
                               safeThis->wallProcessor.setParamLocked(paramID, !locked);
                           else if (result == 2)
                               safeThis->wallProcessor.unlockAllParams();
                           else
                               return;
                           safeThis->updateLockAccessibility();
                           safeThis->content.repaint();
                       });
}
void WallEditor::paintLockIndicators(juce::Graphics &g) {
    if (presetBrowser && presetBrowser->isVisible())
        return;
    const auto drawIfLocked = [&](const juce::String &id, const juce::Component &control) {
        if (control.isVisible() && wallProcessor.isParamLocked(id))
            drawPadlock(g, padlockBoundsFor(id, control).toFloat());
    };
    for (const auto &[id, s] : sliders)
        drawIfLocked(id, *s);
    for (const auto &[id, c] : combos)
        drawIfLocked(id, *c);
    for (const auto &[id, b] : buttons)
        drawIfLocked(id, *b);
    if (wallProcessor.isParamLocked("mode")) {
        const int active = std::clamp(int(wallProcessor.parameters.getRawParameterValue("mode")->load()), 0,
                                      int(modes.size()) - 1);
        drawPadlock(g, padlockBoundsFor("mode", modes[size_t(active)]).toFloat());
    }
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
    undoButton.setBounds(308, 18, 64, 40);
    redoButton.setBounds(376, 18, 64, 40);
    abA.setBounds(448, 18, 40, 40);
    abB.setBounds(492, 18, 40, 40);
    abAToB.setBounds(540, 18, 50, 40);
    abBToA.setBounds(594, 18, 50, 40);
    prevPreset.setBounds(652, 18, 40, 40);
    presets.setBounds(696, 18, 284, 40);
    nextPreset.setBounds(984, 18, 40, 40);
    libraryButton.setBounds(1028, 18, 72, 40);
    buttons["highpass_on"]->setBounds(40, 101, 104, 40);
    sliders["highpass_hz"]->setBounds(152, 101, 202, 40);
    combos["highpass_slope"]->setBounds(368, 101, 136, 40);
    buttons["lowpass_on"]->setBounds(615, 101, 104, 40);
    sliders["lowpass_hz"]->setBounds(727, 101, 202, 40);
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
    combos["lufs_mode"]->setBounds(112, 688, 136, 40);
    sliders["lufs_target"]->setBounds(256, 688, 204, 40);
    matchLoudness.setBounds(468, 688, 122, 40);
    combos["oversampling"]->setBounds(122, 756, 150, 40);
    buttons["true_peak"]->setBounds(284, 756, 104, 40);
    combos["dither"]->setBounds(458, 756, 138, 40);
    buttons["gain_match"]->setBounds(640, 756, 115, 40);
    buttons["delta"]->setBounds(765, 756, 75, 40);
    buttons["bypass"]->setBounds(850, 756, 84, 40);
    reset.setBounds(949, 756, 143, 40);
    gainReduction.setBounds(240, 265, 164, 49);
    shortTerm.setBounds(408, 265, 164, 49);
    integrated.setBounds(576, 265, 164, 49);
    truePeak.setBounds(744, 265, 164, 49);
    inputLevel.setBounds(40, 570, 140, 50);
    outputLevel.setBounds(948, 570, 140, 50);
    guidance.setBounds(598, 695, 498, 24);
    monitoring.setBounds(360, 510, 400, 21);
    backdrop.setBounds(content.getLocalBounds());
    if (presetBrowser)
        presetBrowser->setBounds(690, 88, 406, 560);
    juce::AudioProcessorEditor::resized();
}
void WallEditor::drawGraph(juce::Graphics &g) {
    juce::Rectangle<float> r{240, 323, 640, 162};
    g.setColour(juce::Colour(0xff0d1214));
    g.fillRoundedRectangle(r, 4);
    // Levels span -24 dBFS at the bottom to 0 dBFS at the top.
    // Gain reduction hangs from the top on its own 0 to 12 dB scale.
    const auto y = [&](float v) {
        return r.getBottom() - std::clamp((v + 24.f) / 24.f, 0.f, 1.f) * r.getHeight();
    };
    const auto depth = [&](float v) { return r.getY() + std::clamp(v / 12.f, 0.f, 1.f) * r.getHeight(); };
    g.setColour(juce::Colour(0xff283232));
    std::ranges::for_each(std::array{-18.f, -12.f, -6.f, 0.f},
                          [&](float level) { g.drawHorizontalLine(int(y(level)), r.getX(), r.getRight()); });
    std::ranges::for_each(std::array{0, -6, -12, -18, -24}, [&](int level) {
        text(g, juce::String(level), {208, int(y(float(level))) - 8, 28, 16}, 10, muted,
             juce::Justification::centredRight);
    });
    std::ranges::for_each(std::array{0, 3, 6, 12}, [&](int reduction) {
        text(g, juce::String(reduction), {885, int(depth(float(reduction))) - 8, 28, 16}, 11.5f,
             red.interpolatedWith(paper, .22f));
    });
    for (int i = 1; i < 8; ++i) {
        g.setColour(juce::Colour(0xff20292a));
        g.drawVerticalLine(int(r.getX() + float(i) * 80), r.getY(), r.getBottom());
    }
    const auto trace = [&](const auto &history, const auto &toY) {
        juce::Path line;
        line.startNewSubPath(r.getX(), toY(history[size_t(cursor)]));
        std::ranges::for_each(std::views::iota(size_t{1}, history.size()), [&](size_t i) {
            line.lineTo(r.getX() + float(i) * r.getWidth() / float(history.size() - 1),
                        toY(history[(size_t(cursor) + i) % history.size()]));
        });
        return line;
    };
    const auto reduction = trace(reductionHistory, depth);
    auto band = reduction;
    band.lineTo(r.getRight(), r.getY());
    band.lineTo(r.getX(), r.getY());
    band.closeSubPath();
    g.setColour(red.withAlpha(.2f));
    g.fillPath(band);
    g.setColour(red);
    g.strokePath(reduction, juce::PathStrokeType(1.4f));
    g.setColour(blue.withAlpha(.6f));
    g.strokePath(trace(inputHistory, y), juce::PathStrokeType(1.8f));
    g.setColour(brass);
    g.strokePath(trace(outputHistory, y), juce::PathStrokeType(1.8f));
    const float ceiling = wallProcessor.parameters.getRawParameterValue("ceiling")->load();
    g.setColour(brass.withAlpha(.3f));
    const float dash[]{4, 4};
    g.drawDashedLine({r.getX(), y(ceiling), r.getRight(), y(ceiling)}, dash, 2, 1);
    text(g, "Input", {240, 489, 60, 18}, 13, blue);
    text(g, "Output", {310, 489, 64, 18}, 13, brass);
    text(g, "Reduction", {390, 489, 76, 18}, 13, red);
    text(g, "8 seconds", {776, 489, 104, 18}, 13, paper.withAlpha(.82f), juce::Justification::centredRight);
}
void WallEditor::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff090c0d));
    const auto framed = content.getBounds().toFloat().transformedBy(content.getTransform());
    if (framed.getWidth() < float(getWidth()) - .5f || framed.getHeight() < float(getHeight()) - .5f) {
        g.setColour(juce::Colour(0xff283842));
        g.drawRect(framed.expanded(.5f), 1.f);
    }
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
    text(g, "TRUE PEAK BRICKWALL LIMITER", {82, 45, 220, 18}, 11.5f, brass.interpolatedWith(paper, .15f));
    box(g, {24, 89, 496, 75});
    box(g, {599, 89, 497, 75});
    pillText(g, "Pre-limiter high-pass", {28, 169, 220, 20}, 12,
             paper.withAlpha(.85f).withMultipliedAlpha(sliders.at("highpass_hz")->getAlpha()),
             juce::Justification::centredLeft);
    pillText(g, "Pre-limiter low-pass", {872, 169, 220, 20}, 12,
             paper.withAlpha(.85f).withMultipliedAlpha(sliders.at("lowpass_hz")->getAlpha()),
             juce::Justification::centredRight);
    pillText(g, wallProcessor.parameters.getParameter("mode")->getName(64), {150, 208, 78, 26}, 16.f, brass,
             juce::Justification::centredRight);
    box(g, {24, 255, 166, 290});
    box(g, {930, 255, 166, 290});
    // dB ticks follow each fader's own scale.
    const std::array<std::tuple<const char *, std::array<int, 6>, int, int, int>, 2> faderTicks{{
        {"threshold", {0, -6, -12, -18, -24, -30}, 82, 48, juce::Justification::centredRight},
        {"ceiling", {0, -1, -2, -3, -6, -30}, 1040, 1050, juce::Justification::centredLeft}}};
    std::ranges::for_each(faderTicks, [&](const auto &fader) {
        const auto &[id, values, tickX, labelX, justify] = fader;
        const auto &slider = *sliders.at(id);
        std::ranges::for_each(values, [&](int value) {
            const int y = slider.getY() + juce::roundToInt(slider.getPositionOfValue(value));
            g.setColour(muted.brighter(.2f));
            g.drawHorizontalLine(y, float(tickX), float(tickX + 6));
            text(g, juce::String(value), {labelX, y - 8, 32, 16}, 11.5f, paper.withAlpha(.82f), justify);
        });
    });
    const auto controlAlpha = [&](const char *id) {
        if (const auto it = sliders.find(id); it != sliders.end())
            return it->second->getAlpha();
        if (const auto it = combos.find(id); it != combos.end())
            return it->second->getAlpha();
        return 1.f;
    };
    const std::array<std::tuple<const char *, juce::Rectangle<int>, float, juce::Colour, int>, 6> captions{{
        {"threshold", {40, 270, 134, 25}, 17, paper, juce::Justification::centred},
        {"ceiling", {946, 270, 134, 25}, 17, paper, juce::Justification::centred},
        {"lufs_mode", {24, 695, 88, 24}, 12, muted, juce::Justification::centredLeft},
        {"lufs_target", {256, 667, 204, 20}, 13.f, paper, juce::Justification::centredLeft},
        {"oversampling", {24, 764, 94, 24}, 13.f, paper.withAlpha(.84f), juce::Justification::centredLeft},
        {"dither", {402, 764, 52, 24}, 13.f, paper.withAlpha(.84f), juce::Justification::centredLeft}}};
    std::ranges::for_each(captions, [&](const auto &caption) {
        const auto &[id, area, size, colour, justify] = caption;
        const float alpha = std::max(controlAlpha(id), .58f);
        text(g, wallProcessor.parameters.getParameter(id)->getName(64), area, size,
             colour.withMultipliedAlpha(alpha), justify);
    });
    drawGraph(g);
    std::ranges::for_each(knobIds, [&](const char *id) {
        const auto name = wallProcessor.parameters.getParameter(id)->getName(64);
        text(g, name, {sliders.at(id)->getX(), 537, 74, 20}, 12,
             paper.withMultipliedAlpha(sliders.at(id)->getAlpha()), juce::Justification::centred);
    });
    g.setColour(juce::Colour(0xff283842));
    g.drawHorizontalLine(744, 24, 1096);
    text(g,
         juce::String(wallProcessor.effectiveFactor.load()) + "x active  |  "
             + wall::latencyReadout(wallProcessor.getLatencySamples(), wallProcessor.preparedSampleRate()),
         {24, 801, 620, 20}, 12.5f, paper.withAlpha(.78f));
    text(g, "Alan Marcero / v" JucePlugin_VersionString, {740, 801, 356, 20}, 12.5f, paper.withAlpha(.78f),
         juce::Justification::centredRight);
}
