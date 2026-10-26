# Decisions Log — Linux Port

Why each significant choice was made, with the trade-off accepted. Newest last. If a
decision is invalidated, supersede it here rather than silently changing behavior.

## Language and stack

1. **C++20 + Qt 6 (CMake)** replaces the earlier Python/PyQt6 scaffold.
   The analysis and UI layers are heavy, latency-sensitive, and geometry-exact; a native
   Qt build gives predictable allocation behavior, native Wayland support, and one
   deployable binary. The Python implementation was deleted; the C++ port is the only
   supported implementation.
2. **Qt 6 Widgets, not QML/Quick** — the upstream UI is a fixed, pixel-managed dark
   utility; custom `QWidget` painting reproduces it exactly, and upstream interaction
   quirks (pinned-cursor drags, notch captures) map naturally onto widget mouse events.
3. **Qt6 licensing consequence**: Qt Widgets under GPL-3.0 or commercial; this port is
   therefore **GPL-3.0-or-later**, preserving upstream's BSD-3-Clause notice. The Windows
   original stays untouched (BSD-3-Clause) and remains the reference.
4. **kissfft optional**. When found by CMake the FFT uses `kiss_fftr`; otherwise a built-in
   radix-2 / Bluestein FFT keeps the build dependency-free with O(n log n) performance and
   a startup warning. Correctness is identical; only constant factors differ (tests pass
   either way, and the no-kissfft build runs the full suite in well under a second).
5. **Catch2 for tests** — one dependency, `SECTION`-free case style, integrates with
   CTest. The old stdlib-unittest layout of the Python port is gone.

## Capture

6. **evdev via libevdev, no `EVIOCGRAB`** — mirroring upstream Raw Input semantics:
   passive observation, other applications keep receiving input. libevdev (C) instead of
   python-evdev: one less interpreter layer, stable ABI, no GIL.
7. **One reader thread per device with `select()` timeout ~100 ms** — stop requests are
   honored without blocking; per-device errors localize to that device.
8. **Kernel evdev timestamps, with a domain-locked fallback** — event time is what the
   kernel stamped (`input_event.time`), matching the "real timestamp" spirit of upstream
   `Recorder.ElapsedPrecise`. The synthetic fallback (only for invalid/zero timestamps)
   locks its clock to the domain the first valid event matches
   (`CLOCK_MONOTONIC` vs `CLOCK_REALTIME`), so mixed-domain deltas can never occur.
   Documented divergence: upstream timestamps at `WM_INPUT` with a `Stopwatch`; ours are
   kernel-stamped earlier in the pipeline (see `COMPATIBILITY.md`).
9. **Auto-repeat (`EV_KEY` value 2) is dropped and counted** — repeats are an OS feature,
   not hardware input; the count surfaces as Capture → "Dropped auto-repeats".
10. **No usbmon / USB-report backend** — deliberately not implemented. The usbmon debugfs
    interface needs privileged access and a separate measurement-layer label; until (and
    unless) it is built, the app stays honest about measuring evdev timing only.
11. **No demo/synthetic backend** — deliberately not implemented; screenshots for
    verification use generated `.kbi` files instead, which also exercises the file format.

## Permissions

12. **Never root; never `grab()`** — the GUI runs as the logged-in user. Access comes
    from a targeted `uaccess` udev rule (installed by `packaging/install-udev-rule.sh`)
    or the broad `input` group. The `71-` rule prefix is required so it sorts before
    `73-seat-late.rules`; a `99-` prefix applies the tag too late for the ACL step.
13. **Locked devices stay listed** — detection ≠ permission; the device list shows both
    states and the banner explains the fix. Enumerating and *showing* a device the user
    cannot open is fine; silently skipping it is not.

## Analysis

14. **Upstream math verbatim; documented divergences only where noted** — see
    `ALGORITHM.md` for the two sanctioned deviations (intended circular rotation instead
    of the upstream off-by-one stride bug; peak readout as a spec addition).
15. **Reverse HPS preserved** — upstream's harmonic product spectrum multiplies with
    `copy[int(i*(j+1)/(HPS+1))]` (a *reverse* harmonic selection). Textbook forward HPS
    gives different peaks; do not "fix" it without re-validating against upstream output.
16. **FFT fallback never used silently** — the startup warning plus the `ALGORITHM.md` §3
    note keep the dependency status visible; the fallback matches kissfft's output
    (naive-DFT cross-check and frequency recovery tests).

## File formats

17. **`.kbi` v3 for writing; v0–v3 for reading** — matching upstream's current format and
    its history. Legacy v0 files use 2-byte UTF-16 discriminants and typed ids
    (0=Keyboard/1=Gamepad/2=Mouse/3="TETR.IO Replay"); names are reconstructed from the
    upstream key-name tables. Saving always upgrades to v3.
18. **.NET `Keys` alias resolution = last declared** — empirically matches the upstream
    alias chain (`Return`, `Capital`, `PageUp`, …); pinned by keynames tests.
19. **TETR.IO import with picker + disclaimer** — multiplayer replays carry both players'
    inputs per round; the picker lists one entry per player (side matched by id equality,
    winner by alive flag). The disclaimer (verbatim from `FileSystem.cs:45-51`) shows
    once per session. Replay times land on the 600 Hz subframe grid
    (`round(frame+subframe)/60`), which is a property of the source, not a measurement.

## UI

20. **Upstream visual identity is fixed** — dark studio utility, exact palette from the
    audit, 3 px chart gaps, 25 px analysis strip. No light theme, cards, or added motion.
21. **Chart interactions ported exactly** — wheel `ZoomChange`, pinned-cursor pan with
    direction lock (cursor re-pinned at the capture point, offsets accumulated — the
    upstream `CapturedCursorTick` model), scrollbar end-notches with the `Cramp`
    desmos easing (s=0.0004), `DrawShadowString` stamping, `XAxisMicroDecimals` label
    splitting, hover bubbles with micro segments, double-click spotlight by reparenting.
    These reproduce upstream feel; approximating them would be worse than omitting them.
22. **Qt6 has no `NoRedirectPolicy`** — `QNetworkRequest::ManualRedirectPolicy` +
    `RedirectionTargetAttribute` is the equivalent (used by the URL importer and the
    update check).
23. **Additions over upstream stay minimal and labeled** — rate readout states, device
    selector with lock states, permission banner, diagnostics action, download progress
    state. None restyle or crowd the original controls.
