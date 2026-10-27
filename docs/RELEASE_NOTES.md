# openWallLimiter 1.0.0

First release of openWallLimiter, a mastering limiter with a castle-themed native interface.

- Five original modes: Modern, Smooth, Aggressive, Safe and Classic.
- Pre-limiter high/low-pass filters with 12, 24, 48, 72 and 96 dB/oct slopes, matching openMaster.
- Threshold, ceiling, release, variable clipping, gated upward compression and stereo linking.
- Off/2x/4x/8x/16x oversampling, fixed host latency and crossfaded quality changes.
- True-peak mode uses at least 4x processing with a final reconstructed-peak lookahead guard and 0.3 dB headroom.
- Short-term and gated integrated LUFS, peak, gain reduction and scrolling level history.
- Gain-match, delta audition, latency-aligned bypass, factory starting points and 16/24-bit TPDF dither.
- macOS universal VST3/AU and Windows x64 VST3.

True-peak readout is a 4x estimate. This release does not include an LRA readout or noise shaping. macOS bundles are ad-hoc signed, not notarized.
