#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <optional>

namespace wall {
inline constexpr float lufsStreaming = -14.f;
inline constexpr float lufsClub = -9.f;
inline constexpr float lufsCustomMin = -24.f;
inline constexpr float lufsCustomMax = -6.f;
inline constexpr float inputGainMin = -12.f;
inline constexpr float inputGainMax = 12.f;

// mode: 0 off, 1 streaming, 2 club, 3 custom.
inline std::optional<float> targetLufs(int mode, float custom) noexcept {
    switch (mode) {
    case 1: return lufsStreaming;
    case 2: return lufsClub;
    case 3: return std::clamp(custom, lufsCustomMin, lufsCustomMax);
    default: return std::nullopt;
    }
}

// Shift input drive so a BS.1770 reading lands on the target. Invalid readings are left alone.
inline float matchedInputGain(float currentDb, float measuredLufs, float target) noexcept {
    const float current = std::clamp(std::isfinite(currentDb) ? currentDb : 0.f, inputGainMin, inputGainMax);
    if (!std::isfinite(measuredLufs) || measuredLufs <= -99.f || !std::isfinite(target))
        return current;
    return std::clamp(current + (target - measuredLufs), inputGainMin, inputGainMax);
}

inline juce::String latencyReadout(int samples, double sampleRate) {
    const double ms = sampleRate > 0.0 ? double(samples) * 1000.0 / sampleRate : 0.0;
    return juce::String(ms, 2) + " ms (" + juce::String(samples) + " samples)";
}

inline juce::String loudnessGuidance(int mode, float custom, float integrated, float shortTerm) {
    const auto target = targetLufs(mode, custom);
    if (!target)
        return "Loudness target off";
    const bool integratedReady = std::isfinite(integrated) && integrated > -99.f;
    const bool shortReady = std::isfinite(shortTerm) && shortTerm > -99.f;
    const juce::String targetText = juce::String(*target, 1) + " LUFS";
    if (!integratedReady && !shortReady)
        return "Target " + targetText + "  ·  measuring";
    const float measured = integratedReady ? integrated : shortTerm;
    const float delta = measured - *target;
    juce::String relation;
    if (std::abs(delta) < 0.1f)
        relation = "on target";
    else if (delta > 0.f)
        relation = juce::String(delta, 1) + " LU over";
    else
        relation = juce::String(-delta, 1) + " LU under";
    return (integratedReady ? juce::String("Integrated ") : juce::String("Short-term ")) + juce::String(measured, 1)
         + " LUFS  ·  " + relation + " " + targetText;
}
} // namespace wall
