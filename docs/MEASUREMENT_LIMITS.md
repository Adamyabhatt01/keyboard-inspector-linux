# Measurement Accuracy and Limitations

This is the document that prevents the most common false claim: “the keyboard polls at
1000 Hz because the descriptor says 1 ms.” The application must keep three layers separate.

## Layer 1 — Configured USB descriptor interval (capability, not measurement)

Read from sysfs for the selected device/interface:

- Device speed (`/sys/bus/usb/devices/X-Y/speed`).
- HID interface class/subclass/protocol.
- Interrupt IN endpoint: `type`, `interval`, `wMaxPacketSize`.
- USB vendor/product (`idVendor`, `idProduct`), manufacturer/product strings.

For the connected URX Core75 (2.4 GHz dongle):

| Field | Value |
|---|---|
| USB ID | `36b0:3002` |
| Manufacturer / product | `RDMCTMZT` / `URX Core75 Wireless` |
| Speed | 12 Mbps (full speed) |
| Interface 0 | HID, boot subclass, keyboard protocol |
| Interrupt IN | `0x81`, `interval=1 ms`, `wMaxPacketSize=8` |
| Derived configured maximum | 1000 Hz (1 ms) |

This means “the device is configured to be polled at most every 1 ms.” It does **not**
prove the keyboard’s matrix scan rate, the wireless link cadence, or that the device
actually delivers a report every millisecond. The UI labels it “configured interval” and
never feeds it into the measured-rate readout.

High-speed devices use microframes (125 µs); full/low-speed use 1 ms frames. If the
descriptor is read on a hub or composite interface, make sure the endpoint belongs to the
keyboard HID interface, not a mouse/consumer interface. Display the interface path/endpoint
alongside the value.

## Layer 2 — USB HID report completion timing (`usbmon`, optional)

`usbmon` exposes URB submissions/callbacks at the host-controller level. The optional
backend timestamps **data-bearing interrupt-IN completions** for the selected device.

What it can show:
- Actual report cadence during periods of changing input.
- Batching/coalescing at the USB/HCD layer.
- Differences between wired and wireless links.

Limitations:
- An idle HID interrupt endpoint is typically polled but NAKed internally; `usbmon` does
  not expose every NAK. Only successful data transfers appear, so you cannot measure the
  idle poll rate from silence.
- The kworker/interrupt scheduling, host controller, hub, and kernel can add jitter.
- Composite devices may have several interrupt endpoints; select the keyboard interface.
- Requires debugfs/`usbmon` access (normally root or `CAP_NET_ADMIN`). The GUI must not run
  as root. A privileged usbmon layer is not implemented; if it ever is, it must use a
  separate clearly-labeled diagnostic path.
- Boot-protocol HID reports (8 bytes) are decoded into key usages. Devices with a custom
  report descriptor are rejected with guidance rather than guessed at.

Label every result from this backend as **USB HID report timing**, never as evdev timing
or descriptor data.

## Layer 3 — Linux evdev event timing (default)

libevdev reads `/dev/input/eventN`; the kernel assigns each `input_event` its timestamp
when the input subsystem processes the device. This is the default backend.

What it includes:
- Keyboard matrix scan and firmware debounce.
- HID report transmission (USB/wireless link).
- Kernel HID parsing, input-core handling, and timestamping.
- Device-specific quirks (e.g., firmware that splits chords into separate reports).

What it excludes:
- Qt/compositor/app scheduling (the timestamp is from the kernel, not when the reader thread dequeues it).
- USB host-controller-level retries and NAK behavior (not visible without usbmon).
- Windows-specific Raw Input/app timestamp delays.

Limitations:
- It cannot uniquely separate matrix scan rate from USB report rate; the FFT peaks are the
  combined result. A 1000 Hz peak can be the USB interval, the scan rate, or both.
