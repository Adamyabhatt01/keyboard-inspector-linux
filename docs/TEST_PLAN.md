# Test Plan — Linux Port (C++)

How the port is verified, what each suite covers, and the evidence expected after every
significant change. Tests are Catch2; both binaries are built by the normal CMake build.

## Suites

| Binary | Scope | Notes |
|---|---|---|
| `build/bin/keyboard-inspector-tests` | `core/` + `capture/` (no widgets) | 49 cases / 488 assertions |
| `build/bin/keyboard-inspector-ui-tests` | offscreen `ui/` smoke | 3 cases |

```bash
./build/bin/keyboard-inspector-tests
./build/bin/keyboard-inspector-ui-tests
```

`ui_freeze_test.cpp` runs with `QT_QPA_PLATFORM=offscreen`; it constructs the real
`MainWindow`, feeds a recording, and asserts the freeze/visibility pipeline end to end.

## Case inventory (test_core.cpp)

- **Recording/ingestion** — dedupe of consecutive same-key/same-state events, auto-repeat
  drop, source naming, 80% visibility rule, freeze merge, release synthesis, duration
  handling.
- **Analysis** —
  - frequency recovery: 1 ms → 1000 Hz, 2 ms → 500 Hz, 0.25 ms → 4000 Hz (bin rate 16000,
    kissfft when linked),
  - identical timestamps, zero-ms chords, long gaps, sparse input → insufficient data,
  - circular wraparound, reverse HPS behavior, low-cut on/off, empty input, Nyquist edge,
  - tentative snapshots during recording.
- **`.kbi` formats** —
  - v3 round trip (title, duration, events, settings),
  - v2/v1/v0 legacy blobs built byte-by-byte in the tests (2-byte UTF-16 char
    discriminants, typed ids, InputInfo block gating, creator string),
  - damaged/newer-version error messages,
  - field order vs upstream `Result.cs`/`Event.cs`/`Input.cs`/`InputInfo.cs`/`Source.cs`.
- **Key names** — `.NET Keys` alias resolution (last-declared wins), legacy gamepad
  (Button1–32, HatN–W), mouse, TETR.IO names, undefined VK fallback to decimal.
- **Capture** — backend contract, event filtering (`EV_SYN`/`EV_MSC`/`EV_LED`/repeat),
  timestamp handling, error/disconnect records.
- **EvdevReportTracker** — one `Movement` input per SYN report with axis activity
  (alternating pressed state, empty reports yield nothing, `EV_KEY` is not movement);
  hat switch → `HatN/E/S/W` with upstream bit order and initial-adopt semantics
  (`SYN_DROPPED` resets); hats tracked on gamepad nodes only but hat reports still
  count as movement.
- **Rate estimate — fundamental selection** — regression for a real 2.4G Mouse
  recording: 8 ms report cadence SOF-locked to the 2 ms USB frame grid grows a taller
  500 Hz harmonic; the estimate must report the 125 Hz cadence. Guard case: a true
  500 Hz cadence on the same frame grid stays 500 Hz (no over-downgrade).
- **USB descriptors (`capture/usb_info.cpp`)** —
  - `parse_interval_ms`: `1`, `1ms`, `2 ms`, `1000us`, `0.5`, junk → nullopt,
  - `configured_max_hz`: full-speed `1000/interval`, high-speed `8000/2^(bInterval-1)`,
    missing data stays nullopt,
  - `find_usb_info` against a **synthetic sysfs tree** (temp dir with idVendor/bInterface/
    ep_81 files and an `event7` symlink): vendor/product/speed/interface/endpoint parse,
    `configured_max_hz == 1000`, `interval_text()` says "configured … not measured",
    non-USB device yields nullopt.
- **TETR.IO parser (`core/tetrio.cpp`)** —
  - solo `.ttr`: 600 Hz subframe grid (`round(frame+subframe)/60`), non-key and hoisted
    event drops, 40 LINES title prefix, forced bin rate 600, source naming, grouping,
  - multiplayer `.ttrm`: id-matched round sides, matchup string, per-round labels from
    round stats, min-frames duration, degenerate rounds skipped,
  - selected-player recording build for either side,
  - error paths: old/missing version, missing/invalid round selection, unknown player id,
    no key events, thin leaderboard.
- **Built-in FFT (`core/analysis.cpp`)** — radix-2 vs naive DFT cross-check plus inverse
  round trip; Bluestein for arbitrary sizes (100/127/600) vs naive DFT; magnitude
  spectrum recovers a spectral line at power-of-two (4096) and non-power-of-two (4000)
  lengths.
- **Logger** — level formatting, file output, module tags.

## UI verification (beyond unit tests)

1. **Offscreen render**: `QT_QPA_PLATFORM=offscreen ./build/bin/keyboard-inspector
   --screenshot /tmp/ki.png <sample>.kbi` — inspect the PNG (layout, axis labels,
   shadows, scrollbar, timeline rows, rate readout state). The screenshot timer waits for
   the initial async analysis to land.
2. **Wayland smoke**: same command under `QT_QPA_PLATFORM=wayland` on Hyprland.
3. **Interaction matrix** (manual, on the real desktop):
   wheel zoom around cursor; drag pan (horizontal lock); vertical drag-zoom; scrollbar
   thumb drag; notch drags (left/right) with damped Cramp behavior near minimum width;
   track click page jump; double-click spotlight in and out; hover bubble micro-decimal
   splitting at high zoom; timeline row drag-reorder; row context menu; drag-and-drop
   file open; Import-from-URL clipboard auto-fill; recording start/stop with kind toggles;
   permission banner with no accessible devices.

## Generating test recordings

A sample `.kbi` can be produced with a throwaway generator linked against `core/`
(see the session log in `IMPLEMENTATION_PLAN.md`); 1 ms-period press/release pairs with
±0.4 ms jitter recover ~1000 Hz and populate all charts.

## Invariants every test change must preserve

- Never assert a *measured* USB rate; descriptor values are configured capabilities.
- Frequency-recovery tests state their required bin rate; HPS/low-cut tests state the
  setting they exercise.
- Legacy `.kbi` builders stay byte-exact — they are the executable form of
  `COMPATIBILITY.md`.
- A math change to `core/analysis.cpp` requires both a new test here and an
  `ALGORITHM.md` update in the same change.

## Current status

- 49 core cases / 488 assertions: **pass** (2026-10-26).
- 3 UI smoke cases: **pass** (2026-10-26).
- No-kissfft build (`-DVENDOR_KISSFFT=OFF`): 44 cases **pass** in <1 s — built-in
  radix-2/Bluestein FFT verified against a naive DFT and by frequency recovery.
- Offscreen + Wayland screenshots verified with a 200-event 1000 Hz sample recording.
- Real-hardware evdev timing validation: **complete for the URX75** — 2026-10-25 first
  run (≈1000 Hz strong) and 2026-10-26 long session (890 events / 19.85 s → 999.985 Hz,
  strong, phase 0.963, saved `.kbi` re-analyzed offline). Never claim a measured rate
  without it.
