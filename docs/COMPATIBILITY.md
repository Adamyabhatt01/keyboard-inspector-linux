# Compatibility: Windows Upstream vs Linux Port

This document is the authoritative list of intentional and unavoidable differences. If a
behavior is not listed here, the port intends to match upstream.

## Capture

| Aspect | Windows upstream | Linux port |
|---|---|---|
| API | Raw Input (`WM_INPUT` + `GetRawInputData`) | evdev (`/dev/input/event*`) via libevdev |
| Timestamp point | `Stopwatch` read at `WM_INPUT` in the listener window | kernel `input_event` timestamp (CLOCK_MONOTONIC), assigned by the input subsystem |
| Clock | monotonic `Stopwatch` | `CLOCK_MONOTONIC`; `evdev` exposes sec/usec |
| Timestamp latency | application message dispatch + raw input queue | kernel event delivery to the input core |
| Time origin | listener `StartRecording()` starts a `Stopwatch` | first accepted event; recording start is used as fallback. See `MEASUREMENT_LIMITS.md` |
| Privileges | none beyond a normal desktop session | read access to `/dev/input/event*` (not root GUI) |
| Device identity | raw input device handle (HID interface path) | sysfs path + HID `uniq`/`phys`; `.kbi` int64 is a stable hash |
| Auto-repeat | `Result` dedupes consecutive same-state events | kernel repeat (`value == 2`) dropped at capture; same-state dedupe still applied |
| Grab/exclusive mode | no | no (`EVIOCGRAB` is never used) |
| Keyboard layout | virtual-key names (`A`, `D1`, `ShiftKey`, ...) | physical `KEY_*` codes mapped to Windows-compatible labels |
| Gamepads/mice | Raw Input keyboard/mouse/HID | evdev keyboards, mice and gamepads; mouse buttons use upstream labels, gamepad hats become `HatN/E/S/W` events |
| Multi-device | raw input device handle per event | one reader thread per selected `/dev/input/eventN`; source id derived from sysfs |
| Hotplug | new raw-input device is listed on next enumeration | `QFileSystemWatcher` on `/dev/input` + 3 s poll; new keyboards/gamepads are auto-selected, locked ones raise a permission banner |

Consequence: Linux timestamps are taken *before* the application sees the event, while
upstream timestamps are taken *after* Windows delivered it to the app. The Linux result can
therefore look slightly tighter/earlier. Never compare absolute latencies across platforms;
compare distributions and peaks.

## Timing layers (Linux only)

The Linux port can expose three different things. They are never interchangeable:

1. **evdev event timing** — default. Kernel input-subsystem event timestamps.
2. **USB HID report timing** — optional `usbmon` backend. Per interrupt-IN completion.
3. **USB descriptor interval** — configured `bInterval` from sysfs. A capability, not a
   measurement. For the URX75 full-speed interface this is 1 ms (1000 Hz configured
   maximum), which does **not** prove the keyboard reports at 1000 Hz.

See `MEASUREMENT_LIMITS.md` for the full model and reporting rules.

Verified on the URX75 (2026-10-25): the boot-protocol node (`event3`, interface 0) emitted
nothing while the report-protocol node (`event7`, interface 2) carried keystrokes. Default
selection now prefers the report-protocol node. A real evdev session estimated 1000 Hz at
strong confidence (196 events over 16.58 s, phase coherence 0.92). No USB-report timing
was measured.

## Mouse and gamepad capture (documented addition)

Upstream records **buttons only** for mouse and gamepad: `UnprocessedInput.ProcessMouse`
drops every raw-mouse event with `ButtonFlags == 0` (no movement is recorded), and
`ProcessHID` records HID buttons plus the hat switch as `GamepadKeys` inputs. The Linux
port matches that button model (EV_KEY → `LeftClick`…/`Button1`… with the same upstream
names) and adds two evdev-only layers, both documented here:

1. **Hat switch** — `ABS_HAT0X/ABS_HAT0Y` on gamepad nodes becomes `HatN/HatE/HatS/HatW`
   press/release inputs, diffed against the previous report exactly like upstream's
   `GamepadKeys` 32–35 bitmap (`UnprocessedInput.cs:68-95`). First values adopt state
   without emitting; `SYN_DROPPED` resets it.
2. **Movement inputs** — every SYN-delimited report carrying at least one `EV_REL` or
   `EV_ABS` axis event produces one synthetic `Movement` input with an alternating
   pressed state. Upstream has no equivalent (movement is dropped on Windows); the
   addition exists because on Linux it is the only way to measure mouse/touchpad/gamepad
   polling rate (the same measurement the evhz tools make). One event per report — not
   per axis event — so a dx+dy report counts once. The alternating state survives
   upstream-parity dedupe and renders as activity in the timeline.

