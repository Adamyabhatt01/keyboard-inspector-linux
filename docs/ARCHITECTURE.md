# Architecture — Linux Port (C++ / Qt 6)

How the code is organized, how state flows, and which thread owns what. Read alongside
`UPSTREAM_AUDIT.md` (what the Windows original does) and `ALGORITHM.md` (the math).

## Modules

```
src/keyboard_inspector/
  main.cpp                 CLI parsing, logger setup, screenshot runner
  core/                    pure data + math, no widgets
    model.h/.cpp           Event, InputId, InputInfo, SourceInfo, Recording, settings
    recording.h/.cpp       RecordingSession: live ingestion + finalization
    analysis.h/.cpp        analyze(): events snapshot -> AnalysisResult (pure)
    kbi.h/.cpp             .kbi v3 read/write + legacy v0–v2 readers
    tetrio.h/.cpp          TETR.IO .ttr/.ttrm JSON -> Recording
    keynames.h/.cpp        key-code -> display names (Linux + .NET aliases)
    logger.h/.cpp          leveled logging, crash + Qt handlers
  capture/                 kernel interfaces, no Qt widgets
    base.h/.cpp            DeviceInfo, CaptureEvent, CaptureError, backend contract
    evdev_backend.h/.cpp   libevdev enumeration + per-device reader threads
    usb_info.h/.cpp        sysfs USB descriptor discovery (configured, not measured)
  ui/                      Qt widgets; owns all user-visible state
    main_window.h/.cpp     state machine, menus, timers, analysis scheduling
    chart.h/.cpp           frequency/time-domain line chart widget
    timeline.h/.cpp        key-history widget (rows, holds, drag-reorder)
    dialogs.h/.cpp         About, Import-from-URL, TETR.IO picker + disclaimer
    theme.h/.cpp           palette, fonts, metrics
tests/
  test_core.cpp            Catch2 core suite
  ui_freeze_test.cpp       offscreen UI smoke suite
```

Dependency direction is strictly `ui -> capture -> core`; `core` depends on Qt Core only
(no Widgets), so it is testable headlessly.

## Threading model

| Thread | Work | Communication |
|---|---|---|
| Qt GUI thread | all widgets, recording-state mutation, result application | Qt event loop |
| Capture reader (one per device) | `select()` + `libevdev_next_event`, timestamp, filter, queue | thread-safe event/error queues |
| Analysis worker (`QtConcurrent`) | `analyze()` over an immutable snapshot | `QFutureWatcher` + generation token |

Rules:

1. The GUI `QTimer` (~20 ms) is the **only** place that drains capture queues and changes
   recording state. Reader threads never touch widgets.
2. Analysis submits take a copy of the events/settings; the token (`_analysis_generation`)
   increments on every submit. `_on_analysis_finished` discards stale results.
3. While recording, a tentative **rate-only** analysis runs at most every ~750 ms
   (`_tentative_timer`); the queue never backs up.
4. A final analysis runs on stop over the finalized recording.

## State machine (MainWindow)

States: `idle`, `recording`, `analyzing`, `downloading` (+ transient banner states).

| Transition | Trigger | Effect |
|---|---|---|
| idle -> recording | Start Recording (devices selected, kind toggles allow) | create `EvdevBackend` + `RecordingSession`, reset charts, start timers |
| recording -> idle | Stop Recording (or 30-minute cap) | stop backend, drain queue, finalize recording, run final analysis, freeze merge |
| recording -> idle | device disconnect | synthesize releases for held keys of that source, mark lost, keep data |
| idle -> downloading | Import from URL accepted | `QNetworkAccessManager` GET (redirects disabled); record button = Cancel |
| downloading -> idle | finished/cancel/error | parse payload or show verbatim status error |
| idle -> analyzing | file loaded / settings changed | schedule analysis; results applied by generation token |
| analyzing -> idle | analysis done | charts + rate readout update |

The record button is context-sensitive: Start Recording / Stop Recording / Cancel
(download). Menus are disabled while a recording is active, matching upstream.

## Capture pipeline

```
/dev/input/eventN
  └─ EvdevBackend::reader_thread(info, fd)
       select(timeout 100 ms) ─ libevdev_next_event
         ├─ drop EV_SYN/EV_MSC/EV_LED
         ├─ drop EV_KEY value 2 (auto-repeat; counted -> repeat_dropped())
         ├─ timestamp = ev.time (kernel domain; fallback clock locked to the
         │   domain the first valid event matches: CLOCK_MONOTONIC/REALTIME)
         └─ CaptureEvent{source, key, pressed, timestamp} -> queue
GUI QTimer (20 ms): drain queues -> RecordingSession::add() -> dedupe/visibility/freeze
```

`RecordingSession::build(duration)` finalizes: dedupe, source naming (` [1]`, ` [2]`),
80% visibility rule, held-key release synthesis, freeze merge, `Recording` snapshot.

## Analysis pipeline

```
Recording snapshot (immutable copy) + AnalysisSettings
  └─ analyze(): three distributions -> binning -> rfft -> low cut -> reverse HPS
       -> normalization -> peak readout (confidence-gated) -> AnalysisResult
  └─ GUI: token check -> charts, analysis strip, rate readout
```

Details and parameter ranges: `ALGORITHM.md`. FFT: kissfft when available, otherwise a
built-in radix-2 / Bluestein FFT in O(n log n) (startup warning logged; see
`ALGORITHM.md` §3).

## File formats

- `.kbi` v3 — written by `core/kbi.cpp`; readers cover v0–v3. v0 uses 2-byte UTF-16
  `.NET BinaryReader.ReadChar` discriminants and typed source ids; v1+ use 7-bit-length
  strings and int64 ids; v2+ carry the InputInfo block. Field order mirrors upstream
  `Result.cs`/`Event.cs`/`Input.cs`/`InputInfo.cs`/`Source.cs` (see `COMPATIBILITY.md`).
- `.ttr`/`.ttrm` — JSON; `core/tetrio.cpp` maps frames/subframes to the 600 Hz grid.
- Save dialog always writes v3. Legacy content round-trips to v3 on save.

## UI structure

```
MainWindow
  ├─ top bar (24 px): File · Help · [rate readout] · Capture ▾ · Freeze · Record
  ├─ banner (permission/error/info, auto-hide or persistent)
  ├─ splitter (68/32)
  │   ├─ charts container: [spotlight slot] 3×2 chart grid (3 px gaps) + analysis strip (25 px)
  │   └─ timeline container: TimelineWidget (rows, holds, scrollbar, axis)
```

Spotlight reparents a chart above the grid (upstream `MainForm.cs:150-168`); a second
double-click restores its grid cell.

## Persistence and session state

- No global settings beyond the window/session state; every recording is a self-describing
  `.kbi`. Chart zoom/viewport reset on new data; Freeze carries visibility/colors across
  consecutive recordings; loading a file unchecks Freeze.
- Timeline edits (color, visibility, row order) are held in the widget's working copy and
  synced into the shared recording via `sync_edits_into` (guarded so stale copies are
  no-ops), then persisted when saving `.kbi`.
- Logs: `~/.local/state/keyboard-inspector/keyboard-inspector.log` (level Debug always
  written to file; console level from `--debug/--quiet`).
