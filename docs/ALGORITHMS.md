# Open Wall Limiter algorithms

This is an original C++20 limiter implementation. It provides five useful limiting
policies, rather than emulating a proprietary transfer curve or claiming to sound
identical to Waves L4 or L2. No Waves source code, presets, graphics, or measurements
were used.

## Public references

- [Waves L4 user guide](https://assets.wavescdn.com/pdf/plugins/l4-ultramaximizer.pdf):
  workflow reference for threshold/ceiling controls, release scaling, clipping,
  upward compression, and the five broad sonic choices. Our Classic policy is
  independently tuned, and does not reproduce the guide's L2 Legacy algorithm.
- [Faust compressors library, lookahead limiters](https://faustlibraries.grame.fr/libs/compressors/#lookahead-limiters):
  background on peak detection, channel linkage, delayed audio and gain smoothing.
  Faust notes that smoothing and lookahead affect ceiling accuracy; this core
  instead uses finite gain smoothing and an explicit final sample guard.
- [Geraint Luff, Designing a straightforward limiter](https://signalsmith-audio.co.uk/writing/2022/limiter/):
  mathematical background for combining a moving minimum with finite smoothing
  and delayed audio, for extending the minimum as a hold, and for cascading
  averages to round the gain curve. The ring queues and processing code here were
  written for this project; no Signalsmith implementation is included.

## Signal path and latency

`sanitized input -> threshold drive -> gated upward lift -> audio delay -> gain -> clip knee -> ceiling guard`

The parallel gain path computes a safe gain per channel and links it towards the
smaller gain. It holds the moving minimum, applies the selected release in dB, and
smooths the gain with two cascaded moving averages. A gain ring then aligns the
gain with the delayed audio. Input and internal gain state use double precision;
the public buffers contain floats.

Let `N = ceil(processing sample rate * 0.005)`. The audio delay is exactly `N`
samples in every mode. Each mode has an attack of `A <= N` samples and a hold of
`H` samples. The moving minimum spans `A+1+H` samples. The two averages have
lengths `L1 = floor((A+3)/2)` and `L2 = A+2-L1`, so together they span `A+1`
samples. The gain ring delays the smoothed gain by `N-A` samples. An input target
therefore constrains every gain term that reaches its own audio sample. Release
never exceeds the held target. The cascade gives an S-shaped gain ramp of `A`
samples, which starts and ends with zero slope. The hold keeps the gain flat
between waveform peaks, so the release does not follow a low-frequency cycle.

The target is `min(1, ceilingAmplitude * allowance / abs(drivenInput))`. The
allowance is 1 when Clip is 0. The final symmetric guard always limits sample
magnitude to the current ceiling, including after automation. Float rounding can
introduce approximately one float ULP at the boundary.

The caller must add any resampling filter delay to the reported delay. At
oversampling factor `F`, the core contributes `ceil(hostRate*F*.005)/F` host
samples; rounding this value early can produce an alignment error. This core does
not detect reconstructed intersample peaks. Oversampling and any true peak margin
belong to the plugin layer.

## Policies

The five modes follow the intent of the five L4 modes: Modern is balanced and
punchy, Smooth is transparent and relaxed, Aggressive is loud and forward, Safe
stays clean under heavy limiting, and Classic gives a steady legacy-style
recovery. The tuning below is this project's own.

The Release control multiplies the fast, slow, sustain and classic times. Attack
and hold do not scale. The exponential coefficient is `1 - exp(-1/(sampleRate*time))`.

| Mode | Attack | Hold | Release (fast / slow) | Sustain (rise / fall) | Clip allowance at Clip = 1 |
| --- | --- | --- | --- | --- | --- |
| Modern | 2.5 ms | 12 ms | 45 / 300 ms | 400 / 400 ms | 3 dB |
| Smooth | 5 ms | 12 ms | 60 / 350 ms, plus a 60 ms rounding stage | 300 / 350 ms | 1.5 dB |
| Aggressive | 1 ms | 3 ms | 12 / 150 ms | 600 / 200 ms | 5 dB |
| Safe | 5 ms | 25 ms | 100 / 450 ms, plus a 100 ms rounding stage | 250 / 400 ms | 0 dB; Clip has no effect |
| Classic | 1.5 ms | 10 ms | Constant 40 dB per second | 400 / 400 ms | 3 dB |

The release runs in dB. A sustain detector follows the held attenuation with the
rise and fall times above. Attenuation above the sustain level counts as a
transient and releases with the fast time. Attenuation below it releases with the
slow time. A short peak therefore recovers quickly, while dense material keeps a
steady gain and does not modulate its own waveform. Classic replaces both times
with a constant-rate recovery and keeps the sustain detector for clipping. Smooth
and Safe pass the release through a second one-pole stage that rounds the
recovery corner.

On a mode change, both release stages start from the current attenuation. The
lookahead and smoothing histories continue, and the averages recompute their sums
for the new attack. Returning to a rounded mode therefore cannot revive old
attenuation. Because each mode aligns its own attack, a mode change during a
release can step the gain by up to about 0.2 dB.

The clip allowance in dB is `clipDb * Clip / (1 + sustainDb^2)`. Sustained
limiting therefore shrinks the allowance towards 0 dB, and clipping applies mainly
to transients. Each delayed sample carries the allowance that its target used. A
quadratic knee starts at `ceiling * (2 - allowance)` and reaches the ceiling with
zero slope at `ceiling * allowance`, the largest magnitude the gain path passes.
With an allowance of 1, the knee does nothing. Safe never clips intentionally.

Threshold adds `-thresholdDb` of input drive independently of the output ceiling.
During playback, drive changes follow a 5 ms exponential time constant at the
processing sample rate. Prepare/reset and changes before the first processed sample
initialize drive directly to the requested value, avoiding a startup fade.
Lowering the ceiling alone does not raise quiet signals. Stereo Link interpolates
each independent target toward `min(leftTarget, rightTarget)`; it can only reduce
the permitted gain. Full linkage preserves channel ratios when its gain histories
match. Changing linkage during an existing release can take time to synchronize
the histories. Upward lift uses a shared channel-maximum envelope so that its own
gain does not change stereo balance.

The upward envelope has a 5 ms attack and 80 ms release. A linear amplitude gate
opens between -90 and -72 dBFS; maximum lift is available through -36 dBFS and
tapers to unity by -18 dBFS. Thus digital silence stays zero, and very low noise
does not receive an automatic boost. These thresholds apply after threshold drive.

## Bounds, ownership and callback behavior

| Input | Accepted range | Nonfinite fallback |
| --- | --- | --- |
| Processing rate | 8,000–3,072,000 Hz | 48,000 Hz; also used for nonpositive rates |
| Channels | 1–2 | Integer values clamp |
| Threshold | -72–0 dB | 0 dB |
| Ceiling | -36–0 dB | -1 dB |
| Release | 0.1–10 | 1 |
| Clip / Stereo Link | 0–1 | 0 / 1 |
| Upward | 0–12 dB | 0 dB |
| Mode | Listed enum values | Modern |

NaN and infinite audio samples become zero. Extreme finite samples are bounded to
`+/-1e12` before drive. A missing right buffer is treated as silent input in a
stereo instance. A null left buffer or nonpositive sample count is a no-op. Before
prepare, processing sanitizes and guards samples without delay. Reset clears the
audio/gain history while keeping settings.

`prepare` allocates all rings. `setSettings`, `reset`, and `process` allocate no
memory and acquire no locks. The monotonic minimum queue has **amortized O(1)**
cost per sample: each entry is inserted and removed once. A single sample can
remove multiple entries, so this is not a worst-case constant-time queue. Average
gain updates are O(1), with compensated summation to limit accumulated error.
Storage is O(sampleRate * lookahead) and independent of callback block size.

One audio thread must own each instance, including settings changes. The class is
not internally synchronized. Controls take effect at the next call, with drive
smoothed as above; abrupt ceiling, upward, clip or mode changes can engage the
final guard or change the envelope. Those controls do not have a separate
parameter dezipper. `gainReductionDb()` returns a finite, nonnegative
maximum attenuation across channels and samples in the last processed block,
excluding threshold drive, upward gain and clipping. Its display hold and decay
belong to the plugin.

## Verification

`Tests/LimiterTests.cpp` runs without JUCE. It checks impulse alignment, ceiling
protection through 3.072 MHz, low ceilings with reconstruction margin, deterministic
block partitioning, reset, independent/linked stereo, silence and upward gating,
malformed inputs/settings, distinct modes, release scaling, smooth anticipation,
threshold automation smoothing across sample rates, and 2,000 automation blocks
with allocation tracking. A distortion check limits a steady 50 Hz tone by 6 dB
and requires a residual below -80 dB in every mode except Aggressive.
Mode-transition regressions also cover returning to Smooth after recovery in
every other mode, and changing modes during an active release. Run it with the CMake test
target or compile directly against `Source/Core/Limiter.cpp` using C++20.

This core does not provide resampling, true peak detection, dither, loudness
measurement, bypass compensation, delta routing, or plugin state serialization.
Listening comparisons and a broader independent mastering evaluation remain useful
before using a new limiter on release masters.

## Plugin integration

The wrapper adds the openMaster high/low-pass stage **before** oversampling and detection. JUCE FIR half-band stages provide 1/2/4/8/16x processing; true-peak mode enforces at least 4x. Each quality path is padded to one fixed delay. A cold quality path warms before a 20 ms crossfade.

After downsampling, `TruePeakGuard` estimates eight fractional phases with a normalized 96-tap windowed-sinc detector. It holds required linked gain over 192 samples, adds 50 ms recovery, smooths gain over 64 samples, and applies it to audio delayed by 128 samples. A 0.3 dB reserve covers small detector/reconstruction differences. With True Peak on, the main limiter also limits to the reserve, so the guard only removes reconstruction overshoot and does not add gain modulation of its own. The independent host test uses a 16-phase, 64-tap reconstruction. The finite detector is an estimate; no universal reconstruction-filter guarantee is claimed.

The reused K-weighted meter follows BS.1770-style absolute/relative gates with exact energy sums and a 0.001 LU histogram for the integrated gate. See the [ITU BS.1770 recommendation](https://www.itu.int/rec/R-REC-BS.1770/en). Short-term averaging uses 3 seconds. LRA is not implemented. TPDF dither is unshaped, independently generated per sample/channel, and quantizes to 16 or 24 bits.

## Implementation-to-source map

The mode names describe this project's tuning. They are not names of copied published algorithms. Each mode uses the same finite lookahead construction described above.

| Implementation | Public basis and what is original here |
| --- | --- |
| Attack, hold and smoothing | [Luff's limiter article](https://signalsmith-audio.co.uk/writing/2022/limiter/) describes the moving minimum, the hold extension and cascaded box filters. The per-mode attack alignment through a gain ring and the attack/hold tuning are original. |
| Modern / Smooth / Aggressive / Safe | [Luff's release-curve discussion](https://signalsmith-audio.co.uk/writing/2022/limiter/#release-curve) supplies the exponential-recovery building block and the series cascade used for rounding. The sustain detector that splits transient and sustained attenuation, and all times, are original. The [L4 guide](https://assets.wavescdn.com/pdf/plugins/l4-ultramaximizer.pdf) describes program-adaptive release and clipping as a goal, not a method. |
| Classic | [Luff's alternative-release discussion](https://signalsmith-audio.co.uk/writing/2022/limiter/#alternative-release-curves) motivates other recovery laws. This implementation recovers at a constant 40 dB per second. It is not a published L2 emulation. |
| Upward and clipping | Original gated amplitude-envelope lift, sustain-scaled clip allowance and quadratic knee, with the equations/ranges documented above. The L4 guide is a workflow reference, not the source of these transfer functions. |
| True-peak reconstruction | Windowed-sinc FIR construction is explained by Steven W. Smith in [The Scientist and Engineer's Guide to DSP, Chapter 16](https://www.dspguide.com/CH16.PDF). The eight-phase/96-tap detector, gain scheduling and reserve are this project's design and are tested independently. |
| Oversampling | [JUCE 8.0.9 Oversampling implementation](https://github.com/juce-framework/JUCE/blob/8.0.9/modules/juce_dsp/processors/juce_Oversampling.cpp), using its FIR half-band stages. |
| High/low-pass stage | Reused MIT-licensed [openMaster Butterworth design](https://github.com/alanmarcero/openMaster/blob/8e0c6f00b17ceb37446da5c942ba1af9e22f36fc/Source/Core/ButterworthCoeffs.cpp) and multipole cascade. [Robert Bristow-Johnson's Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/) documents the underlying digital low/high-pass biquad forms. |
| Loudness | [ITU-R BS.1770](https://www.itu.int/rec/R-REC-BS.1770/en), implemented by the reused openMaster meter. Its K-weighting coefficient fit follows the [libebur128 implementation](https://github.com/jiixyj/libebur128/blob/master/ebur128/ebur128.c); the integrated histogram and precision limits are described above. |

The source files identify the implementations; numerical constants in the policies table are part of openWallLimiter's own tuning. Citing these references does not imply their authors endorse this plugin or that its behavior equals another product.
