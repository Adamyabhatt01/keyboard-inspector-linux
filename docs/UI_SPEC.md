# UI Specification — Upstream-Faithful Linux Port

Primary reference: upstream `UI/MainForm.Designer.cs`, `UI/Chart.cs`, `UI/MainForm.cs`,
and the README banner (`docs/reference-ui.png`). No full upstream screenshot was available
in the repository; the geometry below comes from the Designer coordinates, which is more
precise. If a full screenshot is obtained, add it to `docs/` and reference it here.

## Visual identity

- A compact dark desktop utility, not a website: dense controls, 1 px borders, small type,
  no cards, no oversized padding, no decorative animation.
- Blue = time domain. Orange = frequency domain. Gray = text/grid/timeline.
- The only gradient is the subtle chart-line gradient and the timeline hold-bar gradient,
  both inherited from upstream.

## Window

| Property | Upstream | Linux port |
|---|---|---|
| Default size | 1294 × 836 | 1294 × 836 logical px, clamped to available screen |
| Minimum | 720 × 500 | 720 × 500 |
| Top band | 24 px menu/status/record row | 28 px top bar (24 px content + spacing) |
| Chart/timeline split | 553 / 259 px (vertical splitter) | QSplitter, stretch 68% / 32%, min 250 / 125 |
| Icon | `keyboard.ico` | same file copied from upstream |
| Title | `<recording title> – Keyboard Inspector <ver>` | `Keyboard Inspector Linux <ver>` + title when loaded |
| Theme | dark only | dark only (upstream has no light theme) |

High DPI: all sizes are logical; let Qt scale. Do not hard-code device pixels.

## Top bar (single 24-28 px row)

Left to right, matching upstream:

1. `File` menu: `Open…` (Ctrl+O), `Save As…` (Ctrl+S), `Import from URL…` (Ctrl+Shift+O),
   separator, `Exit`. Open/save use `.kbi`; open also accepts `.ttr`/`.ttrm`; URL import
   accepts a direct `.kbi` link or a tetr.io replay link (redirects disabled).
2. Centered status/measurement area (replaces upstream's blank status label):
   - Idle: `Estimated report rate: 1000 Hz` + small `estimated · evdev event timing` tag.
   - Recording: `Recording 12.3 s · 1000 Hz (tentative)`.
   - Insufficient: `Estimated report rate: insufficient data` (never a fake number).
   - Errors: permission/device message in the banner below, not here.
   - Tooltip/details: confidence, warnings (Nyquist, HPS active, small sample), and the
     explicit sentence “Derived from evdev event timing; not the USB descriptor interval.”
3. `Capture` menu (right-aligned before Freeze/Record):
   - Capture method: `Linux input events (evdev)` (default; the optional usbmon layer is
     not implemented).
   - `Devices` submenu: checkable device names; inaccessible devices show a lock glyph and
     a permission tooltip. Include a `Rescan` action.
   - Capture-kind toggles (upstream CheckOnClick): `Keyboard` ✓, `Gamepad/Joystick` ✓,
     `Mouse` ✗ — these filter what a recording captures.
   - Diagnostics (read-only, disabled): `Dropped auto-repeats: N` from the last session.
   - USB descriptor summary of the selected device (read-only): `36b0:3002 · 1 ms
     (configured, not measured)`.
4. `Freeze` checkbox.
5. `Start Recording` / `Stop Recording` button (label becomes `Cancel` while a URL import
   downloads), 122 × 24 px, right-aligned.
6. `Help` menu: `Official Documentation`, `Check for Updates`, `Discord`, `Donate`,
   `About`. When the startup update check detects a newer upstream release, a hidden
   `Update available!` button appears in the top bar and opens the releases page.

The button and Freeze must remain visible at 720 px width; the centered estimate text
elides with an ellipsis at narrow widths. Menus collapse, never overlap.

### Adaptive dropdown sizing

Every dropdown stays inside the current screen, regardless of device count, screen
size, or window position:

- The app style forces `SH_Menu_Scrollable`, so a menu taller than the space below its
  anchor shows scroll arrows instead of overflowing off the screen.
- Each menu's maximum height is capped at the space below its anchor (the top-bar
  button, or the triggering item for a submenu). If the anchor sits near the bottom of
  the screen, the cap falls back to the screen height and placement is left to the
  compositor (which flips the popup above the anchor).
- Long labels (`Devices` entries, the USB descriptor summary) are elided to a
  screen-proportional width; the full text stays in the action tooltip.
- Window geometry and the vertical splitter position persist across runs via
  `QSettings` (`KeyboardInspector/KeyboardInspectorLinux`); skipped in
  screenshot/offscreen modes so renders stay deterministic. `_clamp_to_screen` still
  guards a restored window against a changed monitor arrangement.

## Permission / error banner

A 26-30 px strip directly below the top bar, hidden by default:

