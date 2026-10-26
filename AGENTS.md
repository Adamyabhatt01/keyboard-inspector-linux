# Linux Port — Agent Guide

## Mission

A native, unprivileged Linux application that is recognizably the same tool as the
Windows Keyboard Inspector: record keyboard/input events, build the same three timing
histograms, run the same FFT/low-cut/HPS pipeline, and present the same 3×2 charts and
input timeline, with a clearly labeled polling-rate estimate and separate configured
USB interval information.

Target: Arch Linux + Hyprland + Wayland, no X11 requirement.

## Stack and why

- **C++20 / Qt 6** (Core, Widgets, Gui, Concurrent, Network) — native Wayland support,
  high-DPI scaling, menus, splitters, custom painting. Built with CMake.
- **libevdev** — kernel event capture without `EVIOCGRAB`.
- **kissfft (optional)** — `rfft` for the frequency domain. When absent the build links a
  naive O(n²) DFT fallback in `core/analysis.cpp` and the app logs a warning at startup.
- **Catch2** — unit/UI tests. No other runtime dependencies.

Do not add matplotlib/pyqtgraph-like chart libraries. The charts are custom `QWidget`
painting to match the original geometry and interaction model. Do not add a reactive/web
UI framework. The optional usbmon/USB-report backend is deliberately not implemented.

## Code map (current)

```
keyboard-inspector            CMake root; build/ is the configured build tree
src/keyboard_inspector/
  main.cpp                    entry point, CLI args, screenshot runner
  core/
    model.h/.cpp              InputId, InputInfo, SourceInfo, Event, Recording, settings
    recording.h/.cpp          session ingestion, dedupe, visibility, freeze, releases
    analysis.h/.cpp           histogram/FFT/low-cut/HPS pipeline + confidence-gated readout
    keynames.h/.cpp           KEY_* / .NET Keys / gamepad / mouse / TETR.IO display names
    kbi.h/.cpp                .kbi v3 reader/writer + legacy v0–v2 readers (typed ids)
    tetrio.h/.cpp             TETR.IO .ttr/.ttrm JSON parsing → Recording (solo + multi)
    logger.h/.cpp             leveled logging, crash/Qt message handlers
  capture/
    base.h/.cpp               CaptureBackend interface, DeviceInfo, CaptureEvent, errors
    evdev_backend.h/.cpp      libevdev capture, sysfs enumeration, reader threads
    usb_info.h/.cpp           sysfs USB parent/endpoint/descriptor discovery
  ui/
    main_window.h/.cpp        top bar, menus, charts grid, analysis strip, timeline, states
    chart.h/.cpp              line chart: upstream axes, zoom/pan/notches, hover, scrollbar
    timeline.h/.cpp           key-history widget: rows, holds, drag-reorder, context menu
    dialogs.h/.cpp            About, Import-from-URL, TETR.IO picker + disclaimer
    theme.h/.cpp              palette, fonts, sizes derived from upstream source
tests/
  test_core.cpp               49 Catch2 cases: model, analysis (incl. FFT fallback),
                              kbi (v0–v3), keynames, capture (incl. hat/movement
                              tracker), usb_info (synthetic sysfs), tetrio (solo/multi/
                              error paths)
  ui_freeze_test.cpp          offscreen UI smoke tests
docs/                         audit, algorithm, architecture, UI, limits, tests, status
packaging/                    PKGBUILD, .desktop, icon, udev rule + installer
LICENSE  NOTICE               GPL-3.0-or-later + upstream BSD notice
```

## State and threading rules

- The Qt GUI thread only touches widgets and immutable snapshots.
- **Capture**: one reader thread per selected evdev node, using `select()` with a short
  timeout so stop requests are honored. Emits `CaptureEvent` records into a thread-safe
  queue. It never touches Qt widgets.
- A GUI `QTimer` drains the capture queue every ~20 ms. It updates the recording buffer,
  status, and live chart/timeline state. This is the only place recording state changes.
- **Analysis**: pure functions over a snapshot. Run in a single-worker
  `QtConcurrent`/`QFutureWatcher` pipeline; completed results are applied on a GUI poll
  with a monotonically increasing generation token; stale results are discarded. While
  recording, submit a tentative rate-only analysis at most every ~750 ms; never queue a
  backlog.
- **Stop**: stop accepting new events, flush the queue, finalize the recording, run the
  final analysis. Held keys extend to the stop timestamp in the timeline.
- **Disconnection**: if a reader fails (`select` error / libevdev error), synthesize
  releases for held keys from that source at the last-known time, mark the device lost in
  the UI, and keep the captured data.

## Recording rules (ported upstream behavior)

- Event time is the **kernel evdev timestamp** (`input_event.time`), not the time the
  reader thread dequeues it. Upstream uses `Stopwatch` elapsed at `WM_INPUT`; this is a
  documented Linux difference, not a bug (see `docs/COMPATIBILITY.md`).
- If a reader's first timestamp is unusable, the fallback clock is locked to the domain
  the first valid event matches (`CLOCK_MONOTONIC` vs `CLOCK_REALTIME`), so synthetic
  timestamps never mix domains within one capture.
- Drop `EV_KEY` value `2` (kernel auto-repeat); the count surfaces under
  Capture → "Dropped auto-repeats".
- Drop non-key events (`EV_SYN`, `EV_MSC`, `EV_LED`).
- Dedupe consecutive events for the same `(source, key)` with the same pressed state,
  matching upstream `Result` filtering.
- Group inputs by `(source, key)`. Default visibility: if the busiest source owns >80% of
  events, hide other sources (upstream `GetBestSource(0.8)`); otherwise show all.
