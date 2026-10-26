# Implementation Plan and Status

Single source of truth for what exists, what is verified, and what remains. Update this
file with every significant change. Never mark something verified without evidence.

Legend: **DONE** (implemented and verified by the listed evidence), **PARTIAL**
(implemented, not fully verified), **PENDING** (not implemented).

The original Python/PyQt6 port was replaced by the current C++/Qt6 implementation; the
milestones below describe the C++ port only.

## Milestones

| # | Milestone | Status | Evidence |
|---|---|---|---|
| M0 | AI/maintainer context and upstream audit | DONE | `AGENTS.md`, `docs/UPSTREAM_AUDIT.md`, algorithm/UI/measurement/compatibility/decision docs |
| M1 | C++/Qt6 scaffold, data model, CLI | DONE | CMake build, `--list-devices`, `--screenshot`, `--debug`, model/session in `tests/test_core.cpp` |
| M2 | Analysis port and algorithm tests | DONE | `core/analysis.cpp`; `tests/test_core.cpp` (1000/500/4000 Hz recovery, dedupe, insufficient data, HPS, low cut) |
| M3 | evdev capture, permissions, disconnects | PARTIAL | Backend + sysfs enumeration + permission banner implemented; unit tests pass. Real-hardware timing validation blocked: the current user has no `/dev/input` read access (not in `input` group, no ACL) |
| M4 | Faithful UI (charts, timeline, analysis strip, rate readout) | DONE | Offscreen 1294×836 and Wayland screenshots inspected; chart/timeline interaction port (wheel, pinned pan, notches/Cramp, spotlight, drag-reorder) |
| M5 | `.kbi` v3 + legacy v0–v2 I/O, USB descriptor info, TETR.IO import | DONE | `tests/test_core.cpp`: v0–v3 round trips, legacy typed ids, key-name decoders, synthetic sysfs descriptor walk; TETR.IO parser compiles and drives the picker flow |
| M6 | Packaging, docs, end-to-end verification | DONE | Docs rewritten for C++; PKGBUILD built via `makepkg -f --nodeps` from an isolated copy; package contents inspected (bin, desktop, icon, doc examples, license); both test suites pass inside the package build |

## Upstream research (M0, DONE)

- Reverse-engineered upstream at commit `60901cb`:
  - capture path (`Recording/ListenerWindow.cs`, `Recorder.cs`, `UnprocessedInput.cs`),
  - event model and dedupe (`Result/Result.cs`, `Event.cs`, `Input.cs`, `InputInfo.cs`,
    `Source.cs`),
  - math (`Result/Analysis.cs`),
  - UI geometry/colors/interactions (`UI/MainForm.Designer.cs`, `UI/Chart.cs`,
    `UI/MainForm.cs`),
  - file formats and import (`Result/*.cs`, `FileSystem/*.cs`, `TETR.IO/TetrioReplay.cs`,
    `TETR.IO/TTRMPickerForm.cs`),
  - docs (`DOCUMENTATION.md`, `README.md`).
- Recorded all measurement-layer limits for the URX75: full-speed USB, HID boot keyboard,
  interrupt-IN `0x81`, `bInterval=1`, 8-byte reports, configured maximum 1000 Hz. This is
  descriptor data, not a measurement.
- Documented the two upstream quirks preserved or intentionally fixed (reverse HPS,
  circular rotation loop).

## Delivered components

### M1 — Scaffold and CLI

- CMake project (`CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`);
  `keyboard-inspector` binary from `src/keyboard_inspector/`.
- CLI: `--version`, `--help`, `--list-devices`, `--open/-o`, positional file (`.kbi`,
  `.ttr`, `.ttrm`), `--screenshot PNG`, `--window-size WxH`, `--debug/-d`, `--log-file`,
  `--quiet/-q`.
- Data model: `InputId`, `SourceInfo`, `InputInfo`, `Event`, `Recording`,
  `AnalysisSettings`, `MeasurementKind` (`EVDEV`, `USB_REPORT`, `TETRIO`, `FILE`).
- Leveled logger with file output, Qt message handler, crash handler.

### M2 — Analysis