- Yellow/dark amber background, gray text, 1 px border.
- Examples:
  - “No readable keyboard devices. Add your user to the `input` group or install the
    targeted udev rule.” + `Copy command` + `Retry`.
  - “URX Core75 disconnected; releases synthesized at the last timestamp.”
  - “usbmon unavailable: mount debugfs / load usbmon; see Measurement limits.”
- Dismissible for transient errors; permission banner persists until devices are readable.
- Never auto-elevate.

## Main area: 3 × 2 chart grid

- Three equal columns, two equal rows, 3 px gaps.
- Column titles (upstream primary titles):
  1. `Differences between consecutive events`
  2. `Differences between all events`
  3. `Events wrapped around a second`
- Row subtitles appended in parentheses: `(time domain)`, `(frequency domain)`.
- Time charts: `#418CF0`. Frequency charts: `#F07800`.
- Chart `QWidget` painting:
  - Background `#070707` (upstream `BackColor.Blend(Black, 0.9)`).
  - Title centered at top in `#A0A0A0`, 9 px.
  - Six horizontal grid lines `#2C2C2C`; vertical x-grid `#141414`.
  - Line: 1.5 px antialiased, vertical gradient from line color to ~50% background.
  - Dense spans (samples closer than ~1 px) collapse into vertical min/max strokes,
    mirroring upstream `Chart.OnPaint` `useYLine`, so narrow spikes are never sampled away.
  - Dots appear only when x-spacing is wide enough (upstream threshold).
  - No y-axis numbers; x-axis adaptive labels. Time = ms, frequency = Hz.
  - Tick intervals follow upstream: time charts use `ScreenInterval` × 1000 ms
    (0.125, 0.25, 0.5, 1, 2, 4, 8, 20, 50, … ms); frequency charts use
    5, 10, 25, 75, 125, 250, 500, … Hz. Pick the smallest interval with ≥30 px
    (time) or ≥40 px (frequency) between labels, and extend ticks into the axis gutter.
  - Bottom scrollbar: track `#5C5C5C`, hover `#7A8084`, captured `#9EB1C3`, 17 px band.
    Thumb carries 6 px end-notches with 1.4 px background guide lines; notch drags resize
    the window from either edge through the upstream `Cramp` easing (s=0.0004,
    `Chart.cs:602-628`); track click jumps one page.
  - Hover: 6 px marker and a coordinate bubble placed with the upstream fallback rules;
    at more than three decimals the bubble splits into text + gray micro segment
    (`(12.345` `678` `, 42)`, `Chart.cs:1555-1617`).
  - Wheel zoom: `ZoomChange`, 1.05^scrolls around the cursor; min zoom = full view;
    max = upstream `XMax * 0.66` (line) / time*1000*0.66 (timeline).
  - Drag: pinned-cursor pan with direction lock — horizontal (≤30°) pans, vertical (≥60°)
  drag-zooms around the captured x (upstream `PanAction`); the OS cursor is re-pinned at
    the capture point while offsets accumulate, exactly like `CapturedCursorTick`.
  - Axis labels: shadow-stamped (`DrawShadowString`); time-domain charts use
    `XAxisMicroDecimals = 3` — values with more than three decimals render as a gray
    ".000" micro remainder in the grid-y color (`MainForm.cs:204`).
  - Double-click: spotlight — the chart is reparented above the grid into the top panel
    (`MainForm.cs:150-168`); double-click again restores its grid cell. Keep the other
    charts' data.
- Time charts initial view: 32 ms (upstream `ScopeDefaultX=32`), with full-range scrollbar.
- Frequency charts initial view: full processed spectrum. After HPS, x max =
  `(B/2)/(hps+1)` Hz.
- Circular time chart: rotate max bin to front (intended upstream behavior; display only).
- First time chart footer: `0 ms events: N (P%)` — upstream documented chord-split signal.
  Draw it bottom-left over the plot with a translucent dark backing so histogram bars do
  not make it unreadable.

## Analysis strip (25 px, directly under charts)

Exact left-to-right controls:

| Control | Width / position |
|---|---|
| `Binning Rate:` label | left, 12 px margin |
| Bin rate line edit | 56 px, right-aligned, digits only, 20..128000 |
| `Hz` label | |
| `/ 2` button | 29 px |
| `* 2` button | 29 px |
| `Low Cut Filter` checkbox | checked by default |
| `HPS Partial Elimination:` label | |
| HPS spin box | 71 px, 0..9, right-aligned |
| `partials` label | |
| Event count | right-aligned, `N input events`, gradient color |

Use a stretch spacer before the event count. Keep the strip exactly one row; at narrow
widths, hide the "partials" label first, never the bin rate or record controls.

Event count gradient (upstream): <=50 red `#EE0000`, 200 orange `#EA7700`,
600 yellow `#DDDD00`, >=1500 green `#00EE00`, interpolated.

Changes trigger reanalysis (debounced ~150 ms to avoid recomputing per keystroke).
Invalid values are clamped to the upstream range; show a subtle error border.

## Timeline (bottom pane)

- Header row inside the widget: title `Timeline` small gray, sample count right, `Fit`
  button. (Upstream has no explicit header; the event count lives in the analysis strip.
  Keep the header minimal and optional so it does not change the visual identity.)
