#include "Limiter.h"
#include "LoudnessTarget.h"
#include "Parameters.h"
namespace wall {
juce::String fixed(float value, int decimals) {
    const float scale = std::pow(10.f, float(decimals));
    const float rounded = std::round(value * scale) / scale;
    return juce::String(rounded == 0.f ? 0.f : rounded, decimals);
}
juce::String milliseconds(float ms) {
    if (ms < 9.95f)
        return fixed(ms, 1) + " ms";
    if (ms < 999.5f)
        return juce::String(juce::roundToInt(ms)) + " ms";
    return fixed(ms / 1000.f, 2) + " s";
}
juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    using Text = std::function<juce::String(float)>;
    using Parse = std::function<float(const juce::String &)>;
    const auto number = [&](const char *id, const char *name, float lo, float hi, float step, float def,
                            float skew, Text text, Parse parse) {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>{lo, hi, step, skew}, def,
            juce::AudioParameterFloatAttributes{}
                .withStringFromValueFunction([text](float value, int length) {
                    const auto shown = text(value);
                    return length > 0 ? shown.substring(0, length) : shown;
                })
                .withValueFromStringFunction(parse)));
    };
    const auto boolean = [&](const char *id, const char *name, bool def) {
        layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id, 1}, name, def));
    };
    const auto choice = [&](const char *id, const char *name, const juce::StringArray &values, int def) {
        layout.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{id, 1}, name, values, def));
    };
    const Parse plain = [](const juce::String &text) { return text.getFloatValue(); };
    const Parse hertz = [](const juce::String &text) {
        return text.getFloatValue() * (text.containsIgnoreCase("k") ? 1000.f : 1.f);
    };
    const Parse millis = [](const juce::String &text) {
        const auto trimmed = text.trim();
        const bool seconds = trimmed.endsWithIgnoreCase("s") && !trimmed.endsWithIgnoreCase("ms");
        return trimmed.getFloatValue() * (seconds ? 1000.f : 1.f);
    };
    const Text gain = [](float value) {
        const auto shown = fixed(value, 1);
        return (shown.getFloatValue() > 0 ? "+" : "") + shown + " dB";
    };
    const Text percent = [](float value) { return juce::String(juce::roundToInt(value)) + "%"; };
    const Text frequency = [](float value) {
        if (value < 999.5f)
            return juce::String(juce::roundToInt(value)) + " Hz";
        return fixed(value / 1000.f, 2) + " kHz";
    };
    // Factory ceilings use 0.05 dB steps, so the hundredths digit shows when it is not zero.
    const Text ceiling = [](float value) {
        return fixed(value, juce::roundToInt(value * 100.f) % 10 != 0 ? 2 : 1) + " dB";
    };
    const Text multiple = [](float value) {
        return fixed(value, juce::roundToInt(value * 100.f) % 10 != 0 ? 2 : 1) + "x";
    };
    number("threshold", "Threshold", -30, 0, .01f, 0, 1, gain, plain);
    number("ceiling", "Ceiling", -30, 0, .01f, -1, 1, ceiling, plain);
    choice("mode", "Style", modeNames, 0);
    number("release", "Release", .1f, 10, .01f, 1, .4f, multiple, plain);
    number("clip", "Clip", 0, 100, .1f, 0, 1, percent, plain);
    number("upward", "Upward", 0, 10, .01f, 0, 1, gain, plain);
    number("stereo_link", "Transient link", 0, 100, .1f, 100, 1, percent, plain);
    choice("oversampling", "Oversampling", {"Off", "2x", "4x", "8x", "16x"}, 2);
    boolean("true_peak", "True peak", true);
    choice("dither", "Dither", {"Off", "24-bit Shaped", "16-bit Shaped"}, 0);
    boolean("gain_match", "Gain match", false);
    boolean("delta", "Delta", false);
    boolean("bypass", "Bypass", false);
    number("highpass_hz", "High pass frequency", 10, 200, .1f, 20, .35f, frequency, hertz);
    choice("highpass_slope", "High pass slope", slopeNames, 3);
    boolean("highpass_on", "High pass", true);
    number("lowpass_hz", "Low pass frequency", 8000, 24000, 10, 20000, 1, frequency, hertz);
    choice("lowpass_slope", "Low pass slope", slopeNames, 3);
    boolean("lowpass_on", "Low pass", true);
    number("lookahead_ms", "Lookahead", 0, Limiter::maxLookaheadMs, .01f, 5, 1, milliseconds, millis);
    number("release_link", "Release link", 0, 100, .1f, 100, 1, percent, plain);
    number("input_gain", "Input drive", -12, 12, .1f, 0, 1, gain, plain);
    number("stereo_width", "Stereo width", 0, 200, .5f, 100, 1, percent, plain);
    number("tone_tilt", "Spectral tilt", -6, 6, .1f, 0, 1, gain, plain);
    choice("lufs_mode", "Loudness target", lufsModeNames, 0);
    number("lufs_target", "Custom target", lufsCustomMin, lufsCustomMax, .1f, lufsStreaming, 1,
           [](float value) { return fixed(value, 1) + " LUFS"; }, plain);
    return layout;
}
} // namespace wall
