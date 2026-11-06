// Sound-quality probe. Renders standard distortion signals through every mode at heavy
// limiting and prints objective measures. Lower distortion numbers are better; loudness
// is printed so that a cleaner result is not simply a quieter one.
#include "HostChecks.h"
#include <complex>
#include <cstdio>

namespace {
constexpr double rate = 48000;
constexpr double pi = std::numbers::pi;

double toneLevel(const std::vector<float> &x, size_t start, size_t length, double hz) {
    // Hann-windowed single-bin DFT, normalized to sine amplitude.
    std::complex<double> sum{};
    double windowSum = 0;
    for (size_t i = 0; i < length; ++i) {
        const double w = .5 - .5 * std::cos(2 * pi * double(i) / double(length));
        sum += w * double(x[start + i]) * std::polar(1.0, -2 * pi * hz * double(i) / rate);
        windowSum += w;
    }
    return 2 * std::abs(sum) / windowSum;
}

double db(double v) { return 20 * std::log10(std::max(v, 1e-12)); }

double rms(const std::vector<float> &x, size_t start) {
    double e = 0;
    for (size_t i = start; i < x.size(); ++i)
        e += double(x[i]) * x[i];
    return std::sqrt(e / double(x.size() - start));
}

void configure(WallProcessor &p, int mode, float threshold, bool truePeak) {
    checks::set(p, "highpass_on", 0);
    checks::set(p, "lowpass_on", 0);
    checks::set(p, "mode", float(mode));
    checks::set(p, "threshold", threshold);
    checks::set(p, "ceiling", -1);
    checks::set(p, "true_peak", truePeak ? 1.f : 0.f);
    p.prepareToPlay(rate, 512);
}

std::vector<float> run(int mode, float threshold, bool truePeak, const std::vector<float> &x) {
    WallProcessor p;
    configure(p, mode, threshold, truePeak);
    return checks::render(p, x, 512);
}

// THD of a loud bass sine, harmonics 2..12, measured over the final second.
double bassThd(int mode, double hz, bool truePeak) {
    const auto x = checks::tone(int(rate * 3), rate, hz, 1.f);
    const auto y = run(mode, -10, truePeak, x);
    const size_t start = y.size() - size_t(rate), length = size_t(rate);
    const double fundamental = toneLevel(y, start, length, hz);
    double harmonics = 0;
    for (int h = 2; h <= 12; ++h)
        harmonics += std::pow(toneLevel(y, start, length, hz * h), 2);
    return db(std::sqrt(harmonics) / fundamental);
}

// Bass modulating a treble tone: sidebands at 5 kHz +- k*hz relative to the 5 kHz tone.
double modulation(int mode, double hz, bool truePeak) {
    std::vector<float> x(size_t(rate * 3));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = float(.85 * std::sin(2 * pi * hz * double(i) / rate) + .15 * std::sin(2 * pi * 5000 * double(i) / rate));
    const auto y = run(mode, -10, truePeak, x);
    const size_t start = y.size() - size_t(rate), length = size_t(rate);
    const double carrier = toneLevel(y, start, length, 5000);
    double sidebands = 0;
    for (int k = 1; k <= 8; ++k)
        sidebands += std::pow(toneLevel(y, start, length, 5000 + k * hz), 2) +
                     std::pow(toneLevel(y, start, length, 5000 - k * hz), 2);
    return db(std::sqrt(sidebands) / carrier);
}

// A drum-and-bass program over a sparse multitone. Energy more than 25 Hz away from any
// input tone counts as distortion (slow pumping lands close to the tones, waveform-rate
// gain modulation and clipping land far from them).
struct ProgramResult { double distortion, pumping, loudness; };
constexpr std::array<double, 9> tones{41, 97, 211, 487, 1031, 2153, 4447, 8963, 13001};

std::vector<float> programSignal() {
    // The kick repeats every 2^15 samples, so the 2^18-sample kick track is exactly periodic
    // and a brickwall in the frequency domain removes all of its energy above 300 Hz.
    constexpr int order = 18;
    constexpr size_t length = size_t(1) << order, period = size_t(1) << 15;
    std::vector<float> kick(2 * length, 0.f);
    for (size_t i = 0; i < length; ++i) {
        const double beat = double(i % period) / rate;
        kick[i] = float(.9 * std::exp(-beat * 18) *
                        std::sin(2 * pi * (52 * beat + 60 * (1 - std::exp(-beat * 40)) / 40)));
    }
    juce::dsp::FFT fft(order);
    fft.performRealOnlyForwardTransform(kick.data());
    for (size_t bin = size_t(300 * double(length) / rate); bin <= length / 2; ++bin)
        kick[2 * bin] = kick[2 * bin + 1] = 0.f;
    fft.performRealOnlyInverseTransform(kick.data());
    std::vector<float> x(length);
    for (size_t i = 0; i < length; ++i) {
        const double t = double(i) / rate;
        double v = kick[i];
        for (size_t k = 0; k < tones.size(); ++k)
            v += (k < 2 ? .22 : .07) * std::sin(2 * pi * tones[k] * t + double(k));
        x[i] = float(v);
    }
    return x;
}

ProgramResult score(const std::vector<float> &y) {
    const size_t length = y.size();
    constexpr int order = 16;
    constexpr size_t n = size_t(1) << order;
    const size_t start = length - n;
    juce::dsp::FFT fft(order);
    std::vector<float> data(2 * n, 0.f);
    for (size_t i = 0; i < n; ++i)
        data[i] = y[start + i] * float(.5 - .5 * std::cos(2 * pi * double(i) / double(n)));
    fft.performFrequencyOnlyForwardTransform(data.data());
    // The kick has little energy above 400 Hz, so only that region is scored.
    double far = 0, total = 0, close = 0, tonal = 0;
    for (size_t bin = 1; bin < n / 2; ++bin) {
        const double hz = double(bin) * rate / double(n);
        if (hz > 18000)
            break;
        const double power = double(data[bin]) * data[bin];
        total += power;
        bool near = false;
        for (double t : tones) {
            const double distance = std::abs(hz - t);
            near = near || distance < 25;
            if (hz > 400 && distance < 3)
                tonal += power;
            if (hz > 400 && distance >= 3 && distance < 25)
                close += power;
        }
        if (!near && hz > 400)
            far += power;
    }
    return {10 * std::log10(std::max(far, 1e-24) / total), 10 * std::log10(std::max(close, 1e-24) / tonal),
            db(rms(y, start))};
}

ProgramResult program(int mode, float threshold, bool truePeak) {
    return score(run(mode, threshold, truePeak, programSignal()));
}
} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const char *names[] = {"Modern", "Smooth", "Aggressive", "Safe", "Classic"};
    const auto input = score(programSignal());
    std::printf("input program: far %.1f dB, close %.1f dB\n", input.distortion, input.pumping);
    for (bool tp : {true, false}) {
        std::printf("true peak %s (threshold -10 dB, ceiling -1 dB, 4x oversampling)\n", tp ? "on" : "off");
        std::printf("%-11s %8s %8s %8s %8s %8s %8s %10s %7s %9s\n", "mode", "thd30", "thd60", "thd120", "imd40",
                    "imd80", "imd160", "program", "pump", "rms");
        for (int mode = 0; mode < 5; ++mode) {
            const auto prog = program(mode, -8, tp);
            std::printf("%-11s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %10.1f %7.1f %9.2f\n", names[mode], bassThd(mode, 30, tp),
                        bassThd(mode, 60, tp), bassThd(mode, 120, tp), modulation(mode, 40, tp),
                        modulation(mode, 80, tp), modulation(mode, 160, tp), prog.distortion, prog.pumping, prog.loudness);
            std::fflush(stdout);
        }
    }
    return 0;
}
