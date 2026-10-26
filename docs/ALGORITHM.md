# Analysis Algorithm — Port Specification

This file is the normative description of the Linux implementation. It ports
`Result/Analysis.cs` exactly except where marked **Linux addition** or **documented
divergence**. Every change here must have a test in `tests/test_core.cpp`.

## Inputs

- A finalized list of events, sorted by timestamp. Each event has:
  - `time`: seconds, monotonic, normalized so the first event (or recording start) is ~0.
  - `pressed`: bool.
  - `input_id`: `(source_id, key_code)`, only for grouping/visibility; the math uses time only.
- `bin_rate`: int, default 4000, valid range 20..128000.
- `hps`: int, default 0, range 0..9.
- `low_cut`: bool, default true.

Events are filtered before analysis exactly like upstream:

1. Drop kernel `EV_KEY` value `2` (auto-repeat).
2. Remove consecutive events for the same input with the same pressed state
   (`Result.cs:27-38`). This also catches duplicate press/release packets.
3. Apply input visibility. Analysis uses only events whose `(source,key)` is visible.

## 1. Three distributions

Let events be `e[0..n-1]`, `t[i] = e[i].time`.

### 1a. Consecutive differences (`diffs`)

```
diffs = [t[i] - t[i-1] for i in 1..n-1]
```

### 1b. All-pairs differences (`compound`)

Exact upstream loop, including the adjacent-pair exception:

```
compound = []
for i in 0..n-2:
    for j in i+1..n-1:
        diff = t[j] - t[i]
        if j > i+1 and diff >= 1.0: break
        compound.append(diff)
```

This is O(n × events_per_second) in practice. Use a two-pointer/inner loop that breaks
early; do not generate all O(n²) pairs. A manual recording is small; a pathological
recording must not freeze the UI (analysis runs off-thread).

### 1c. Circular timestamps (`circular`)

```
circular = [t[i] % 1.0 for i in 0..n-1]
```

Python's `%` is non-negative for non-negative timestamps. Timestamps are normalized to
non-negative before this step.

## 2. Binning

`B = bin_rate`; `bin_seconds = 1/B`.

```
counts = zeros(B)
for v in source:
    bin = round(v * B)          # midpoint-to-even, like .NET Math.Round
    if bin >= B:
        if modular: bin %= B
        else: continue           # intervals >= 1 s are ignored
    counts[bin] += 1
```

- `modular=True` only for the circular distribution.
- NumPy: `np.rint(v * B).astype(np.int64)` (half-to-even).
- Empty input → all-zero histogram.

## 3. Real FFT

- `spectrum = np.fft.rfft(counts)`; result length `B//2 + 1`.
- `magnitude[i] = hypot(spectrum[i].real, spectrum[i].imag)`.
- Frequency resolution is **1 Hz** because the histogram spans exactly 1 second.
- Before HPS, bin index `i` corresponds to `i` Hz.
- Upstream uses FFTW; NumPy's pocketfft produces the same mathematical result within
  floating-point tolerance. This is not a behavioral change.
- The port uses kissfft's `kiss_fftr` when vendored. Without it, a built-in fallback
  computes the same magnitude spectrum in O(n log n): an iterative radix-2 FFT for
  power-of-two `B`, and a Bluestein chirp-z transform for arbitrary `B` (chirp phases
  reduced `j² mod 2n` in exact 64-bit integer arithmetic; the (k−j)² kernel becomes a
  circular convolution evaluated with power-of-two FFTs). A startup warning is logged
  when kissfft is absent. Verified against a naive DFT and by the same frequency
  recovery tests (1000/500/4000 Hz) that run with kissfft.

## 4. Low cut

If `low_cut`:

```
for i in 0 .. min(70, len(magnitude))-1:
    magnitude[i] /= 1 + exp(-(i - 25) / 4.0)
```

This is a logistic attenuation, not a brick-wall filter. It suppresses the raised DC/low
floor around 0-45 Hz and is nearly 1.0 by i≈70. It operates on bin index, which equals Hz.

## 5. Reverse harmonic product spectrum (HPS)

Exact upstream variant:

```
if hps > 0:
    copy = magnitude.copy()
    for i in 0 .. len(magnitude)-1:
        for j in 0 .. hps-1:
            magnitude[i] *= copy[int(i * (j + 1) / (hps + 1))]
```

Then normalize:

```
magnitude /= max(1.0, magnitude.max())
x_factor = 1.0 / (hps + 1)
```