- Append duplicate source names with ` [1]`, ` [2]`, etc. (upstream `Recorder.StopRecording`).
- Freeze: preserve the previous input rows, colors, and visibility for matching
  `(source, key)` pairs in the next recording; new rows default visible. Opening or
  loading a file silently unchecks Freeze (upstream `MainForm.cs:214-216`).
- Total recording time for held-key bars is the stop time relative to the time origin.

## Analysis rules

Read `docs/ALGORITHM.md` before editing `core/analysis.cpp`. The key invariants:

- Bin rate `B` is both the number of bins in the 1-second histogram and the FFT length.
  Valid `B >= 20`, capped at 128000.
- Histogram bin index = `round(v * B)`; modular data wraps `bin %= B`; non-modular drops
  `bin >= B`.
- Three sources: consecutive event deltas, all pairs (<1 s cutoff with the upstream
  adjacent-pair exception), and timestamps modulo 1 s.
- Real FFT output length `B//2 + 1`; frequency resolution is 1 Hz because the histogram
  spans 1 second.
- Low cut: for `i < min(70, len)`, `data[i] /= 1 + exp(-(i-25)/4)`.
- HPS: `data[i] *= copy[int(i*(j+1)/(HPS+1))]` for `j in 0..HPS-1`, then normalize by
  `max(1, max(data))`; display x-factor `1/(HPS+1)`. This is the upstream **reverse**
  harmonic product; do not replace it with the textbook forward HPS.
- Circular time chart: upstream rotates the histogram so its maximum is first and has an
  off-by-one stride bug. We implement the intended rotation (max first) and document the
  display-only divergence; FFT is unaffected.
- Peak readout is an addition required by the product spec. Use the processed spectra,
  find prominent local maxima above ~50 Hz, and report only when sample size and peak
  prominence are sufficient. Never report the descriptor interval as this value.

## UI rules

- Reproduce upstream layout proportions and controls: 24 px top bar, 3×2 charts with 3 px
  gaps, 25 px analysis strip, bottom timeline, default ~1294×836, min ~720×500.
- Colors: time charts `#418CF0`, frequency charts `#F07800`, chart text `#A0A0A0`,
  chart background near-black, menu `#3C3F41`, text `#DCDCDC`, textbox `#45494A`.
- Keep the original dark identity. Do not add light theme, cards, gradients, or animations
  beyond the original subtle chart gradient.
- Ported interactions live in `ui/chart.cpp` / `ui/timeline.cpp` and mirror upstream:
  wheel `ZoomChange` (1.05^scrolls around the cursor), pinned-cursor pan with direction
  lock (≤30° horizontal pan, ≥60° vertical drag-zoom, cursor pinned at the capture point
  with accumulated offsets), scrollbar thumb + end-notches with `Cramp` easing
  (`Chart.cs:602-628`, s=0.0004), track click page jump, `DrawShadowString` text stamping,
  axis labels with `XAxisMicroDecimals` (3 on the timeline/time-domain axis, gray ".000"
  micro remainders), hover bubbles with micro-decimal segments (`Chart.cs:1555-1617`),
  double-click spotlight reparenting the chart above the grid (`MainForm.cs:150-168`),
  timeline row drag-reorder (upstream YText drag).
- Add only what the product spec requires: prominent labeled rate estimate, device
  selector, backend/method label, permission/error banner. These must not crowd out or
  restyle the original controls.
- The estimated rate belongs in the centered top-bar status area; use a clear
  "estimated / tentative / insufficient data" state and a tooltip explaining the method.
- Every timing value must carry its unit and source.

## Capture and permissions

- Default backend: evdev via libevdev. Enumerate `/dev/input/event*`, read names and
  capabilities, try to open. Show inaccessible devices with a lock state and a permission
  banner; never crash or request root automatically.
- The app must **not** call `grab()`; other applications must keep receiving input.
- Access options to document: add the user to `input` (broad), or install a targeted udev
  `uaccess` rule (narrow; requires re-plug/trigger). Never run the GUI as root.
- Detecting a device is not the same as having permission. Surface both states.
- USB descriptor lookup: walk sysfs from the evdev node to the USB interface/device; read
  vendor/product/speed and interrupt-IN `interval`/`bInterval`/`wMaxPacketSize`. Label it
  "configured interval", separate from measured values.

## Testing and verification

- Run both suites after every significant change:
  ```bash
  ./build/bin/keyboard-inspector-tests      # 49 cases / 488 assertions
  ./build/bin/keyboard-inspector-ui-tests   # offscreen UI smoke tests
  ```
- Synthetic sequences must recover known frequencies (e.g., 1 ms → 1000 Hz; 2 ms → 500 Hz;
  0.25 ms → 4000 Hz at a sufficient bin rate), handle identical timestamps, repeats,
  sparse/noisy data, and multiple sources.
- Render `--screenshot` offscreen and inspect the PNG; then smoke-test under
  `QT_QPA_PLATFORM=wayland`.
- Hardware validation requires read access to `/dev/input/event*`. If unavailable, say so
  explicitly; do not claim a measured rate from it.
- Cross-check `.kbi` field order against upstream `Result.cs`, `Event.cs`, `Input.cs`,
  `InputInfo.cs`, `Source.cs`, `Analysis.cs`. Document that opening in the Windows app is
  the final compatibility check.

## Documentation update checklist

After any behavior change, update:

- `docs/IMPLEMENTATION_PLAN.md` (status + verification evidence)
- `docs/ALGORITHM.md` if math/parameters changed
- `docs/UI_SPEC.md` if layout/colors/interactions changed
- `docs/MEASUREMENT_LIMITS.md` if capture or labeling changed
- `docs/COMPATIBILITY.md` if a divergence from Windows is introduced
- `README.md` if user-facing setup/usage changed