- `core/analysis.cpp`: diffs/compound/circular distributions, binning, real FFT (kissfft
  or built-in radix-2/Bluestein fallback with a startup warning), logistic low cut,
  reverse HPS, normalization, rate estimate with sample/prominence/phase-coherence
  gating, diagnostics.
- Tests cover 1000/500/4000 Hz, zero-ms chords, circular wrap, dedupe, insufficient data,
  reverse HPS, low-cut on/off, long gaps, empty input, Nyquist, and tentative snapshots.

### M3 — Capture

- `capture/base.h/.cpp`: backend interface, `DeviceInfo`, `CaptureEvent`, error records.
- `capture/evdev_backend.h/.cpp`: sysfs enumeration without permission, reader threads
  with `select()`, kernel timestamps with domain-locked fallback (first valid event
  decides `CLOCK_MONOTONIC` vs `CLOCK_REALTIME`), auto-repeat drop counting, stop and
  disconnect handling, no `grab()`.
- `capture/usb_info.h/.cpp`: sysfs traversal from the evdev node to the USB interface and
  device, interrupt-IN endpoint discovery, descriptor interval parsing, configured
  maximum (`full-speed: 1000/interval_ms`, `high-speed: 8000/2^(bInterval-1)`).
  Tested against a synthetic sysfs tree.
- Permission model: devices listed even when unreadable, lock state shown, banner explains
  the `input` group vs targeted `uaccess` rule options. Never root.
- **Hardware validation is still outstanding on this machine** (no `/dev/input` read
  access). Historical evdev-session evidence from the previous port implementation:
  1000 Hz with strong confidence (196 events over 16.58 s, phase coherence 0.92) —
  evdev timing, never USB-report or descriptor timing.

### M4 — UI

- `ui/theme.cpp` (upstream palette/fonts/sizes), `ui/chart.cpp`, `ui/timeline.cpp`,
  `ui/main_window.cpp`, `ui/dialogs.cpp`.
- Top bar (File/Help/Capture menus, centered rate readout, Freeze, record button), banner,
  3×2 chart grid, 25 px analysis strip, timeline splitter, drag-and-drop file open.
- States: idle, recording (record = Stop), analyzing, downloading (record = Cancel for
  URL import), result, insufficient data, permission, disconnect.
- Chart interactions ported from `UI/Chart.cs`: `ZoomChange` wheel (1.05^scrolls around
  the cursor), pinned-cursor pan with direction lock (horizontal pan / vertical
  drag-zoom at the captured x), scrollbar thumb and end-notches with `Cramp` easing,
  track-click page jump, per-component hover/captured colors, `DrawShadowString` text,
  `XAxisMicroDecimals` axis labels with gray ".000" remainders, hover bubbles with
  micro-decimal segments, double-click spotlight reparenting the chart above the grid.
- Timeline: rows grouped by source, hold bars with event-count gradient, right-aligned
  key labels, upstream YText drag-reorder, context menu (color/visibility/copy),
  `ScreenInterval`-series axis with micro decimals, drag-and-drop open.
- Capture-kind toggles (Keyboard ✓ / Gamepad-Joystick ✓ / Mouse ✗, CheckOnClick) filter
  what starts recording, mirroring the upstream device-kind capture menu.
- Timeline edits (color/visibility/row order) are synced back into the shared recording
  (`TimelineWidget::sync_edits_into`) guarded by id-set + event-count equality, so they
  survive re-analysis and are saved to `.kbi`.

### M5 — File formats, descriptors, TETR.IO

- `core/kbi.cpp`: v3 writer/reader; readers for legacy v0 (2-byte UTF-16 `char`
  discriminants, typed ids 0=Keyboard/1=Gamepad/2=Mouse/3="TETR.IO Replay"), v1 (string
  ids), v2 (InputInfo block); creator-string-aware "newer version" errors.
- `core/keynames.cpp`: `.NET Keys` alias table (last-declared-wins semantics), legacy
  gamepad (Button1–32, HatN–W), mouse, and TETR.IO key name decoders for legacy files.
- `core/tetrio.cpp`: `.ttr`/`.ttrm` JSON parsing; multiplayer match-up inspection with
  per-round per-player entries (side matched by id, winner by alive flag); recording
  construction on the 600 Hz subframe grid (time = round(frame+subframe)/60,
  bin_rate=600, `MeasurementKind::TETRIO`).
