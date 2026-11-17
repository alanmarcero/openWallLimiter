# openWallLimiter

A mastering limiter for controlling peaks and loudness at the end of a mix.

![openWallLimiter limiter editor](docs/screenshots/openWallLimiter.png)

[Download the latest release](https://github.com/alanmarcero/openWallLimiter/releases/latest) · [Install with OpenVST Manager](https://github.com/alanmarcero/alan-m-OpenVSTManager/releases/latest)

The fortress mark pulses on quarter notes from the DAW transport, including after tempo changes or seeks. It holds when playback stops. Hosts without transport use an audio-sample clock.

## Controls

- **High pass and Low pass:** Filter audio before limiting. High pass covers 10–200 Hz; Low pass covers 8–24 kHz. Choose 12, 24, 48, 72, or 96 dB-per-octave slopes, or bypass either filter.
- **Threshold and Ceiling:** Set limiting and maximum output level. Link the controls to move them together.
- **Styles:** Choose Modern, Smooth, Aggressive, Safe, or Classic. Factory presets provide starting points.
- **Release, Clip, and Upward:** Set recovery time, allow transient clipping, and lift quiet audio.
- **Transient Link and Release Link:** Set how much the channels share peak detection and limiter recovery.
- **Lookahead:** Set transient anticipation from 0 to 5 ms. Reported latency stays fixed at 5 ms.
- **True peak and Oversampling:** Enable True peak for protection against peaks between samples. Choose Off, 2x, 4x, 8x, or 16x oversampling. True peak uses at least 4x.
- **Meters:** View gain reduction, short-term and integrated loudness, sample peaks, estimated true peak, and an eight-second history. Reset measurements for a new pass.
- **Gain Match and Delta:** Match levels for comparison or listen to the difference between processed and original audio.
- **Dither:** Choose Off, 24-bit, or 16-bit TPDF dither.
- **Bypass:** Bypass the complete limiter.

Choose a style to set the limiter's response:

| Style | Character |
| --- | --- |
| Modern | Balanced, with fast recovery for transients. |
| Smooth | Transparent, with a slower attack and rounded recovery. |
| Aggressive | Fast recovery with more transient clipping. |
| Safe | Slow recovery without intentional clipping. The Clip control has no effect. |
| Classic | Steady recovery. |

Parameters support DAW automation and session recall. The limiter supports mono and stereo audio.

## Formats

- **macOS 11 or newer:** Universal VST3 and Audio Unit (AU).
- **Windows 10 or newer, x64:** VST3.

Use [OpenVST Manager](https://github.com/alanmarcero/alan-m-OpenVSTManager/releases/latest) or download a package from the [latest release](https://github.com/alanmarcero/openWallLimiter/releases/latest).

## License

AGPL-3.0. See [LICENSE](LICENSE) and [third-party credits](THIRD_PARTY.md).
