# Keyboard Inspector for Linux

A native Linux port of [Keyboard Inspector](https://github.com/mat1jaczyyy/Keyboard-Inspector)
(Windows, C#/.NET): record keyboard and input events, build the same three timing
histograms, run the same FFT / low-cut / HPS pipeline, and read a confidence-gated
report-rate estimate from the same 3×2 chart grid and input timeline.

The Windows application is the normative reference for behavior, math, layout, colors,
and the `.kbi` file format. This port reproduces it with Qt 6 on Linux and adds
Linux-specific capture through the kernel's evdev interface.

```
┌ File  Help ────────── rate readout ────────── Capture ▾  □ Freeze  [Start Recording] ┐
│  ┌ consecutive · time ┐ ┌ all pairs · time ┐ ┌ wrapped · time ┐   (blue #418CF0)     │
│  ├ consecutive · freq ┤ ├ all pairs · freq ┤ ├ wrapped · freq ┤   (orange #F07800)  │
│  bin rate · low cut · HPS partials · event count       (25 px analysis strip)         │
│  input timeline: rows, hold bars, sources                                             │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

## What it does

- **Capture** — unprivileged evdev readers on `/dev/input/event*` (libevdev API). Kernel
  event timestamps are used as-is; the app never calls `EVIOCGRAB`, so other applications
  keep receiving input. Devices without permission are listed with a lock state and a
  permission banner; the app never requests root.
- **Analysis** — a direct C++ port of upstream `Result/Analysis.cs`: three timing
  distributions (consecutive deltas, all pairs, timestamps modulo 1 s), real FFT, logistic
  low-cut, upstream **reverse** harmonic product spectrum, and a confidence-gated rate
  estimate. FFT runs off the GUI thread; results are applied by generation token.
- **Rate readout** — the estimated rate appears in the top bar only when sample size and
  peak prominence suffice; the source ("estimated · evdev timing") and caveats are shown
  next to it. It is **never** conflated with the USB descriptor polling interval, which is
  shown separately as a *configured* capability in Capture → USB descriptor.
- **Files** — opens and saves `.kbi` recordings (v3 write; v0–v3 read, including legacy
  typed-id files), and imports TETR.IO replays (`.ttr`/`.ttrm`) with the upstream round
  picker and disclaimer. `File → Import from URL…` downloads a direct `.kbi` link or a
  `tetr.io` replay link (redirects disabled, status codes surfaced verbatim).
- **UI** — upstream layout and identity: dark studio utility, 3×2 charts with 3 px gaps,
  wheel zoom, pinned-cursor pan with direction lock (horizontal pan / vertical drag-zoom),
  scrollbar end-notches with Cramp easing, hover coordinate bubbles with micro-decimal
  splitting, double-click spotlight into the top panel, timeline with row drag-reorder,
  context menu, and drag-and-drop file open.

## Building

Arch Linux (tested): Qt 6 (Core, Widgets, Gui, Concurrent, Network), libevdev,
CMake ≥ 3.24, and a C++20 compiler. `kissfft` is optional — without it the FFT falls
back to a built-in radix-2/Bluestein transform (O(n log n), still fast on long
recordings; the app logs a warning at startup).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/keyboard-inspector
```

Tests (Catch2; the core suite and a UI smoke suite):

```bash
cmake --build build -j$(nproc)
./build/bin/keyboard-inspector-tests        # core: model, analysis, kbi, keynames, capture, usb
./build/bin/keyboard-inspector-ui-tests     # offscreen UI smoke tests
```

Visual verification from an offscreen platform:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/keyboard-inspector --screenshot /tmp/ki.png path/to/recording.kbi
QT_QPA_PLATFORM=wayland   ./build/bin/keyboard-inspector --screenshot /tmp/ki.png path/to/recording.kbi
```

## Running

```bash
keyboard-inspector [file.kbi | file.ttr | file.ttrm]
keyboard-inspector --list-devices          # show devices, access state, USB descriptors
keyboard-inspector --open path/to/file.kbi
keyboard-inspector --debug                 # verbose console log
keyboard-inspector --help                  # all options
```

Logs go to `~/.local/state/keyboard-inspector/keyboard-inspector.log` by default
(`--log-file` to override, `--quiet` to silence the console stream).

## Device access

`/dev/input/event*` is `root:input 0660` by default. The application must run
unprivileged; choose one of:

- **Targeted udev rule (recommended)** — `packaging/install-udev-rule.sh` installs
  `71-keyboard-inspector.rules`, granting the logged-in seat user an ACL only on the
  matching keyboard's event nodes (`uaccess`). The rule must sort before
  `73-seat-late.rules`; a re-plug (or `udevadm trigger`) is needed after install.
- **`input` group (broad)** — `usermod -aG input $USER` grants read access to every input
  device. Simpler but wider than needed.

Never run the GUI as root. Devices that cannot be opened are still listed, marked
locked, and explained in the permission banner.

## USB polling interval — what is measured and what is not

The Capture menu can show the device's USB descriptor data (vendor/product, speed,
interrupt-IN endpoint, `bInterval`, `wMaxPacketSize`). For the URX75 Core75 this reads
full-speed USB, HID boot keyboard, endpoint `0x81`, `bInterval=1` ms, 8-byte reports —
a **configured maximum of 1000 Hz, not a measurement**. The measured rate in the top bar
comes only from captured event timing. See `docs/MEASUREMENT_LIMITS.md`.

## Package

`packaging/` contains a PKGBUILD (Arch), a `.desktop` entry, an icon, and the udev rule
with an installer script. Build the package from an isolated copy of the whole port:

```bash
rm -rf /tmp/ki-pkg && cp -r . /tmp/ki-pkg && rm -rf /tmp/ki-pkg/build
cd /tmp/ki-pkg/packaging && makepkg -f --nodeps
```

## License

GPL-3.0-or-later for this port (Qt 6 bindings are GPL-3.0 or commercial). The upstream
Keyboard Inspector is BSD-3-Clause; its copyright notice is preserved in `NOTICE`.
See `docs/DECISIONS.md`.
