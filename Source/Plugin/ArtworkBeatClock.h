#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

// Written once per audio block. Only the published phase is read by the editor.
class ArtworkBeatClock {
public:
    void reset() noexcept {
        nextBeat = 0.0;
        tempo = 120.0;
        phase.store(0.0f, std::memory_order_relaxed);
    }

    void update(const juce::Optional<juce::AudioPlayHead::PositionInfo>& position,
                int frames, double sampleRate) noexcept {
        if (position) {
            if (!position->getIsPlaying()) return;
            if (const auto bpm = position->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0.0)
                tempo = *bpm;
            if (const auto ppq = position->getPpqPosition(); ppq && std::isfinite(*ppq))
                nextBeat = *ppq - std::floor(*ppq);
        }
        phase.store(static_cast<float>(nextBeat), std::memory_order_relaxed);
        if (sampleRate > 0.0) {
            nextBeat += frames * tempo / (60.0 * sampleRate);
            nextBeat -= std::floor(nextBeat);
        }
    }

    float currentPhase() const noexcept { return phase.load(std::memory_order_relaxed); }

private:
    double nextBeat = 0.0;
    double tempo = 120.0;
    std::atomic<float> phase {0.0f};
};