- Rows sorted by source name then source id. If multiple sources, draw source names in a
  left gutter with a vertical separator; otherwise key labels only.
- Key labels right-aligned, 9 px, gray; hover highlights the row.
- Hold intervals: vertical gradient bars, per-input color; default upstream DarkGray
  `#A9A9A9`; bars extend to `duration` for keys held at stop.
- x-axis: adaptive seconds (`ScreenInterval`), labels in seconds with
  `XAxisMicroDecimals = 3` (gray ".000" remainders) and shadow stamping; bottom
  scrollbar; wheel zoom; drag pan; hover bubble with key, time, down/up.
- Row drag-reorder: press in the label gutter and drag (upstream `YTextIntent.Drag`);
  the row moves to the drop row (insert after target when moving down) and the new order
  persists into the saved recording via the edit sync.
- Event count colors as above.
- Right-click menu: `Change Color…`, `Reset Color`, `Hide Input`, `Hide Device`,
  `Sort by Device`, `Show All Inputs`, `Copy Interface`. Hiding re-runs analysis.
- Drag a `.kbi`/`.ttr`/`.ttrm` file onto the window to load it (upstream behavior).
- Double-click a row to spotlight? Not upstream; do not add.

## Typography and spacing

- Upstream font: Microsoft Sans Serif 8.25 pt (~11 px). Linux substitute:
  `Noto Sans` / `DejaVu Sans` at 9 pt; fallback to Qt's system sans.
- Chart titles 9 pt; top-bar text 9 pt; rate readout 11 pt semibold; timeline labels 8.5 pt.
- Use `QFontDatabase` to pick Noto Sans/DejaVu Sans; do not bundle fonts.
- Spacing: 4 px grid, 6 px row gaps, 3 px between charts, 12 px outer chart margins.
- Borders: 1 px `#2C2C2C`; no rounded cards.

## Color palette

| Token | Value |
|---|---|
| Window background | `#2B2B2B` |
| Top bar/menu | `#3C3F41` |
| Text primary | `#DCDCDC` |
| Text secondary | `#A0A0A0` |
| Chart background | `#070707` |
| Grid x / y | `#141414` / `#2C2C2C` |
| Time line | `#418CF0` |
| Frequency line | `#F07800` |
| Textbox background | `#45494A` |
| Scrollbar | `#5C5C5C` / hover `#7A8084` / captured `#9EB1C3` |
| Error/amber banner | background `#3A2F1B`, border `#8A6D2F`, text `#E8D8A8` |
| Recording accent | `#E05A4F` (button text/border only) |
| Event count gradient | `#EE0000` → `#EA7700` → `#DDDD00` → `#00EE00` |

Do not introduce a second accent color for general UI. Blue/orange are data-domain colors.

## Empty, loading, and error states

- Empty (no result): charts show their titles and an empty dark plot; centered hint:
  `Select a keyboard, press Start Recording, then type.` Timeline shows the same hint.
- Recording: charts can show the previous result dimmed or a “recording…” hint; do not
  draw incomplete FFTs as final. The tentative rate may show in the top bar.
- Analyzing: status text `Analyzing…`; keep previous charts until the new result arrives.
- Permission: banner + disabled Start when no selected device is readable.
- Insufficient data: `Insufficient data` in the rate area; charts still draw what exists.

## Qt mapping

- `QMainWindow` + central `QWidget` + `QVBoxLayout`.
- Custom top bar widget with `QToolButton` menus (not a native `QMenuBar`) to allow centered
  status/readout and right-aligned controls in one row.
- `QSplitter(Qt.Vertical)` for charts/analysis vs timeline.
- `QGridLayout` for the 3×2 charts with 3 px spacing and equal stretch.
- `QWidget` subclasses `ChartWidget` and `TimelineWidget` with `paintEvent`, wheel, mouse,
  context-menu, and double-click handlers.
- `QTimer` for capture draining and tentative analysis; `QtConcurrent` +
  `QFutureWatcher` for analysis.
- `QFileDialog` for open/save; `QNetworkAccessManager` (redirects disabled) for URL
  import and the update check; drag/drop via `dragEnterEvent`/`dropEvent`.
- `QSettings` for window geometry, selected devices, theme (dark only), and settings.
- Do not use a web view; do not bundle Chromium/Electron.

## Acceptance checks

1. At 1294×836 the window looks like the upstream layout: one compact top row, 3×2 charts,
   analysis strip, timeline.
2. At 720×500 nothing overlaps; the record button, Freeze, bin rate, and event count remain
   usable.
3. Blue time charts / orange frequency charts / gray timeline labels match the palette.
4. Wheel zoom, drag pan, hover marker, scrollbar, spotlight, timeline context menu, and
   `.kbi` drag/drop work.
5. The rate readout never shows a number without a backend label and confidence state.
6. The permission banner appears on this machine (user not in `input`) with actionable
   commands, and the app still launches.
