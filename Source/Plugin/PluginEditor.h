#pragma once
#include "PluginProcessor.h"
#include <map>
#include <memory>
namespace suite {
class PresetLibrary;
class PresetBrowser;
} // namespace suite
class WallLook final : public juce::LookAndFeel_V4 {
  public:
    WallLook();
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawRotarySlider(juce::Graphics &, int, int, int, int, float, float, float, juce::Slider &) override;
    void drawLinearSlider(juce::Graphics &, int, int, int, int, float, float, float,
                          juce::Slider::SliderStyle, juce::Slider &) override;
    void drawButtonBackground(juce::Graphics &, juce::Button &, const juce::Colour &, bool, bool) override;
};
// A meter reading as a component, so screen readers can find it. A click runs onClick when it is set.
class Readout final : public juce::Component, public juce::SettableTooltipClient {
  public:
    Readout(const char *id, const juce::String &caption, float valueSize);
    std::function<void()> onClick;
    void show(const juce::String &shown, juce::Colour shownColour, juce::Colour shownLed = {});
    void paint(juce::Graphics &) override;
    void mouseUp(const juce::MouseEvent &) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

  private:
    juce::String value;
    juce::Colour colour, led;
    float size;
};
class WallEditor final : public juce::AudioProcessorEditor, private juce::Timer {
  public:
    explicit WallEditor(WallProcessor &);
    ~WallEditor() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;

  private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    static constexpr int refreshHz = 30;
    // Holds the highest reading for one second of editor ticks.
    struct PeakHold {
        float value;
        int age = 0;
        void push(float peak) noexcept {
            ++age;
            if (peak < value && age <= refreshHz)
                return;
            value = peak;
            age = 0;
        }
    };
    void timerCallback() override;
    void slider(const char *id, bool rotary);
    void combo(const char *id, const juce::StringArray &);
    void toggle(const char *id);
    void drawGraph(juce::Graphics &);
    void paintContent(juce::Graphics &);
    // Holds every control at the design size. resized() scales it.
    struct Content final : juce::Component {
        explicit Content(WallEditor &owner) : editor(owner) {}
        void paint(juce::Graphics &g) override { editor.paintContent(g); }
        WallEditor &editor;
    };
    WallProcessor &wallProcessor;
    WallLook look;
    Content content{*this};
    juce::Image fortress;
    std::unique_ptr<juce::Drawable> ceilingMark;
    std::map<juce::String, std::unique_ptr<juce::Slider>> sliders;
    std::map<juce::String, std::unique_ptr<juce::ComboBox>> combos;
    std::map<juce::String, std::unique_ptr<juce::TextButton>> buttons;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ComboAttachment>> comboAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;
    std::array<juce::TextButton, 5> modes;
    juce::ComboBox presets;
    juce::TextButton prevPreset{"Previous"}, nextPreset{"Next"};
    juce::TextButton reset{"Reset meters"}, link{"Link levels"};
    juce::TextButton undoButton{"Undo"}, redoButton{"Redo"};
    juce::TextButton abA{"A"}, abB{"B"}, abAToB{"A>B"}, abBToA{"B>A"};
    juce::TextButton libraryButton{"Library"}, matchLoudness{"Drive to target"};
    Readout gainReduction{"gain_reduction", "Gain reduction", 22}, shortTerm{"short_term", "Short-term", 22},
        integrated{"integrated", "Integrated", 22}, truePeak{"true_peak_reading", "True peak", 22},
        inputLevel{"input_level", "Input", 23}, outputLevel{"output_level", "Output", 23};
    juce::Label guidance;
    std::unique_ptr<suite::PresetLibrary> presetLibrary;
    std::unique_ptr<suite::PresetBrowser> presetBrowser;
    juce::TooltipWindow tooltip{this, 650};
    std::array<float, 240> inputHistory{}, outputHistory{}, reductionHistory{};
    PeakHold inputHold{-100}, outputHold{-100}, reductionHold{0};
    bool inputClipped = false, outputClipped = false;
    int cursor = 0;
    double lastThreshold = 0, lastCeiling = -1;
    bool linking = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WallEditor)
};
