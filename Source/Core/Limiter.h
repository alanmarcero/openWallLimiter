#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wall {

enum class Mode { Modern, Smooth, Aggressive, Safe, Classic };

struct Settings {
    float thresholdDb = 0;
    float ceilingDb = -1;
    float release = 1;
    float clip = 0;
    float upwardDb = 0;
    float stereoLink = 1;
    Mode mode = Mode::Modern;
};

// Single audio-thread owner. Call prepare outside the audio callback.
class Limiter {
public:
    void prepare(double sampleRate, int channels);
    void reset() noexcept;
    void setSettings(const Settings&) noexcept;
    void process(float* left, float* right, int samples) noexcept;
    [[nodiscard]] int latencySamples() const noexcept { return latency_; }
    [[nodiscard]] float gainReductionDb() const noexcept { return reductionDb_; }

private:
    struct Minimum {
        struct Entry { double value = 1; std::uint64_t time = 0; };
        std::vector<Entry> entries;
        std::size_t head = 0, size = 0;
        void reset() noexcept;
        double push(double value, std::uint64_t time, int window) noexcept;
    };
    struct Channel {
        std::vector<double> audio, gains;
        Minimum minimum;
        double sum = 0, sumCorrection = 0;
        double fast = 1, slow = 1;
    };
    std::array<Channel, 2> channels_;
    Settings settings_;
    double sampleRate_ = 48000;
    double drive_ = 1, driveTarget_ = 1, driveSlew_ = 0;
    double ceiling_ = .8912509381337456;
    double upwardGain_ = 1, upwardEnvelope_ = 0;
    double upwardAttack_ = 0, upwardRelease_ = 0;
    double releaseFast_ = 0, releaseSlow_ = 0, classicSlew_ = 1;
    double clipAllowance_ = 1, clipKnee_ = 0;
    int channelCount_ = 1, latency_ = 0;
    std::size_t position_ = 0;
    std::uint64_t time_ = 0;
    float reductionDb_ = 0;
    bool started_ = false;
    double releaseGain(Channel&, double target) const noexcept;
    double shape(double sample) const noexcept;
};

} // namespace wall
