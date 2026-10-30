# openWallLimiter 1.1.0

A rebuilt limiter core. The five modes now sound different from each other, and the default sound is much cleaner under heavy limiting.

- Each mode has its own attack, hold, release and clip tuning, following the intent of the L4 modes: Modern balanced and punchy, Smooth transparent, Aggressive loud and forward, Safe clean, Classic steady.
- A program-dependent release recovers fast after short peaks and slowly from sustained limiting. Dense material no longer modulates its own waveform.
- A per-mode hold keeps the gain flat between bass peaks. Two cascaded averages give the gain an S-shaped attack ramp.
- Clip now uses a smooth quadratic knee, and sustained limiting shrinks the clip allowance. Clipping applies mainly to transients.
- With True Peak on, the main limiter now limits to the 0.3 dB reserve, so the final guard only removes reconstruction overshoot.

Measured at default settings with 8 dB of input drive, intermodulation on a steady chord drops from -38.5 dB to -45.5 dB in Modern and from -44 dB to -52 dB in Safe. A 50 Hz tone limited by 6 dB measures 0.000% THD in every mode except Aggressive.

Packages include macOS universal VST3/AU and Windows x64 VST3. macOS bundles are ad-hoc signed, not notarized.
