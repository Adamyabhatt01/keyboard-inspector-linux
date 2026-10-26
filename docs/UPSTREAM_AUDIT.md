# Upstream Audit — Keyboard Inspector (Windows)

Reference checkout: `mat1jaczyyy/Keyboard-Inspector`, branch `master`, commit
`60901cb` ("Support smaller window size"). All paths below are relative to
`Keyboard Inspector/`.

This audit is the factual basis for the Linux port. If upstream changes, re-audit and
update this file with new line references.

## 1. Capture path

### Registration and window

- `Recording/ListenerWindow.cs:25-71` creates a hidden Win32 window on a dedicated thread
  at `THREAD_PRIORITY_TIME_CRITICAL` (15).
- `ListenerWindow.cs:90-120` registers Raw Input usages while recording:
  keyboard (Generic desktop / Keyboard), gamepad + joystick, and optionally mouse.
  `InputSink` is used so the listener receives input even when not focused.
- `ListenerWindow.cs:74-85` handles `WM_INPUT` only while `Recorder.IsRecording`.
  It timestamps **immediately** with `Recorder.ElapsedPrecise` before parsing the packet.
  That timestamp is application receipt time (Stopwatch), not hardware/USB time.
- Parsing is queued to a worker task (`ListenerWindow.cs:31-35`) to keep timestamping light;
  the UI thread is kept static/empty during recording because capture and timestamping
  share the UI thread (`DOCUMENTATION.md:3-5`).

### Keyboard event extraction

- `Recording/UnprocessedInput.cs:11-31`:
  - Rejects make code `0xFF` (keyboard overrun) and VKey `>= 0xFF`.
  - Extends the scan code with `E0`/`E1` flags.
  - Resolves Shift/Control/Alt left vs right via `MapVirtualKey(... MAPVK_VSC_TO_VK_EX)`.
  - `Pressed = !RI_KEY_BREAK`; one event is recorded per `WM_INPUT`.
  - Input identity = `(VKey name, raw device handle)`.
- Gamepad HID parsing (`UnprocessedInput.cs:44-100`) tracks button/hat state and emits
  changes. Mouse parsing (`33-42`) emits button changes only. These are out of scope for
  the initial Linux port.
- `Recording/Recorder.cs:16-30` starts a new event list and Stopwatch; `56-63` appends
  events and counts per source handle.
- `Recording/Recorder.cs:32-53` stops, resolves raw handles to `Source` objects, appends
  ` [n]` to duplicate source names, builds `Result`, and sorts inputs by device.
- `Result/Source.cs:115-130` resolves a friendly source name from the raw interface:
  CfgMgr device name first, then HID product string, then the interface path.

### Windows auto-repeat

- `Result/Result.cs:27-38` removes consecutive events for the same `Input` with the same
  `Pressed` state. This is how Windows auto-repeat is eliminated; there is no explicit
  repeat flag at this layer.

## 2. Data model and visibility

- `Result/Event.cs:5-30`: `Time` (double seconds), `Pressed` (bool), `Input`.
- `Result/Input.cs:8-64`: `Key` string + `Source` int64; identity is the pair.
- `Result/InputInfo.cs:6-34`: per-input color, visibility, event list.
- `Result/Source.cs:7-15`: count, name, device interface.
- `Result/Result.cs:24-103`:
  - Builds `InputInfo` groups while filtering repeats.
  - Supports a "Freeze" mode that preserves rows/sources from the previous result
    (`Program.cs:31-42`); events are new, but input colors/visibility/rows persist.
  - Non-frozen default visibility: `GetBestSource(0.8)` — if the busiest source has more
    than 80% of all events, only inputs from that source are visible.
- `Result/Result.cs:105-141`: title from best source (0.66) or "Multiple devices",
  visible-input filtering, event selection, sorting by source name.

## 3. Analysis pipeline (exact)

File: `Result/Analysis.cs`.

### Parameters and validation

- `BinRate` default 4000, clamp 0..128000, considered valid only when `>= 20`
  (`Analysis.cs:13-18`). The number of histogram bins equals `BinRate` over a 1-second
  span; bin width = `1/BinRate` seconds.
- `HPS` default 0, clamp 0..9 (`Analysis.cs:20-24`).
- `LowCut` default true (`Analysis.cs:26`).

### Event sets

- `CalcDiffs` (`47-52`): `diffs[i] = e[i].Time - e[i-1].Time`.
- `CalcCompound` (`54-68`): all pairs `j > i`; for `j > i+1`, break when `diff >= 1`.
  The first non-adjacent pair at/above 1 s is not included, but an adjacent pair can be.