Effects: input names `Movement`, `HatN`… can appear in recordings and `.kbi` files from
this port; the Windows original would not produce them. Analysis math is unchanged —
movement events are ordinary events in the same histograms/FFT pipeline.

## Analysis

Ported exactly:

- 1-second histograms, `B` bins, `round(v*B)`.
- consecutive diffs, all-pairs diffs (with the `j > i+1 && diff >= 1` break), timestamps
  modulo 1 s.
- `rfft` magnitudes, logistic low cut, reverse harmonic product (`copy[i*(j+1)/(HPS+1)]`),
  normalization by `max(1, max)`.
- bin rate 20..128000 validation; values below 20 are marked invalid and not analyzed
  (loaded files clamp to 20); HPS 0..9; low cut default on.
- input visibility: if the busiest source has more than 80% of events, hide the others.
- consecutive same-state event dedupe.

Documented divergences and additions:

| Difference | Reason | Impact |
|---|---|---|
| Circular time chart rotates the maximum to index 0 without upstream's every-other-bin loop bug | Upstream's loop increments `i` twice; it is a display-only defect. The intended rotation is implemented | Circular *time* chart only. FFT input and all other charts are unchanged |
| FFT selection is NumPy `rfft` instead of FFTW | One less system dependency; same DFT definition | floating-point noise only; 1 Hz bin spacing preserved |
| Polling-rate estimate | Product requirement; upstream expects a human to read the peak | New, confidence-gated, labeled as an addition |
| `EstimatePeak` is not used | Upstream's function is unused and its logic is not a reliable estimator | No behavioral loss |
| No FFTW wisdom file | NumPy has no wisdom concept | none |
| Analysis runs in a worker thread and results are generation-tagged | Keeps GUI responsive on large files | no mathematical change |
| Bin-rate/HPS/low-cut changes may be debounced ~150 ms | Avoid recomputing per keystroke | UI responsiveness only |
| Sub-20 bin rates are rejected instead of retained as upstream's invalid state | Never compute charts from an invalid histogram length; loaded values clamp to 20 | Upstream leaves old charts with an invalid setting; the port never analyzes below 20 |
| Frequency-chart y values may be normalized even when the raw maximum is below 1 | Clean future implementation uses `max(1, max)` explicitly, matching upstream's helper | Values are 0..1, same visual shape |

## User interface

- Same window proportions, 3×2 chart order, colors, top row concept, analysis strip and
  timeline. Exact pixel geometry is in `UI_SPEC.md`.
- Fonts differ (the upstream uses Microsoft Sans Serif 8.25 pt; Linux uses Noto/DejaVu
  Sans 9 pt). Metrics are therefore close but not pixel-identical.
- Menus are reorganized for Linux: `File`, `Capture`, `Help`; capture method and device
  selection replace upstream's Keyboard/Gamepad/Mouse capture checkboxes.
- New required elements: centered rate readout, backend/method label, permission banner,
  device menu with accessible/locked states. They are kept inside the existing top bar and
  a single banner strip so the chart identity is preserved.
- `Capture` menu includes a read-only USB descriptor summary. This is not an upstream
  concept.
- Timeline has a minimal header (`Timeline`, sample count, `Fit`) for discoverability; the
  original has no header. It does not change chart geometry.
- TETR.IO import is not ported in the first milestone; the menu item is absent/disabled.
- Spotlight, wheel zoom, drag pan, scrollbar, hover bubble, timeline context menu, and
  `.kbi` drag-and-drop are ported.

## File format

- `.kbi` version 3 is written with the same header (`KBI\0`), field order, .NET 7-bit
  length-prefixed strings, `DateTime.ToBinary` timestamp, event/source/analysis/input
  records. See `ALGORITHM.md` and `core/kbi.cpp`.
- `Creator` is `Keyboard Inspector Linux <version>`. Upstream will display the file
  normally; it does not know about the Linux capture method. The port does not add
  non-standard fields to v3, so files remain valid upstream.
- Reading v0-v2: the reader rejects unsupported legacy layouts with a clear error in the
  first milestone; v3 is fully supported. This is a documented gap, not silent misparsing.
- Source ids are stable hashes, not Win32 handles; they are opaque int64 values in `.kbi`.
- Colors are stored as .NET `Color.ToArgb()` int32 values.

## Final compatibility check

Opening a Linux-written `.kbi` in the unmodified Windows application is the final
interoperability check. It requires a Windows machine or VM and is listed as a
user-assisted validation in `TEST_PLAN.md`.
