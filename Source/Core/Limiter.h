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
    float releaseLink = 0;
    float lookaheadMs = 5;
    Mode mode = Mode::Modern;
};

// Single audio-thread owner. Call prepare outside the audio callback.
class Limiter {
public:
    static constexpr float maxLookaheadMs = 10.0f;

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
    struct TptBiquad {
        double a1 = 1, a2 = 0, a3 = 0, m0 = 1, m1 = 0, m2 = 0;
        double ic1 = 0, ic2 = 0;
        void set(double b0, double b1, double b2, double d1, double d2) noexcept;
        void setButterworth(double cutoffHz, double q, double sampleRate, bool highpass) noexcept;
        void reset() noexcept { ic1 = ic2 = 0; }
        double process(double x) noexcept;
    };
    struct Envelope {
        std::vector<double> gains;
        Minimum minimum;
        std::array<Box, 4> stages;
        double releaseDb = 0, roundDb = 0, sustainDb = 0;
        void reset() noexcept;
    };
    struct Channel {
        std::vector<double> audio, audioSub, allowances;
        Envelope envelope;
        std::array<TptBiquad, 2> kFilter{}, subLpFilter{}, subHpFilter{}, hfHpFilter{};
        double subEnergy = 0, hiEnergy = 0;
        double kMeanSq = 0, rmsMeanSq = 0, peakEnv = 0, hfFast = 0, hfSlow = 0, fullSlow = 0;
    };
    std::array<Channel, 2> channels_;
    Envelope subEnvelope_;
    Settings settings_;
    double sampleRate_ = 48000;
    double drive_ = 1, driveTarget_ = 1, driveSlew_ = 0;
    double ceiling_ = .8912509381337456;
    double upwardGain_ = 1, upwardEnergy_ = 0;
    double upwardAttack_ = 0, upwardRelease_ = 0;
    double kRmsSlew_ = 0, peakRelease_ = 0;
    double hfFastAttack_ = 0, hfFastRelease_ = 0, hfSlowSlew_ = 0;
    double midEnv_ = 0, sideEnv_ = 0, msSlew_ = 0;
    double releaseFast_ = 0, releaseSlow_ = 0, sustainAttack_ = 0, sustainRelease_ = 0, classicStepDb_ = 0;
    double clipDb_ = 0;
    int channelCount_ = 1, latency_ = 0, lookahead_ = 0, attack_ = -1, hold_ = 0;
    std::size_t position_ = 0;
    std::uint64_t time_ = 0;
    float reductionDb_ = 0;
    bool started_ = false;
    double advanceRelease(Envelope&, double heldDb, double kOverDb, double crest) const noexcept;
    double shape(double sample, double allowance) const noexcept;
};

} // namespace wall