- `CalcCircular` (`70-76`): `e[i].Time % 1`.

### Binning

- `Bin` (`78-96`): `bin = round(v * B)`.
  - Non-modular: skip `bin >= B` (intervals >= 1 s).
  - Modular: wrap `bin %= B`.
  - `.NET Math.Round` default is midpoint-to-even; Python `round`/`np.rint` matches.
- `CircularRotationFix` (`98-113`) is applied only to the circular time-domain chart:
  it finds the maximum bin and rotates the array so the maximum is first.
  The loop increments `i` twice (`i++` in the body and in the `for`), so it checks every
  other bin. This is a display-only quirk; FFT input is not rotated.

### FFT

- `RunGraphJob` (`180-194`): time chart uses x-factor `1000/B` (ms per bin).
  FFTW.NET real FFT input length `B`, output length `B/2 + 1`.
  Magnitude = `sqrt(re^2 + im^2)`.
- Because the histogram spans exactly 1 second, FFT bin spacing is 1 Hz, so **bin index
  equals frequency in Hz before HPS scaling**.

### Low cut

- `ApplyLowCut` (`142-148`): if enabled, for `i < min(70, len)`:
  `data[i] /= 1 + exp(-(i - 25) / 4.0)`.
  This is a logistic attenuation of ~0-70 Hz, reaching ~1.0 around i=70.

### Harmonic product (upstream variant)

- `HarmonicProduct` (`150-160`) + `RunFromHPS` (`206-215`):
  ```
  copy = data.copy()
  for i in range(len(data)):
      for j in range(HPS):
          data[i] *= copy[int(i * (j + 1) / (HPS + 1))]
  data /= max(1, data.max())
  x_factor = 1 / (HPS + 1)
  ```
  For HPS=2 this multiplies by `copy[i/3] * copy[2i/3]`, i.e. it finds a fundamental from
  higher harmonics and scales the x-axis down. This is the **reverse** of textbook HPS
  (`copy[2i] * copy[3i]`). Port it exactly; see `ALGORITHM.md`.
- `Normalize` (`162-167`): divide by `max(1, max(data))`.

### Peak detection in upstream

- `EstimatePeak` (`115-135`) exists but is **never called** (verified by grep across the
  source). It also has quirky initialization (`max = double.MaxValue`, result `-1`).
- Upstream does **not** display an automatic polling-rate number. Users are expected to
  interpret the largest FFT peak manually (`README.md`, `DOCUMENTATION.md:43-57`).
- The Linux port keeps the same charts and adds an explicitly labeled, confidence-aware
  estimate (required by the port spec). This is an addition, not a claimed upstream feature.

## 4. UI: layout, colors, interactions

### Window

- `UI/MainForm.Designer.cs:567-586`: default client 1294×836, minimum 720×500, icon
  `keyboard.ico`.
- Top band is 24 px (`mainmenu` size); charts/timeline start below it (`split.Location.y=24`).
- `Freeze` checkbox at top-right, `Start/Stop Recording` button at far top-right
  (`1102,3` and `1172,0`), status label centered (`324,5`), `Capture` menu right-aligned
  with a 200 px right margin, `File` and `Help` at left.
- Vertical `SplitContainer`: top = charts + analysis strip (default 553 px), bottom =
  timeline (default ~251 px), min top 250 / bottom 125. The splitter is visible only when a
  result exists (`split.Visible = false` initially).

### Charts

- `TableLayoutPanel`: 3 columns × 2 rows, 3 px gaps, 50/50 row heights; each chart is a
  custom `Chart` control.
- Columns: consecutive diffs, all diffs, circular/wrapped timestamps.
- Rows: time domain, frequency domain.
- Time charts color `#418CF0`; frequency charts color `#F07800`
  (`Designer.cs:477-553`).
- Chart title is centered at the top; time charts get a default 32 ms view
  (`ScopeDefaultX=32`), frequency charts show the full spectrum.
- Chart drawing (`UI/Chart.cs`):
  - Background `BackColor.Blend(Black, 0.9)` (very dark).
  - Text `#A0A0A0`; x-grid `#141414`; y-grid `#2C2C2C`; scrollbar `#5C5C5C`,
    hover `#7A8084`, captured `#9EB1C3`.
  - Line color = chart `ForeColor`, drawn with a vertical gradient (line color to ~50%
    background). Small dots appear when x-units are wide enough.
  - Six horizontal grid lines; no y-axis numeric labels.
  - X-axis labels use an adaptive interval generator; time charts use
    `ScreenInterval(i)*1000` ms, frequency charts use 5/10/25/50/125/250/500/1000... Hz.
  - Bottom scrollbar with drag/notch/jump interactions; wheel zoom (factor `1.05` per
    notch); drag pan; hover marker + coordinate bubble; double-click spotlight.
  - Circular time chart applies the rotation quirk described above.