Interpretation: for HPS=2, a peak at index `i` is multiplied by values at `i/3` and
`2i/3`, and displayed at `i/3` Hz via `x_factor`. This recovers a fundamental from
higher harmonics. It is **not** the textbook `copy[2i] * copy[3i]`; do not "fix" it.

`int()` truncates toward zero (indices are non-negative). `hps=0` skips the product and
uses `x_factor=1`.

## 6. Time-domain display data

- Consecutive and compound time charts: `x = bin_index * (1000/B)` ms, y = counts.
- Circular time chart: upstream rotates the counts so the maximum is at index 0, then
  plots with the same x-factor. The upstream rotation loop checks every other bin; the
  port uses the intended "rotate max to front" behavior. **Documented display-only
  divergence**; FFT input is not rotated.
- Chart y maximum = `max(1, counts.max())`.

## 7. Frequency-domain display data

- x = `bin_index * (1/(hps+1))` Hz.
- y = normalized magnitude (0..1, unless the unnormalized max was <1; upstream leaves it).
- `low_cut` and `hps` can be re-applied from cached raw magnitudes without recomputing
  histograms/FFT.

## 8. Linux addition: polling-rate estimate

Upstream only shows spectra; the port must show a prominent rate. This section is the
**only algorithmic addition** and is explicitly labeled as such in the UI and README.

### Candidate extraction

For each of the three processed spectra:

1. Ignore bins below `max(50 Hz, low-cut region)` (default 50 Hz; use 50 even if low cut
   is off, because the DC/floor is not a report rate).
2. Find local maxima: `m[i] >= m[i-1] and m[i] >= m[i+1]` (or use `scipy.signal.find_peaks`
   if SciPy were a dependency; it is not, so implement in NumPy).
3. For each candidate, compute frequency `f = i / (hps+1)` for HPS>0 or `f = i` for HPS=0.
   Ignore `f < 20 Hz`.
4. Compute prominence proxy: `peak / max(median(m), 1e-12)` and ratio to the second-best
   candidate at least 5% away in frequency.
5. Reject candidates within 2 bins of the Nyquist end of the processed axis.

### Selection and confidence

Select the candidate with the highest prominence across the three spectra. If two
candidates from different spectra agree within 3%, treat them as one candidate and add
their evidence (this reduces the chance of picking a chart-specific artifact).

Candidates are sorted by `(prominence, ratio)` with a **stable** sort. An evenly spaced
event train (for example a 2 ms grid) gives equal-rank peaks at `f`, `2f`, `3f`, …;
stable ordering keeps the lowest frequency first so the readout reports the 500 Hz
fundamental rather than the 1000 Hz harmonic, matching the reference implementation.

### Fundamental selection (grid-coherence guard)

Host timing grids can inflate a harmonic above the true report cadence. Observed on
real hardware (2.4G Mouse, `1ea7:0064`): the device reports every 4th USB frame
(8 ms = **125 Hz** cadence), but evdev timestamps are SOF-locked to the 2 ms frame
sub-grid, so *every* timestamp is 500 Hz-coherent and the circular/all-pairs spectra
grow a taller 500 Hz peak than the 125 Hz fundamental. Two guards run after the
prominence ranking, before phase verification:

