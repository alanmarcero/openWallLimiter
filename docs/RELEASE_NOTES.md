# openWallLimiter 1.1.2

A fix for lost treble at 44.1 kHz.

- With oversampling on at 44.1 kHz, the output lost treble even with no limiting: -2.4 dB at 10 kHz, -6.3 dB at 15 kHz and -10.9 dB at 18 kHz. The lookahead came to a half sample at the base rate, and the delay that aligns it averaged two samples. The lookahead now rounds up to a whole number of samples at every oversampling factor, and the response is flat. 48 kHz sessions were not affected.

Packages include macOS universal VST3/AU and Windows x64 VST3. macOS bundles are ad-hoc signed, not notarized.
