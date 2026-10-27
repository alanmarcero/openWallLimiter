#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
namespace wall {
juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout();
inline const juce::StringArray modeNames{"Modern", "Smooth", "Aggressive", "Safe", "Classic"};
inline const juce::StringArray slopeNames{"12 dB/oct", "24 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct"};
} // namespace wall