- `ui/dialogs.cpp`: About (Qt6/licensing text), Import-from-URL dialog with clipboard
  auto-fill and live format detection (upstream `FileSystem.FileFormat` URL filters,
  `.kbi` default filter, TETR.IO link rewriting to the replay API), TETR.IO round picker,
  one-session TETR.IO disclaimer.
- Download flow: `QNetworkAccessManager` with `ManualRedirectPolicy`, 30 s transfer
  timeout, upstream User-Agent pattern, verbatim status-code error messages, record
  button acting as Cancel while downloading.
- Update check: startup one-shot (skipped in screenshot mode) parses the
  `/releases/latest` redirect tag against `APP_VERSION`; on mismatch a hidden top-bar
  "Update available!" button appears and opens the releases page.
- `.kbi` field order cross-checked against upstream `Result.cs`, `Event.cs`, `Input.cs`,
  `InputInfo.cs`, `Source.cs`, `Analysis.cs`. **Opening a generated file in the Windows
  app remains the final compatibility check (user-assisted).**

### M6 — Packaging and docs

- `packaging/`: `PKGBUILD` (C++/CMake build), `.desktop` entry, icon, targeted udev rule
  (`71-` prefix with rationale) and installer script.
- Docs rewritten for the C++ port: `README.md`, `AGENTS.md` (linux + root), `docs/*`.
- Package verified: `makepkg -f --nodeps` from an isolated copy runs `check()` (both test
  suites pass) and produces a clean payload — `usr/bin/keyboard-inspector`, desktop entry,
  icon, udev-rule examples under `share/doc` (rules are never auto-installed), license.

## Verification log (dated)

- **2026-10-26 (mouse/gamepad capture)** — mouse and gamepad capture made functional
  beyond buttons: `EvdevReportTracker` (capture/evdev_backend) adds (1) gamepad hat
  switch → `HatN/HatE/HatS/HatW` inputs, a direct port of upstream
  `UnprocessedInput.ProcessHID` (GamepadKeys 32–35 bitmap diff), and (2) one synthetic
  `Movement` input per SYN report with axis activity — the documented Linux addition
  (`COMPATIBILITY.md`, `MEASUREMENT_LIMITS.md`) that makes mouse/touchpad/gamepad
  polling rate measurable, since upstream drops all movement (`ButtonFlags == 0`).
  Buttons keep the upstream model and names (`LeftClick`…, `Button1`…).
  Evidence: 3 new tracker test cases (47 / 484 pass); synthetic sequences cover
  per-report counting, alternating pressed state, hat direction mapping in upstream
  bit order, gamepad-only hats, and SYN_DROPPED resync.
- **2026-10-26 (rate-estimate fix: fundamental selection)** — the live 2.4G Mouse
  session (saved as `linux/recordingmouse.kbi`, 2148 Movement events + 16 LeftClick
  over 20.864 s) exposed an estimate bug: the device reports every 4th USB frame
  (8 ms = 125 Hz cadence, 98.3% of consecutive deltas) but evdev timestamps are
  SOF-locked to the 2 ms frame grid, so circular/all-pairs spectra grew a taller
  500 Hz harmonic and the estimate reported 499.99 Hz "strong". Fix in
  `core/analysis.cpp` `estimate_rate` (documented addition, `ALGORITHM.md` §8):
  (1) harmonic-family downgrade — top candidate that is an integer multiple of another
  candidate with ≥ half its prominence yields to the slower fundamental; (2) modal-gap
  reconciliation — when the spectral winner is an integer multiple of the rate implied
  by the modal consecutive gap, report the gap cadence; (3) phase-refine multipliers
  capped after either guard. Evidence: mouse file now estimates **125.05 Hz (strong,
  phase 0.905)**; URX75 file unchanged at 999.985 Hz (strong); synthetic regression +
  true-500 Hz guard tests; **49 cases / 488 assertions pass**. Real-hardware mouse
  validation complete: measured 125 Hz vs 500 Hz configured descriptor maximum
  (descriptor ≠ measurement).
