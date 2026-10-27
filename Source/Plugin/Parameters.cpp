#include "Parameters.h"
namespace wall {
juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto number = [&](const char *id, const char *name, float lo, float hi, float step, float def,
                            float skew, const char *unit) {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>{lo, hi, step, skew}, def,
            juce::AudioParameterFloatAttributes{}.withLabel(unit)));
    };
    const auto boolean = [&](const char *id, const char *name, bool def) {
        layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id, 1}, name, def));
    };
    const auto choice = [&](const char *id, const char *name, const juce::StringArray &values, int def) {
        layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id, 1}, name, values, def));
    };
    number("threshold", "Threshold", -30, 0, .01f, 0, 1, "dB");
    number("ceiling", "Ceiling", -30, 0, .01f, -1, 1, "dB");
    choice("mode", "Algorithm", modeNames, 0);
    number("release", "Release", .1f, 10, .01f, 1, .4f, "x");
    number("clip", "Clip", 0, 100, .1f, 0, 1, "%");
    number("upward", "Upward", 0, 10, .01f, 0, 1, "dB");
    number("stereo_link", "Stereo Link", 0, 100, .1f, 100, 1, "%");
    choice("oversampling", "Oversampling", {"Off", "2x", "4x", "8x", "16x"}, 2);
    boolean("true_peak", "True Peak", true);
    choice("dither", "Dither", {"Off", "24-bit TPDF", "16-bit TPDF"}, 0);
    boolean("gain_match", "Gain Match", false);
    boolean("delta", "Delta", false);
    boolean("bypass", "Bypass", false);
    number("highpass_hz", "High Pass Frequency", 10, 200, .1f, 20, .35f, "Hz");
    choice("highpass_slope", "High Pass Slope", slopeNames, 3);
    boolean("highpass_on", "High Pass", true);
    number("lowpass_hz", "Low Pass Frequency", 8000, 24000, 10, 20000, 1, "Hz");
    choice("lowpass_slope", "Low Pass Slope", slopeNames, 3);
    boolean("lowpass_on", "Low Pass", true);
    return layout;
}
} // namespace wall