1. **Harmonic-family downgrade.** If the top candidate is an integer multiple `k`
   (2–16) of another candidate within 2.5% whose prominence is at least **half** of
   the top's, the slower rate is the report cadence. Real-recording separation: the
   frame-grid sub-harmonic reaches ~0.88 of the top prominence, unrelated sidebands
   stay below ~0.3 (a 1000 Hz keyboard's 495 Hz sideband is 0.29). One downgrade step;
   no cascades.
2. **Modal-gap reconciliation.** The consecutive-delta histogram measures actual
   report-to-report gaps and is immune to grid coherence. When the spectral winner is
   an integer multiple `m` (2–16, 2% tolerance) of the rate implied by the *modal*
   consecutive gap (modal bin ± 1, center-of-mass, bin share ≥ 0.2), report the gap
   cadence instead. This catches cleaner data where the grid harmonics tower over the
   cadence harmonics (continuous motion, little jitter).

After either guard fires, phase verification runs with the multipliers capped at
1.5× the corrected rate so a stronger grid harmonic cannot pull the result back up.
Both guards append an explanatory warning to the estimate, and both leave clean data
untouched (a true 500 Hz device on the same frame grid still reports 500 Hz).

Require:
- at least 20 analyzed events, and
- at least 0.5 s between the first and last event, and
- peak prominence ≥ 2.5.

### Phase verification (raw timestamps)

The spectra alone are ambiguous for sparse typing and for rates near the bin-rate limit.
After selecting a candidate `f`, the port checks the raw event timestamps against a
periodic grid:

1. Compute phase coherence `R(f) = |mean(exp(2πi·f·t))|` on the relative timestamps.
2. Refine `f` within ±2% by maximizing `R` (step ≈ 0.25/duration, floored at 0.01 Hz).
3. Check `f/2..f/4` and `2f..4f` the same way. A factor is adopted only if its coherence
   exceeds the current value by at least 0.12 and is at least 0.35. This corrects harmonic
   overestimates and aliased/low-bin-rate halving (e.g. a 1000 Hz grid misread as ~495 Hz).
4. If `R(f) < 0.35` and the recording has at most 2000 events, run a coarse 20–20000 Hz
   phase scan and refine its best frequency. This recovers grids that the histograms
   cannot resolve; it is skipped during tentative analyses to keep recording responsive.

`R ≥ 0.35` means the timestamps confirm a stable grid. `R` is exposed in the estimate
tooltip. Phase verification does not alter the upstream histogram/FFT charts; it only
selects/refines the added rate readout, so charts still need a bin rate above twice the
measured rate to show the peak.

Confidence labels:
- `strong`: phase coherence ≥ 0.55 and events ≥ 60.
- `moderate`: phase coherence ≥ 0.35.
- `weak`: grid not verified, but the spectrum is very strong (prominence ≥ 8) and events ≥ 200.
- `insufficient`: requirements not met, or no stable grid was confirmed. No number is shown.

Additional warnings (shown with the estimate, not hidden):
- Raw timestamps did not confirm a grid → “treat this rate as a hint”.
- `peak >= 0.4 * bin_rate` → “near Nyquist; increase binning rate”.
- `hps > 0` is active → the displayed frequency is after harmonic scaling; mention HPS.
- `events < 100` → small sample.
- Backend is `usbmon` → this is HID-report timing, not evdev event timing.
- The configured descriptor interval is always displayed separately and never substituted.

While recording, run this on all events so far at most every ~750 ms and label the result
**tentative**. On stop, run the final analysis and label it final. Do not show a live
number before the minimum data requirements are met.

### Why this is not an upstream value

Upstream's `EstimatePeak` is unused and quirky; upstream expects a human to read the
largest FFT peak. The port's estimate is deterministic, confidence-gated, and documented.
It must never be described as “the” hardware polling rate without the caveats in
`MEASUREMENT_LIMITS.md`.

## 9. Derived diagnostics shown in the UI

- **0 ms events**: count and percentage of consecutive diffs with `diff == 0` (within
  floating tolerance). Upstream docs identify a 0 ms peak as evidence of simultaneous
  reporting (no chord splitting). Show it next to the first time chart.
- **Event count** and **recording duration** (upstream shows the count; duration is needed
  for context).
- **Configured USB interval** from sysfs, labeled separately (e.g., “descriptor 1 ms =
  1000 Hz max, configured”).
- Do not invent average hold time, jitter, or other stats unless a future spec adds them
  and they are clearly labeled as additions.

## 10. Required synthetic tests

| # | Input | Assertion |
|---|-------|-----------|
| A1 | Alternating two keys every 1.000 ms | consecutive histogram peak at bin 1 ms; FFT peak 1000 Hz |
| A2 | 500 Hz and 4000 Hz sequences | recover 500/4000 Hz at adequate bin rates |
| A3 | Two keys with identical timestamps | 0 ms count = 1; chord indicator ≥ 1 |
| A4 | Timestamps 1.1, 2.1, 3.1… | circular peak at 0.1 s |
| A5 | Press/press duplicate + release/release | dedupe removes the duplicate states |
| A6 | Sparse/noisy events | `insufficient` estimate, no fabricated Hz |
| A7 | HPS=2 on a spectrum with harmonics | exact upstream product and `x_factor=1/3` |
| A8 | Low cut off/on | values below 70 Hz attenuated only when on |
| A9 | Long gap > 1 s | non-modular bins drop it; compound loop breaks |
| A10 | Multiple sources with one at >80% | visibility rule hides the others |
| A11 | Bin rate edge values (19 invalid, 20 valid, 128000 cap) | validation matches upstream |
| A12 | Circular rotation | max becomes first element; FFT unchanged |

Additional port tests: `.kbi` round-trip, key-name mapping, USB sysfs parsing, capture
repeat filtering, device disconnection handling.