- **2026-10-26 (hardware validation record)** — long URX75 typing session recorded and
  saved as `.kbi` (`/home/adamya/projects/Keyboard-Inspector/linux/recording.kbi`):
  890 events over 19.852 s, 0 zero-ms differences, re-analyzed offline with the port's
  own pipeline → **999.985 Hz, strong confidence, phase coherence 0.963**, peak in all
  three spectra. Matches the descriptor's configured 1 ms interval while remaining a
  distinct evdev-layer measurement. Full record in `MEASUREMENT_LIMITS.md`
  ("Long-session validation run"). Together with the 2026-10-25 first run, the
  evdev-layer hardware validation for the URX75 is complete.
- **2026-10-26 (quality batch)** —
  - **Adaptive dropdown sizing (user-reported overflow)**: every top-bar dropdown now
    stays inside the screen — app style forces `SH_Menu_Scrollable` (Fusion `QProxyStyle`),
    per-menu height capped to the space below the anchor, long labels (device names,
    USB summary) elided with full text kept in the tooltip; removed the fixed
    360 px devices-menu cap. Evidence: new UI test "Capture and device dropdowns stay
    within the screen" (3 cases / 22 assertions pass), offscreen + Wayland renders.
  - **TETR.IO parser tests**: JSON-fixture coverage for solo `.ttr` (600 Hz subframe
    grid, hoisted/non-key drops, title, forced bin rate), multiplayer `.ttrm` (id
    matching, matchup, round labels, degenerate rounds, selected-player build) and
    error paths.
  - **FFT fallback**: replaced the naive O(n²) DFT with a built-in radix-2 FFT plus a
    Bluestein chirp-z for arbitrary sizes (chirp phases reduced `j² mod 2n` exactly).
    No-kissfft build (`-DVENDOR_KISSFFT=OFF`) passes the full suite in <1 s; cross-checked
    against a naive DFT and by frequency recovery.
  - **UI polish**: window geometry + splitter persistence via `QSettings` (guarded in
    screenshot/offscreen modes); removed the unused `charts_container` member.
  - Evidence: `keyboard-inspector-tests` 44 cases / 446 assertions pass;
    `keyboard-inspector-ui-tests` 3 cases pass; offscreen screenshot inspected; Wayland
    smoke render (`ki-geopersist.png`, `ki-wayland.png`) intact.
- **2026-10-26 (packaging)** — `packaging/PKGBUILD` written for the C++/CMake build;
  `makepkg -f --nodeps` from an isolated copy (`/tmp/opencode/pkgtest/linux/packaging`)
  succeeded, running both test suites in `check()` and producing a clean package payload
  (binary, desktop entry, icon, udev-rule examples, license).
- **2026-10-26** — Batches C/D/E of the Windows-parity pass:
  - URL import + download flow, TETR.IO import (picker + disclaimer), Help menu
    (Documentation/Updates/Discord/Donate/About), update check with top-bar notification,
    capture-kind toggles, freeze auto-uncheck on load.
  - Chart fidelity: notches + Cramp, pinned-cursor pan/drag-zoom, shadow text,
    micro-decimal axis + hover segments, spotlight reparenting.
  - Timeline: micro-decimal axis with shadows, label-area row drag-reorder with edit sync.
  - Robustness: kissfft-missing startup warning, evdev fallback timestamp domain locking,
    "Dropped auto-repeats" diagnostics action, `usb_info` tests (interval parsing, speed
    rules, synthetic sysfs walk).
  - Evidence: `keyboard-inspector-tests` 37 cases / 193 assertions pass;
    `keyboard-inspector-ui-tests` 2 cases pass; offscreen + Wayland screenshots
    (`/tmp/opencode/ki-e.png`, `ki-e-wayland.png`) inspected with a generated 1000 Hz
    sample recording.
- **Earlier C++-port sessions** — milestones M1–M5 as listed above; see git history of
  the working tree (uncommitted at the time of writing).

## Known gaps

- Demo/synthetic capture backend: intentionally not implemented.
- Opening a generated `.kbi` in the Windows app (final format check, user-assisted).
- `usbmon`/USB-report timing layer: intentionally not implemented (no evdev↔USB-transfer
  comparison is possible).
