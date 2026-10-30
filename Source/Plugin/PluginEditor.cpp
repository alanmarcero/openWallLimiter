#include "PluginEditor.h"
#include "BinaryData.h"
#include <cmath>
namespace {
const juce::Colour ink{0xff131617}, panel{0xff1b1f20}, brass{0xffd2ad70}, paper{0xffe9e3d7},
    muted{0xff9da39f}, blue{0xff78b7bf}, red{0xffe88d70};
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
    return std::isfinite(v) && v > -99 ? juce::String(v, 1) + unit : juce::String("--") + unit;
}
void box(juce::Graphics &g, juce::Rectangle<float> r) {
    g.setColour(panel.withAlpha(.94f));
    g.fillRoundedRectangle(r, 6);
    g.setColour(juce::Colour(0xff41433d));
    g.drawRoundedRectangle(r, 6, 1);
}
} // namespace
WallLook::WallLook() {
    setColour(juce::Slider::textBoxTextColourId, paper);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, ink);
    setColour(juce::ComboBox::textColourId, paper);
    setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff535344));
    setColour(juce::ComboBox::arrowColourId, brass);
    setColour(juce::PopupMenu::backgroundColourId, ink);
    setColour(juce::PopupMenu::textColourId, paper);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, brass);
    setColour(juce::PopupMenu::highlightedTextColourId, ink);
    setColour(juce::TextButton::textColourOffId, muted);
    setColour(juce::TextButton::textColourOnId, ink);
    setColour(juce::TooltipWindow::backgroundColourId, paper);
    setColour(juce::TooltipWindow::textColourId, ink);
}
void WallLook::drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &, bool hover,
                                    bool down) {
    const bool on = b.getToggleState();
    auto r = b.getLocalBounds().toFloat().reduced(.5f);
    g.setColour(on ? brass : juce::Colour(hover || down ? 0xff363a36 : 0xff222727));
    g.fillRoundedRectangle(r, 4);
    g.setColour(on ? brass.brighter(.1f) : juce::Colour(0xff555548));
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
    g.setColour(juce::Colour(0xff454940));
    g.strokePath(track, juce::PathStrokeType(3));
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, radius, radius, 0, start, start + pos * (end - start), true);
    g.setColour(brass);
    g.strokePath(arc, juce::PathStrokeType(3));
    auto circle = juce::Rectangle<float>(radius * 1.62f, radius * 1.62f).withCentre(centre);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff62605a), circle.getTopLeft(),
                                           juce::Colour(0xff171b1c), circle.getBottomRight(), false));
    g.fillEllipse(circle);
    g.setColour(juce::Colour(0xff777467));
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
void WallEditor::slider(const char *id, const char *suffix, bool rotary) {
    auto s = std::make_unique<juce::Slider>();
    s->setComponentID(id);
    s->setName(id);
    s->setSliderStyle(rotary ? juce::Slider::RotaryHorizontalVerticalDrag : juce::Slider::LinearVertical);
    s->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 110, 25);
    s->setTextValueSuffix(suffix);
    s->setNumDecimalPlacesToDisplay(1);
    s->setDoubleClickReturnValue(true, wallProcessor.parameters.getParameter(id)->convertFrom0to1(
                                           wallProcessor.parameters.getParameter(id)->getDefaultValue()));
    s->setTooltip(juce::String(id) + ": drag to adjust, double-click to reset, or type a value.");
    addAndMakeVisible(*s);
    sliderAttachments.push_back(std::make_unique<SliderAttachment>(wallProcessor.parameters, id, *s));
    sliders.emplace(id, std::move(s));
}
void WallEditor::combo(const char *id, const juce::StringArray &items) {
    auto c = std::make_unique<juce::ComboBox>();
    c->setComponentID(id);
    c->addItemList(items, 1);
    addAndMakeVisible(*c);
    comboAttachments.push_back(std::make_unique<ComboAttachment>(wallProcessor.parameters, id, *c));
    combos.emplace(id, std::move(c));
}
void WallEditor::toggle(const char *id, const char *label) {
    auto b = std::make_unique<juce::TextButton>(label);
    b->setComponentID(id);
    b->setClickingTogglesState(true);
    addAndMakeVisible(*b);
    buttonAttachments.push_back(std::make_unique<ButtonAttachment>(wallProcessor.parameters, id, *b));
    buttons.emplace(id, std::move(b));
}
WallEditor::WallEditor(WallProcessor &p) : AudioProcessorEditor(p), wallProcessor(p) {
    setLookAndFeel(&look);
    fortress = juce::ImageCache::getFromMemory(BinaryData::fortress_png, BinaryData::fortress_pngSize);
    inputHistory.fill(-60);
    outputHistory.fill(-60);
    reductionHistory.fill(0);
    slider("threshold", " dB", false);
    slider("ceiling", " dB", false);
    slider("release", "x", true);
    slider("clip", "%", true);
    slider("upward", " dB", true);
    slider("stereo_link", "%", true);
    slider("highpass_hz", " Hz", false);
    slider("lowpass_hz", " Hz", false);
    for (auto id : {"highpass_hz", "lowpass_hz"})
        sliders[id]->setSliderStyle(juce::Slider::LinearHorizontal);
    combo("highpass_slope", wall::slopeNames);
    combo("lowpass_slope", wall::slopeNames);
    combo("oversampling", {"Off", "2x", "4x", "8x", "16x"});
    combo("dither", {"Off", "24-bit TPDF", "16-bit TPDF"});
    toggle("highpass_on", "High pass");
    toggle("lowpass_on", "Low pass");
    toggle("true_peak", "True peak");
    toggle("gain_match", "Gain match");
    toggle("delta", "Delta");
    toggle("bypass", "Bypass");
    buttons["true_peak"]->setTooltip("Uses at least 4x processing and a final reconstructed-peak lookahead "
                                     "guard and 0.3 dB headroom. Meter is a 4x true-peak estimate.");
    buttons["delta"]->setTooltip("Listen to processed output minus the latency-aligned original. This "
                                 "monitoring signal can exceed the output ceiling.");
    buttons["gain_match"]->setTooltip("Remove threshold makeup gain for level-matched comparison.");
    const std::array<const char *, 5> hints{
        "Short attack and program-dependent release for punch and density",
        "Full lookahead and a rounded release for a transparent, relaxed sound",
        "1 ms attack, fast release and the most transient clipping for a loud, forward sound",
        "Full lookahead, long hold and slow release, with intentional clipping disabled",
        "Constant-rate release in dB for a steady, classic recovery"};
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
        addAndMakeVisible(modes[i]);
    }
    presets.addItemList({"Init / clean start", "Modern master", "Smooth & full", "Push the wall",
                         "Safe delivery", "Classic weight"},
                        1);
    presets.setSelectedId(1, juce::dontSendNotification);
    presets.setComponentID("presets");
    presets.onChange = [this] { wallProcessor.applyPreset(presets.getSelectedItemIndex()); };
    addAndMakeVisible(presets);
    reset.setComponentID("reset_meters");
    reset.onClick = [this] { wallProcessor.resetMeters(); };
    addAndMakeVisible(reset);
    link.setClickingTogglesState(true);
    link.setTooltip("Move threshold and ceiling together when dragging either fader.");
    addAndMakeVisible(link);
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
    setSize(1120, 760);
    timerCallback();
    startTimerHz(30);
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
    repaint();
}
void WallEditor::resized() {
    presets.setBounds(825, 22, 267, 32);
    buttons["highpass_on"]->setBounds(40, 105, 104, 29);
    sliders["highpass_hz"]->setBounds(152, 94, 202, 58);
    combos["highpass_slope"]->setBounds(368, 105, 136, 29);
    buttons["lowpass_on"]->setBounds(615, 105, 104, 29);
    sliders["lowpass_hz"]->setBounds(727, 94, 202, 58);
    combos["lowpass_slope"]->setBounds(940, 105, 136, 29);
    for (size_t i = 0; i < modes.size(); ++i)
        modes[i].setBounds(240 + int(i) * 132, 201, 124, 35);
    sliders["threshold"]->setBounds(53, 300, 110, 190);
    sliders["ceiling"]->setBounds(959, 300, 110, 190);
    link.setBounds(91, 509, 62, 24);
    sliders["release"]->setBounds(260, 560, 115, 96);
    sliders["clip"]->setBounds(430, 560, 115, 96);
    sliders["upward"]->setBounds(600, 560, 115, 96);
    sliders["stereo_link"]->setBounds(770, 560, 115, 96);
    combos["oversampling"]->setBounds(122, 693, 88, 30);
    buttons["true_peak"]->setBounds(222, 693, 104, 30);
    combos["dither"]->setBounds(398, 693, 138, 30);
    buttons["gain_match"]->setBounds(640, 693, 115, 30);
    buttons["delta"]->setBounds(765, 693, 75, 30);
    buttons["bypass"]->setBounds(850, 693, 84, 30);
    reset.setBounds(949, 693, 143, 30);
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
    g.drawImage(fortress, {0, 62, 1120, 374});
    g.setGradientFill(juce::ColourGradient(ink.withAlpha(.05f), 0, 100, ink, 0, 410, false));
    g.fillRect(0, 62, 1120, 374);
    g.setColour(juce::Colour(0xff090c0d));
    g.fillRect(0, 0, getWidth(), 73);
    g.setColour(brass.withAlpha(.3f));
    g.drawHorizontalLine(72, 0, float(getWidth()));
    // The crenellated mark echoes the limiter's flat ceiling.
    juce::Path mark;
    mark.startNewSubPath(27, 45);
    mark.lineTo(27, 27);
    mark.lineTo(34, 27);
    mark.lineTo(34, 33);
    mark.lineTo(42, 33);
    mark.lineTo(42, 23);
    mark.lineTo(50, 23);
    mark.lineTo(50, 33);
    mark.lineTo(58, 33);
    mark.lineTo(58, 27);
    mark.lineTo(65, 27);
    mark.lineTo(65, 45);
    mark.closeSubPath();
    g.setColour(brass);
    g.fillPath(mark);
    text(g, "openWallLimiter", {80, 15, 480, 35}, 29, paper);
    text(g, "A fortress for your final mix", {82, 49, 500, 18}, 12, muted);
    box(g, {24, 89, 496, 75});
    box(g, {599, 89, 497, 75});
    text(g, "Pre-limiter filters", {24, 171, 220, 18}, 12, paper);
    text(g, "High pass  /  low pass", {856, 171, 240, 18}, 12, paper, juce::Justification::centredRight);
    text(g, "Character", {35, 204, 180, 25}, 17, brass);
    box(g, {24, 255, 166, 290});
    box(g, {930, 255, 166, 290});
    text(g, "Threshold", {40, 270, 134, 25}, 17, paper, juce::Justification::centred);
    text(g, "Ceiling", {946, 270, 134, 25}, 17, paper, juce::Justification::centred);
    text(g, "Push into the wall", {36, 493, 142, 16}, 11, muted, juce::Justification::centred);
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
    drawGraph(g);
    const std::array<const char *, 4> names{"Release", "Clip", "Upward", "Stereo link"};
    const std::array<const char *, 4> desc{"Recovery time", "Peak character", "Quiet detail",
                                           "Image stability"};
    for (size_t i = 0; i < 4; ++i) {
        int x = 250 + int(i) * 170;
        text(g, names[i], {x, 537, 135, 20}, 14, paper, juce::Justification::centred);
        text(g, desc[i], {x, 656, 135, 17}, 11, muted, juce::Justification::centred);
    }
    text(g, "Input", {40, 570, 70, 20}, 12, muted);
    text(g, reading(wallProcessor.inputDb.load(), " dB"), {40, 592, 140, 28}, 23, blue);
    text(g, "Output", {948, 570, 100, 20}, 12, muted);
    text(g, reading(wallProcessor.outputDb.load(), " dB"), {948, 592, 140, 28}, 23, brass);
    text(g, "Oversampling", {24, 698, 98, 20}, 12, muted);
    text(g, "Dither", {348, 698, 50, 20}, 12, muted);
    g.setColour(juce::Colour(0xff3a4038));
    g.drawHorizontalLine(680, 24, 1096);
    text(g,
         juce::String(wallProcessor.effectiveFactor.load()) + "x active  |  " +
             juce::String(wallProcessor.getLatencySamples()) + " samples latency",
         {24, 733, 500, 18}, 11, muted);
    text(g, "Alan Marcero   /   v" JucePlugin_VersionString, {792, 733, 300, 18}, 11, muted, juce::Justification::centredRight);
}
