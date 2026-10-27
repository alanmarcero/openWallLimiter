# openWallLimiter design

Build a mastering limiter in Alan Marcero's open plugin collection, inspired by the public Waves L4 workflow. The user requested a new public GitHub repository, generated graphics, multiple researched limiter algorithms, and installation through alan-m-OpenVSTManager. The corrected product/repository name is **openWallLimiter**; castle imagery remains the visual concept.

## Scope and architecture

JUCE 8.0.9, C++20, CMake; VST3 for macOS universal and Windows x64, AU for macOS. An independently testable C++ core supplies stereo/mono lookahead limiting. The JUCE wrapper supplies oversampling, host parameters/state, latency compensation, metering and original native graphics. Use the collection's AGPL-3.0 licensing and package JUCE notices.

Five modes: Modern (crest-dependent dual release), Smooth (longer release and smooth anticipation), Aggressive (short release with controllable clipping), Safe (long release without intentional clipping), Classic (single-envelope exponential recovery). These are original implementations, not bit-identical emulations of Waves code. Shared controls: threshold -30..0 dB, ceiling -30..0 dB, release 0.1..10 multiplier, clip 0..100%, upward 0..10 dB, stereo link 0..100%, oversampling off/2/4/8/16, true peak, 16/24-bit TPDF dither or off, gain match, delta, bypass. Factory presets and resettable short-term/integrated LUFS and peak/GR meters.

Signal flow: smoothed input drive derived from threshold; gated upward gain; optional clip allocation; lookahead limiter; oversampling reconstruction; final output protection; dither; gain-match/delta/bypass monitoring. The dry path must match reported latency. Allocate buffers in prepare, never callback. Sanitize invalid audio. Stereo linking must preserve the louder channel's ceiling at any link value. True-peak tests must measure independently reconstructed output, not only sample maxima. If the verified guard uses additional margin, document it honestly.

Native UI: 1080x700 dark graphite panel, warm brass accents, generated stone fortress artwork, readable vector labels, threshold and ceiling faders, prominent five-mode selection, central scrolling signal/GR graph, lower parameter knobs and quality/monitor controls. All controls connected to host automation and saved state. Generated image source and prompt recorded under assets.

## Validation and distribution

Tests: ceiling at sample rates 44.1/48/96/192 kHz and all modes, impulse latency, block partition invariance, stereo link, quiet/silent input, invalid input, extreme parameters, distinct mode output, upward gating, parameter/state roundtrip, mono/stereo, bypass/delta alignment, oversampling changes, reconstructed peaks, native UI controls/screenshots. Run pluginval level 5 and AU validation on macOS. CI runs sanitizer core tests plus native builds/tests/validation, then publishes paired ZIPs and SHA256 checksums. Public catalog entry uses exact bundle names openWallLimiter.vst3 and openWallLimiter.component. Verify manager discovers and installs release in isolated directories.
