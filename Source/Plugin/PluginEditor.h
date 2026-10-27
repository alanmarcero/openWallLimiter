#pragma once
#include "PluginProcessor.h"
#include <map>
class WallLook final : public juce::LookAndFeel_V4 {
  public:
    WallLook();
    void drawRotarySlider(juce::Graphics &, int, int, int, int, float, float, float, juce::Slider &) override;
    void drawLinearSlider(juce::Graphics &, int, int, int, int, float, float, float,
                          juce::Slider::SliderStyle, juce::Slider &) override;
    void drawButtonBackground(juce::Graphics &, juce::Button &, const juce::Colour &, bool, bool) override;
};
class WallEditor final : public juce::AudioProcessorEditor, private juce::Timer {
  public:
    explicit WallEditor(WallProcessor &);
    ~WallEditor() override;
    void paint(juce::Graphics &) override;
    void resized() override;

  private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    void timerCallback() override;
    void slider(const char *id, const char *suffix, bool rotary);
    void combo(const char *id, const juce::StringArray &);
    void toggle(const char *id, const char *label);
    void drawGraph(juce::Graphics &);
    WallProcessor &wallProcessor;
    WallLook look;
    juce::Image fortress;
    std::map<juce::String, std::unique_ptr<juce::Slider>> sliders;
    std::map<juce::String, std::unique_ptr<juce::ComboBox>> combos;
    std::map<juce::String, std::unique_ptr<juce::TextButton>> buttons;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ComboAttachment>> comboAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;
    std::array<juce::TextButton, 5> modes;
    juce::ComboBox presets;
    juce::TextButton reset{"Reset meters"}, link{"Link"};
    juce::TooltipWindow tooltip{this, 650};
    std::array<float, 240> inputHistory{}, outputHistory{}, reductionHistory{};
    int cursor = 0;
    double lastThreshold = 0, lastCeiling = -1;
    bool linking = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WallEditor)
};
