#pragma once

#include <algorithm>
#include <cmath>

namespace analog_master::core {

struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// Topology-Preserving Transform (TPT) State Variable Filter Biquad Section
// Exact transfer function equivalence to Direct Form biquad (within ~1e-15),
// with unconditional stability and zero transient gain explosion during fast cutoff modulation.
class BiquadFilter {
public:
    BiquadFilter() = default;

    void setCoefficients(const BiquadCoeffs& coeffs) noexcept {
        coeffs_ = coeffs;

        const double dPlus = std::max(1e-18, 1.0 + coeffs.a1 + coeffs.a2);
        const double dMinus = std::max(1e-18, 1.0 - coeffs.a1 + coeffs.a2);
        const double rootProd = std::sqrt(dPlus * dMinus);

        const double g = std::sqrt(dPlus / dMinus);
        const double kDamp = 2.0 * (1.0 - coeffs.a2) / rootProd;

        a1_ = 1.0 / (1.0 + g * (g + kDamp));
        a2_ = g * a1_;
        a3_ = g * a2_;

        const double mHp = (coeffs.b0 - coeffs.b1 + coeffs.b2) / dMinus;
        const double mBp = 2.0 * (coeffs.b0 - coeffs.b2) / rootProd;
        const double mLp = (coeffs.b0 + coeffs.b1 + coeffs.b2) / dPlus;

        m0_ = mHp;
        m1_ = mBp - mHp * kDamp;
        m2_ = mLp - mHp;
    }

    void reset() noexcept {
        ic1eq_ = 0.0;
        ic2eq_ = 0.0;
    }

    [[nodiscard]] float process(float sample) noexcept {
        // Sanitize non-finite inputs (NaN / Inf protection)
        if (!std::isfinite(sample)) {
            reset();
            return 0.0f;
        }

        const double v0 = static_cast<double>(sample);
        const double v3 = v0 - ic2eq_;
        const double v1 = a1_ * ic1eq_ + a2_ * v3;
        const double v2 = ic2eq_ + a2_ * ic1eq_ + a3_ * v3;

        ic1eq_ = 2.0 * v1 - ic1eq_;
        ic2eq_ = 2.0 * v2 - ic2eq_;

        const double out = m0_ * v0 + m1_ * v1 + m2_ * v2;

        // Catch non-finite filter divergence / instability
        if (!std::isfinite(out)) {
            reset();
            return 0.0f;
        }

        // Flush denormals
        if (std::abs(ic1eq_) < 1e-25) {
            ic1eq_ = 0.0;
        }
        if (std::abs(ic2eq_) < 1e-25) {
            ic2eq_ = 0.0;
        }

        return static_cast<float>(out);
    }

    [[nodiscard]] const BiquadCoeffs& coefficients() const noexcept {
        return coeffs_;
    }

private:
    BiquadCoeffs coeffs_{};
    double a1_ = 1.0;
    double a2_ = 0.0;
    double a3_ = 0.0;
    double m0_ = 1.0;
    double m1_ = 0.0;
    double m2_ = 0.0;
    double ic1eq_ = 0.0;
    double ic2eq_ = 0.0;
};

} // namespace analog_master::core
