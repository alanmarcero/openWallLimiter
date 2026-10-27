#pragma once
#include "BiquadFilter.h"
#include <array>
#include <cstdint>
#include <limits>
#include <vector>
namespace analog_master::core {
// BS.1770 K weighting and gates; 10 ms energy buckets, 0.001 LU integrated histogram.
// Owned by the audio thread. The UI reads atomics published by MasterProcessorCore.
class LoudnessMeter {
public:
    void prepare(double sampleRate, int channels, bool integrated = true);
    void reset() noexcept;
    void process(float left, float right) noexcept;
    [[nodiscard]] float shortTermLufs() const noexcept { return shortTerm_; }
    [[nodiscard]] float integratedLufs() const noexcept { return integrated_; }
    [[nodiscard]] bool shortTermReady() const noexcept { return ticks_ >= 300; }
    [[nodiscard]] bool integratedReady() const noexcept { return ticks_ >= 40; }

private:
    void finishBucket() noexcept;
    static float lufs(double energy) noexcept;
    static constexpr int binsPerLu = 1000;
    static constexpr int binCount = 140 * binsPerLu;
    struct Bin {
        double energy = 0;
        std::uint64_t count = 0, epoch = 0;
    };
    void addEnergy(int index, double energy) noexcept;
    [[nodiscard]] Bin prefixThrough(int index) const noexcept;
    // A Fenwick prefix tree makes each integrated update O(log bins), not a scan.
    // Epoch tags make reset O(1); only the output meter allocates this history.
    std::vector<Bin> histogram_, prefix_;
    std::uint64_t epoch_ = 0;
    std::array<double, 300> energies_{};
    std::array<std::array<BiquadFilter, 2>, 2> filters_{};
    double bucketEnergy_ = 0, shortSum_ = 0, momentarySum_ = 0, gatedSum_ = 0;
    std::uint64_t gatedCount_ = 0, ticks_ = 0;
    int bucketSamples_ = 480, frame_ = 0, cursor_ = 0, channels_ = 2;
    bool measureIntegrated_ = true;
    float shortTerm_ = -std::numeric_limits<float>::infinity();
    float integrated_ = -std::numeric_limits<float>::infinity();
};
} // namespace analog_master::core
