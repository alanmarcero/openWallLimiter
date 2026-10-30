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
    // Moving average of the newest `length` values in a fixed-capacity ring.
    struct Box {
        std::vector<double> values;
        std::size_t position = 0;
        int length = 1;
        double sum = 1, sumCorrection = 0;
        void reset() noexcept;
        void resize(int newLength) noexcept;
        double push(double value) noexcept;
    };
    struct Channel {
        std::vector<double> audio, allowances, gains;
        Minimum minimum;
        Box first, second;
        double releaseDb = 0, roundDb = 0, sustainDb = 0;
    };
    std::array<Channel, 2> channels_;
    Settings settings_;
    double sampleRate_ = 48000;
    double drive_ = 1, driveTarget_ = 1, driveSlew_ = 0;
    double ceiling_ = .8912509381337456;
    double upwardGain_ = 1, upwardEnvelope_ = 0;
    double upwardAttack_ = 0, upwardRelease_ = 0;
    double releaseFast_ = 0, releaseSlow_ = 0, sustainAttack_ = 0, sustainRelease_ = 0, classicStepDb_ = 0;
    double clipDb_ = 0;
    int channelCount_ = 1, latency_ = 0, attack_ = 0, hold_ = 0;
    std::size_t position_ = 0;
    std::uint64_t time_ = 0;
    float reductionDb_ = 0;
    bool started_ = false;
    double releaseDb(Channel&, double heldDb) const noexcept;
    double shape(double sample, double allowance) const noexcept;
};

} // namespace wall
