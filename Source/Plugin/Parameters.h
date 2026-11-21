#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
namespace wall {
juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();
// Rounds to `decimals` places and never shows "-0.0".
juce::String fixed(float value, int decimals);
// Shows "4.5 ms" below 10 ms, "160 ms" up to 1 s, and "1.20 s" above.
juce::String milliseconds(float ms);
inline const juce::StringArray modeNames{"Modern", "Smooth", "Aggressive", "Safe", "Classic"};
inline const juce::StringArray slopeNames{"12 dB/oct", "24 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct"};
inline const juce::StringArray lufsModeNames{"Off", "Streaming -14", "Club -9", "Custom"};
} // namespace wall
