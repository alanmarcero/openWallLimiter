# Commercial feature research

## FabFilter Pro-L 2

Official references:

- [Pro-L 2 overview](https://www.fabfilter.com/help/pro-l): lists adjustable lookahead and separate channel-link settings for transients and release.
- [Advanced settings](https://www.fabfilter.com/help/pro-l/using/advancedsettings): describes lookahead as control of the initial transient stage and a separate Release channel-link control.

The comparison identified three controls that openWallLimiter did not expose independently:

1. **Adjustable lookahead** — Pro-L 2 exposes lookahead for its initial transient stage. openWallLimiter offers 0–5 ms. Moving the knob does not change the latency reported to the host. That figure also includes the longest oversampling filter delay and a 128-sample true-peak guard.
2. **Transient channel link** — Pro-L 2 separates transient linking from release linking. openWallLimiter's existing `stereo_link` parameter now has the clearer display name **Transient Link** while retaining the same stable parameter ID and behavior.
3. **Release channel link** — a new independent **Release Link** control blends each channel's recovery toward shared release behavior. Its default is 100%, matching the prior linked envelope feel.

These controls are inspired by the documented workflow and control separation. The DSP and interface are original and are not intended to emulate FabFilter's limiter algorithms.

## State and automation compatibility

The existing `stereo_link` parameter ID is unchanged, preserving host automation. When an older session has no `lookahead_ms`, it loads at 5 ms. When it has no `release_link`, that control loads at 0% so old sessions retain their former independent release envelopes; new sessions default both link controls to 100%.
