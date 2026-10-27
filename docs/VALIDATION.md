# Validation record

The core suite checks ceiling safety, delay, block partitioning, stereo behavior, upward gating, invalid inputs, allocation behavior, drive automation and every mode-transition pair. The standalone and AddressSanitizer/UndefinedBehaviorSanitizer runs pass 6,135 assertions.

Native host checks cover parameter/state roundtrip, every visible parameter control, exact unity/delta/host-bypass alignment, all five high/low-pass slopes, filtering before detection, mono processing, oversized buffers and unchanged latency during quality changes at 44.1/48/96/192 kHz. True-peak stress covers all modes with maximum clipping and heavy drive against a separate 16-phase/64-tap reconstruction.

Initial reconstructed-peak tests exposed downsampling overshoot; the post-downsampling lookahead guard was added and the same tests now pass. Review found stale Smooth release state after mode changes; transitions now preserve outgoing gain and the regression passes.

Local macOS VST3 passed pluginval strictness 5. Published release workflows separately build and test macOS universal and Windows x64; their run records are the evidence for the downloadable artifacts. This record documents automated behavior, not a listening equivalence claim against another commercial plugin.