- Timeline (`Kind.KeyHistory` in `Chart.cs`):
  - Rows sorted by source name then source id; source names shown when multiple sources.
  - Key labels right-aligned; hold intervals drawn as vertical-gradient bars (not points);
    bars blend at edges and extend to recording end for a held key.
  - Row hover, drag-to-reorder, context menu: change/reset color per input or device,
    hide input/device, sort by device, show all, copy interface.
  - Event count label top-right with a red→orange→yellow→green gradient at
    50/200/600/1500 events (`MainForm.cs:365-379`).

### Analysis strip

- 25 px high, below charts (`MainForm.Designer.cs:154-170`):
  - "Binning Rate:" label at x=12, value box at x=83 (56 px, right-aligned), "Hz" at x=140,
    `/ 2` at x=166, `* 2` at x=195.
  - "Low Cut Filter" checkbox at x=261.
  - "HPS Partial Elimination:" label at x=375, spin box at x=494 (71 px), "partials" at x=566.
  - Event count right-aligned at x=1161.
- Changes re-run analysis immediately; bin rate changes invalidate bins, low cut/HPS
  re-run from cached spectra (`MainForm.cs:414-448`).

### Menus and flow

- File: Open… (Ctrl+O), Save As… (Ctrl+S), Import from URL… (Ctrl+Shift+O).
- Capture: Keyboard (checked), Gamepad/Joystick (checked), Mouse (unchecked).
- Help: Documentation, Updates, Discord, Donate.
- Start/Stop button text changes; while recording, the menu/freeze are disabled and status
  reads "Recording..."; the menu is re-enabled and results analyzed on stop
  (`MainForm.cs:44-115`).
- Drag-and-drop a supported file anywhere on the window loads it.
- No light theme; the original is dark-only.

### Available visual reference

`docs/reference-ui.png` is the upstream README banner (logo + wordmark), not a full
screenshot. The full layout above is derived from the Designer coordinates and the
Chart renderer, which is more precise than eyeballing a screenshot. Any future full
screenshot should be added here and referenced from `UI_SPEC.md`.

## 5. File format (`.kbi`)

- `Result/Result.cs:143-216`: header `KBI\0`, uint32 file version (current 3), strings,
  `DateTime.ToBinary()` int64, duration double, event list, source dictionary,
  analysis params, input list.
- Lists/dictionaries: int32 count, then entries (`Utility.cs:71-110`).
- Strings: .NET `BinaryWriter` = 7-bit encoded byte length + UTF-8.
- `Event`: double time, bool pressed (1 byte), Input.
- `Input`: string key, int64 source handle.
- `Source`: int32 count, string name, string device interface.
- `Analysis`: int32 bin rate, int32 HPS, bool low cut.
- `InputInfo`: Input, int32 ARGB color, bool visible.
- File version <2 has no input list; <3 has no creator string; version 0 has legacy
  one-char input encodings (`Input.cs:46-64`, `Result.cs:158-213`).
- Writes are portable; recordings contain user input and may contain sensitive text.

## 6. Gaps and risks relevant to Linux

1. **Windows-only capture** (`user32` Raw Input, `hid.dll`, CfgMgr). Linux needs evdev and
   a different timestamp source.
2. **Timestamp semantics differ**: upstream timestamps at application receipt; evdev
   timestamps in the kernel input subsystem. Neither is USB-level.
3. **Auto-repeat is implicit** in upstream dedupe; Linux also emits explicit `value=2`
   repeats, which must be dropped.
4. **No USB-level capture** upstream; the Linux port adds it as an optional, separate
   backend and must label the difference.
5. **No automatic rate readout** upstream; the port adds one from the same processed
   spectra, with confidence and caveats.
6. **HPS is reverse/textbook-unusual**; a "cleanup" would change results. Keep exact.
7. **Circular rotation** has a display-only stride bug; decide and document.
8. **UI fidelity** requires custom painting; generic chart libraries will not match.
9. **Permissions**: `/dev/input` is root:input; the GUI must remain unprivileged and guide
   the user through udev/group setup.
10. **Key names** differ: Linux `KEY_*` vs Windows `Keys` enum; `.kbi` compatibility needs
    a best-effort mapping.
