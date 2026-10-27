# openWallLimiter Implementation Plan

> **For agentic workers:** Use $subagent-driven-development or $executing-plans to implement this plan task-by-task.

**Goal:** Ship an original multi-algorithm mastering limiter and register its installable release in OpenVST Manager.
**Architecture:** A C++ DSP library is wrapped in JUCE for host integration and original native UI. Existing collection release and catalog formats are reused.
**Tech Stack:** C++20, JUCE 8.0.9, CMake, GitHub Actions.
**Spec:** docs/superpowers/specs/2026-10-27-open-wall-limiter.md

## Global Constraints
- Product and repository: openWallLimiter; company Alan Marcero; manufacturer Amrc; code Owli.
- Float, mono/stereo, fixed reported delay, callback free of locks and allocation.
- Public-source-derived original algorithms; no claim of exact Waves equivalence.
- macOS universal VST3/AU and Windows x64 VST3; AGPL-3.0.

## Review Focus
- Output ceiling under extreme drive and automation: core and wrapper tests.
- Quality changes must preserve host latency and finite output: wrapper tests.
- Bypass/delta must use delayed original signal: impulse and null tests.
- Silent input and nonfinite samples must not poison envelopes: core tests.
- Installer exact bundle names and valid release archives: isolated manager install.

### Task 1: Limiter DSP and behavior tests
**Files:** Source/Core/Limiter.h, Source/Core/Limiter.cpp, Tests/LimiterTests.cpp, docs/ALGORITHMS.md
**Interfaces:** namespace wall; enum class Mode { Modern, Smooth, Aggressive, Safe, Classic }; struct Settings { float thresholdDb=0, ceilingDb=-1, release=1, clip=0, upwardDb=0, stereoLink=1; Mode mode=Mode::Modern; }; class Limiter { void prepare(double sampleRate,int channels); void reset() noexcept; void setSettings(const Settings&) noexcept; void process(float* left,float* right,int samples) noexcept; int latencySamples() const noexcept; float gainReductionDb() const noexcept; };
- [ ] Write ceiling, delay, partition, stereo, silence, invalid, mode distinction and upward tests first; compile with C++20 and verify missing implementation failure.
- [ ] Implement preallocated constant-time moving-window gain control with sample-rate-correct fixed lookahead, smoothing and distinct release/clip policies. Support processing rates up to 3.072 MHz (192 kHz x16); zero allocations in setSettings/process; exact known latency. Threshold drives level upward independently of ceiling.
- [ ] Reject NaN/inf, clamp user settings; final ceiling protection; linked gain never increases either channel above its independent safe gain.
- [ ] Add public research links and exact implementation/tuning details, limitations and original authorship. Core need not implement oversampling, true peak, dither, metering, bypass or delta; those belong to task 2.
- [ ] Compile/run standalone Tests/LimiterTests.cpp against Limiter.cpp with clang++ -std=c++20; run ASan/UBSan. Commit only task-owned files.

### Task 2: JUCE integration, native UI and artwork
**Files:** CMakeLists.txt, Source/Plugin/*, Source/Core metering helpers, Tests/PluginTests.cpp, assets/*.
**Interfaces:** Uses wall::Limiter and Settings exactly as task 1. Public processor class WallProcessor; juce::AudioProcessorValueTreeState parameters; atomic float meter fields for UI; createEditor returns native UI.
- [ ] Write host control/state/layout/latency tests first.
- [ ] Preallocate oversampling paths, delay-compensated dry path; smooth controls; add protected output and independent true-peak tests.
- [ ] Reuse collection's tested loudness meter with attribution; add gain-match, delta, bypass and TPDF quantization.
- [ ] Generate fortress panorama with built-in image tool and place in assets; render live vector controls and meters over artwork.
- [ ] Build, run host tests and screenshot editor; inspect actual rendering; run pluginval and auval.

### Task 3: Publish and manager integration
**Files:** .github/workflows/*, scripts/*, README.md, LICENSE, THIRD_PARTY.md, manager catalog.json.
- [ ] Add CI sanitizer and macOS/Windows native builds and validation, ZIP packaging plus checksums.
- [ ] Create public alanmarcero/openWallLimiter repository, push tested sources and release tag; monitor builds and repair failures.
- [ ] Add catalog entry matching exact bundle names, run manager checks and isolated discovery/install/removal, push catalog update.
- [ ] Verify public release assets and report links, validation and material limits.
