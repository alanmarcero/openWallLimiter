#pragma once

#include "BiquadFilter.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace analog_master::core {

enum class FilterSlope : uint8_t {
    Slope12dB = 0,
    Slope24dB,
    Slope48dB,
    Slope72dB,
    Slope96dB
};

constexpr int kMaxFilterSections = 8; // Supports up to 16 poles (96 dB/oct)

// Stack-allocated filter design result - 100% realtime-safe, zero heap allocation
struct FilterDesign {
    std::array<BiquadCoeffs, kMaxFilterSections> sections{};
    int count = 0;

    [[nodiscard]] size_t size() const noexcept { return static_cast<size_t>(count); }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] const BiquadCoeffs& operator[](size_t idx) const noexcept { return sections[idx]; }
    [[nodiscard]] BiquadCoeffs& operator[](size_t idx) noexcept { return sections[idx]; }

    auto begin() noexcept { return sections.begin(); }
    auto end() noexcept { return sections.begin() + count; }
    auto begin() const noexcept { return sections.begin(); }
    auto end() const noexcept { return sections.begin() + count; }
};

[[nodiscard]] int slopeToOrder(FilterSlope slope) noexcept;
[[nodiscard]] const char* slopeToString(FilterSlope slope) noexcept;

[[nodiscard]] FilterDesign designButterworthLowPass(
    double sampleRate,
    double cutoffHz,
    int order
) noexcept;

[[nodiscard]] FilterDesign designButterworthHighPass(
    double sampleRate,
    double cutoffHz,
    int order
) noexcept;

} // namespace analog_master::core
