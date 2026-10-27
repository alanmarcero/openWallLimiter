#include "ButterworthCoeffs.h"
#include <algorithm>
#include <numbers>

namespace analog_master::core {

int slopeToOrder(FilterSlope slope) noexcept {
    switch (slope) {
        case FilterSlope::Slope12dB: return 2;
        case FilterSlope::Slope24dB: return 4;
        case FilterSlope::Slope48dB: return 8;
        case FilterSlope::Slope72dB: return 12;
        case FilterSlope::Slope96dB: return 16;
    }
    return 4;
}

const char* slopeToString(FilterSlope slope) noexcept {
    switch (slope) {
        case FilterSlope::Slope12dB: return "12 dB/oct";
        case FilterSlope::Slope24dB: return "24 dB/oct";
        case FilterSlope::Slope48dB: return "48 dB/oct";
        case FilterSlope::Slope72dB: return "72 dB/oct";
        case FilterSlope::Slope96dB: return "96 dB/oct";
    }
    return "24 dB/oct";
}

FilterDesign designButterworthLowPass(
    double sampleRate,
    double cutoffHz,
    int order
) noexcept {
    if (sampleRate <= 0.0) {
        return {};
    }

    const int safeOrder = std::clamp((order / 2) * 2, 2, 16);
    const double maxCutoff = sampleRate * 0.4995;
    const double safeCutoff = std::clamp(cutoffHz, 10.0, maxCutoff);

    const double K = std::tan(std::numbers::pi * safeCutoff / sampleRate);
    const double K2 = K * K;
    const int numSections = safeOrder / 2;

    FilterDesign design;
    design.count = numSections;

    for (int k = 0; k < numSections; ++k) {
        const double theta = (2.0 * k + 1.0) * std::numbers::pi / (2.0 * safeOrder);
        const double Q = 1.0 / (2.0 * std::cos(theta));

        const double norm = 1.0 / (1.0 + (K / Q) + K2);
        const double b0 = K2 * norm;
        const double b1 = 2.0 * b0;
        const double b2 = b0;
        const double a1 = 2.0 * (K2 - 1.0) * norm;
        const double a2 = (1.0 - (K / Q) + K2) * norm;

        design.sections[static_cast<size_t>(k)] = { b0, b1, b2, a1, a2 };
    }

    return design;
}

FilterDesign designButterworthHighPass(
    double sampleRate,
    double cutoffHz,
    int order
) noexcept {
    if (sampleRate <= 0.0) {
        return {};
    }

    const int safeOrder = std::clamp((order / 2) * 2, 2, 16);
    const double maxCutoff = sampleRate * 0.49;
    const double safeCutoff = std::clamp(cutoffHz, 5.0, maxCutoff);

    const double K = std::tan(std::numbers::pi * safeCutoff / sampleRate);
    const double K2 = K * K;
    const int numSections = safeOrder / 2;

    FilterDesign design;
    design.count = numSections;

    for (int k = 0; k < numSections; ++k) {
        const double theta = (2.0 * k + 1.0) * std::numbers::pi / (2.0 * safeOrder);
        const double Q = 1.0 / (2.0 * std::cos(theta));

        const double norm = 1.0 / (1.0 + (K / Q) + K2);
        const double b0 = norm;
        const double b1 = -2.0 * norm;
        const double b2 = norm;
        const double a1 = 2.0 * (K2 - 1.0) * norm;
        const double a2 = (1.0 - (K / Q) + K2) * norm;

        design.sections[static_cast<size_t>(k)] = { b0, b1, b2, a1, a2 };
    }

    return design;
}

} // namespace analog_master::core