- Multiple keys in the same report get identical timestamps → the 0 ms bin grows. A lack of
  0 ms values during deliberate simultaneous presses suggests chord splitting/separate
  reports (upstream's documented test).
- Wireless (2.4 GHz/Bluetooth) adds link buffering, retries, and jitter; wired USB is the
  cleaner measurement path.
- Kernel timestamps are only as good as the device’s report cadence; sparse typing gives a
  sparse histogram. Use deliberate alternating key presses for 20-30 s.
- The binning rate must be high enough: to analyze 1000 Hz use 4000 Hz (default); for 8000 Hz
  use >= 16000 Hz and expect more noise. The FFT x-axis resolution remains 1 Hz.
- Low-cut/HPS processing is upstream’s; HPS can move the displayed peak to a
  fundamental/harmonic. Always read the chart and the estimate caveat together.

### Mouse, touchpad and gamepad measurement (movement inputs)

Upstream records **buttons only** for mouse and gamepad (movement is dropped on
Windows), so a button-only recording cannot show a mouse polling rate. The Linux port
adds one synthetic `Movement` input per SYN-delimited report that carries at least one
`EV_REL`/`EV_ABS` axis event (`EvdevReportTracker`), so the standard histogram/FFT
pipeline measures the **report cadence while the device is actively generating motion**:

- Counting is **per report**, not per axis event: a report carrying dx and dy counts
  once. (The evhz tools count per axis event, which overcounts diagonal motion.)
- An idle mouse emits nothing, so like every evdev measurement this only covers periods
  of real activity; keep the mouse moving continuously for the session.
- Touchpads are classified as mice; their movement includes finger tracking reports.
- Gamepads: buttons and the hat switch (`HatN/E/S/W`, upstream `GamepadKeys` 32–35
  parity) are recorded as button inputs; stick axes appear as movement events only
  while the stick is actually deflected (a centered stick emits no reports).
- The measured rate is the combined scan+report+kernel pipeline at the evdev layer, the
  same limits as keyboard timing above. Cross-check against the descriptor interval
  (e.g. a `2.4G Mouse` at `1ea7:0064` is configured for 2 ms → 500 Hz max) but never
  present the descriptor value as the measured one.
- **Frame-grid coherence**: evdev timestamps of USB HID devices are SOF-locked to the
  1 ms frame clock, so timestamps of a slower device still lie on a fast grid. A device
  reporting every 4th frame grows 500/1000 Hz spectral peaks that are *not* its report
  cadence. The estimate reconciles this (fundamental selection, see `ALGORITHM.md`
  §8), and the consecutive-delta histogram always shows the true inter-report gap.
- Measured example (2026-10-26, live session, this machine): the `2.4G Mouse` reported
  with 8 ms cadence — **≈125 Hz measured** while its descriptor advertises a 500 Hz
  configured maximum. Descriptor ≠ measurement, in the opposite direction from the
  common assumption.

## Windows comparison

- Upstream timestamps at `WM_INPUT` receipt using `Stopwatch` on the UI thread and raises
  the process/thread priority (`Program.cs:48-49`, `ListenerWindow.cs:48-50`). It includes
  Windows scheduler/compositor delays and is sensitive to CPU load.
- Linux evdev timestamps earlier, in the kernel, and does not need real-time priority. This
  is usually more consistent but represents a different point in the pipeline.
- Neither is a USB analyzer. Cross-platform comparisons should use the same keyboard and a
  similar deliberate input pattern, and should report the timestamp layer alongside the
  number.

## Confidence and “insufficient data”

The port’s estimate is gated (see `ALGORITHM.md` §8):
- <20 events or <0.5 s → insufficient; show no number.
- Weak prominence → label weak, not a definitive polling rate.
- Near-Nyquist peak → warn and recommend a higher bin rate.
- HPS active → mention that the displayed frequency is after harmonic scaling.
- USB backend → label USB report timing, not evdev event timing.
- Descriptor interval is always separate and explicitly “configured”.

These rules exist because a visually tall FFT peak can be a harmonic, a matrix-scan
artifact, a wireless retry cadence, or noise. The app must not collapse that ambiguity into
a single confident number.

## URX75 test procedure

1. Prefer wired USB for timing; repeat over 2.4 GHz and Bluetooth for comparison.
2. Close CPU-heavy apps; the Linux capture thread is lightweight but the device/kernel
   still share the machine.
3. Select only the URX75 keyboard interface(s). If multiple HID interfaces appear, choose
   the one that actually emits letter keys; document duplicates.
4. Start recording. Alternate two keys (e.g., A/D) at a steady, comfortable rate for
   ~20-30 s. Then deliberately press 3-6 keys simultaneously several times to test the
   0 ms/chord-split behavior.
5. Stop. Read:
   - event count and duration,
   - 0 ms count/percentage,
   - estimated rate + confidence + backend label,
   - all three time histograms and all three spectra; the first frequency chart is the
     most direct.
6. Repeat 3 times in the same mode; do not treat one noisy run as definitive.
7. Compare:
   - descriptor: 1 ms configured (1000 Hz max, full speed);
   - measured evdev peak: what the combined scan+report pipeline actually shows;
   - optional usbmon: data-report cadence, if privileges are available.
8. Record the host, port/hub, cable, wireless mode, and app version with the result.

### First measured run (2026-10-25)

```
Device: URX Core75 (36b0:3002), wireless receiver on USB bus 3
Capture: Linux evdev event timing, /dev/input/event7 (report protocol), defaults
Sample: 196 transitions over 16.58 s (30 s capture window)
0 ms consecutive differences: 0 (0.0%)
Estimated report rate: 1000 Hz (strong confidence, phase coherence 0.92)
USB descriptor: interrupt IN 0x84, bInterval 1 ms (configured 1000 Hz max)
Notes: one run; typing was not a steady two-key alternation. The boot-protocol
       node /dev/input/event3 produced no events. No usbmon measurement.
Saved: /home/adamya/projects/urx75-recordings/urx75-live.kbi
```

This is an evdev-layer result only. It does not establish USB transfer timing, and it is
not the descriptor value. Repeat the procedure for a stronger result.

### Long-session validation run (2026-10-26)

```
Device: RDMCTMZT URX Core75 Wireless Keyboard, /dev/input/event27
Capture: Linux evdev event timing, defaults (bin rate 4000 Hz, low cut on, HPS 0)
Sample: 890 transitions over 19.852 s (live typing session)
0 ms consecutive differences: 0 (0.0%)
Estimated report rate: 999.985 Hz (strong confidence, phase coherence 0.963,
                       prominence 193.7; peak present in the consecutive, all-pairs,
                       and circular spectra)
USB descriptor: interrupt IN 0x81, bInterval 1 ms (configured 1000 Hz max) — not the
                measured value
Saved: /home/adamya/projects/Keyboard-Inspector/linux/recording.kbi (.kbi v3)
```

The saved file was re-analyzed offline with the port's own pipeline
(`read_kbi_file` + `analyze`); the estimate reproduced independently of the live UI.
Together with the first run (2026-10-25, 1000 Hz strong) this closes the evdev-layer
hardware validation for the URX75: the measured ≈1000 Hz matches the descriptor's
configured 1 ms interval, while remaining a separate measurement layer.

## Reporting template

```
Device: URX Core75 (36b0:3002), 2.4 GHz dongle / wired / BT
Capture: Linux evdev event timing, bin rate 4000 Hz, low cut on, HPS 0
Sample: 842 events over 27.4 s
0 ms consecutive differences: 37 (4.4%)
Estimated report rate: 1000 Hz (strong confidence), warnings: none
USB descriptor: interrupt IN 0x81, interval 1 ms (configured 1000 Hz max)
Notes: host port, hub, load, repeat count
```

Never report the descriptor line as the measured result. Never compare two devices using
different capture layers without saying so.
