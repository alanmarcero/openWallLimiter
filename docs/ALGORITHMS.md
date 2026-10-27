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
  and delayed audio. The ring queues and processing code here were written for
  this project; no Signalsmith implementation is included.

## Signal path and latency

`sanitized input -> threshold drive -> gated upward lift -> audio delay -> gain -> soft clip -> ceiling guard`

The parallel gain path computes a safe gain per channel, links towards the smaller
gain, holds the moving minimum, applies the selected release, and smooths the gain
with a rectangular moving average. Input and internal gain state use double
precision; the public buffers contain floats.

Let `N = ceil(processing sample rate * 0.005)`. The audio delay is exactly `N`
samples in every mode. Both the moving minimum and gain average contain `N+1`
samples. An input target at time `t` constrains every held target from `t` through
`t+N`. Release never exceeds the held target. Consequently all terms contributing
to the smoothed gain at `t+N` obey that input's limit. This gives an anticipatory
linear gain transition without an instantaneous attack on the audible transient.

When intentional clipping is disabled, the target is
`min(1, ceilingAmplitude / abs(drivenInput))`. With clipping enabled, the numerator
also contains the mode's clipping allowance. The final symmetric guard always
limits sample magnitude to the current ceiling, including after automation. Float
rounding can introduce approximately one float ULP at the boundary.

The caller must add any resampling filter delay to the reported delay. At
oversampling factor `F`, the core contributes `ceil(hostRate*F*.005)/F` host
samples; rounding this value early can produce an alignment error. This core does
not detect reconstructed intersample peaks. Oversampling and any true peak margin
belong to the plugin layer.

## Policies

All times in this table are base release constants, multiplied by the release
control. The exponential coefficient is `1 - exp(-1/(sampleRate*time))`.

| Mode | Recovery policy | Maximum clip allowance at Clip = 1 |
| --- | --- | --- |
| Modern | Single release whose coefficient interpolates between 45 ms at unity gain and 180 ms at deep attenuation | 1.5 dB |
| Smooth | Weighted sum of independent 120 ms (35%) and 280 ms (65%) releases | 0.75 dB |
| Aggressive | Fast 25 ms exponential recovery | 6 dB |
| Safe | Conservative 250 ms exponential recovery | 0 dB; Clip has no effect |
| Classic | Gain returns at 60 dB/second, scaled inversely by Release | 3 dB |

Clipping allowance increases continuously with Clip. A symmetric tanh shoulder
starts at `ceiling * (1 - .12*Clip)` and asymptotically approaches the ceiling.
The shoulder runs only when Clip is positive, and never runs in Safe mode. Clip
therefore trades attenuation for intentional saturation; Safe still produces the
normal modulation distortion inherent in gain limiting under difficult material.

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
with allocation tracking. Run it with the CMake test
target or compile directly against `Source/Core/Limiter.cpp` using C++20.

This core does not provide resampling, true peak detection, dither, loudness
measurement, bypass compensation, delta routing, or plugin state serialization.
Listening comparisons and a broader independent mastering evaluation remain useful
before using a new limiter on release masters.
