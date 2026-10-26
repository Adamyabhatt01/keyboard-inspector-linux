#include "analysis.h"
#include "logger.h"

#include <complex>
#include <cstring>
#include <numeric>
#include <tuple>
#include <iostream>

#ifdef USE_KISSFFT
#include "kiss_fft.h"
#include "kiss_fftr.h"
#endif

namespace keyboard_inspector {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

} // namespace

// In-place iterative radix-2 FFT (decimation in time) for power-of-two
// sizes; inverse applies the conjugate sign and 1/n scaling.
void fft_radix2_inplace(std::vector<std::complex<double>>& data, bool inverse) {
    const size_t n = data.size();
    if (n < 2) return;

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    const double sign = inverse ? 1.0 : -1.0;
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = sign * kTwoPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (size_t base = 0; base < n; base += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = data[base + k];
                const std::complex<double> v = data[base + k + len / 2] * w;
                data[base + k] = u + v;
                data[base + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (auto& value : data) value *= scale;
    }
}

// Chirp-z (Bluestein) DFT for arbitrary n, built from power-of-two FFTs.
// Uses jk = (j² + k² − (k−j)²)/2 so e^(−2πijk/n) factors into three chirps;
// the (k−j)² term becomes a circular convolution computed with the radix-2
// FFT above. j² mod n is exact in 64-bit integers, keeping the angles
// accurate for the largest supported bin rates.
void dft_bluestein_inplace(std::vector<std::complex<double>>& x) {
    const size_t n = x.size();
    if (n < 2) return;
    size_t m = 1;
    while (m < 2 * n - 1) m <<= 1;

    std::vector<std::complex<double>> chirp_down(n), chirp_up(n);
    for (size_t j = 0; j < n; ++j) {
        // e^(±πi j²/n) has period 2n in j², so reduce modulo 2n (j < 2^32,
        // so j² and the reduction are exact in 64-bit integers).
        const unsigned long long square =
            (static_cast<unsigned long long>(j) * j) % (2 * n);
        const double angle =
            kTwoPi * 0.5 * static_cast<double>(square) / static_cast<double>(n);
        chirp_down[j] = std::complex<double>(std::cos(angle), -std::sin(angle));
        chirp_up[j] = std::complex<double>(std::cos(angle), std::sin(angle));
    }

    std::vector<std::complex<double>> a(m, std::complex<double>(0.0, 0.0));
    std::vector<std::complex<double>> b(m, std::complex<double>(0.0, 0.0));
    for (size_t j = 0; j < n; ++j) a[j] = x[j] * chirp_down[j];
    for (size_t j = 0; j < n; ++j) b[j] = chirp_up[j];
    // The kernel is evaluated at (k − j), which wraps around for k < j.
    for (size_t j = 1; j < n; ++j) b[m - j] = b[j];

    fft_radix2_inplace(a, false);
    fft_radix2_inplace(b, false);
    for (size_t i = 0; i < m; ++i) a[i] *= b[i];
    fft_radix2_inplace(a, true);

    for (size_t k = 0; k < n; ++k) x[k] = a[k] * chirp_down[k];
}

// Real-input magnitude spectrum without kissfft: radix-2 when the length is
// a power of two, Bluestein otherwise. O(n log n) in both cases.
std::vector<double> real_fft_magnitude_fallback(const std::vector<double>& input) {
    const size_t n = input.size();
    std::vector<double> magnitude(n / 2 + 1, 0.0);
    if (n < 2) return magnitude;

    std::vector<std::complex<double>> spectrum(n);
    for (size_t i = 0; i < n; ++i) spectrum[i] = {input[i], 0.0};

    if ((n & (n - 1)) == 0) {
        fft_radix2_inplace(spectrum, false);
    } else {
        dft_bluestein_inplace(spectrum);
    }

    for (size_t i = 0; i <= n / 2; ++i) magnitude[i] = std::abs(spectrum[i]);
    return magnitude;
}

namespace {

// Simple real FFT using kissfft when available, otherwise the built-in
// radix-2 / Bluestein FFT.
std::vector<double> real_fft_magnitude(const std::vector<double>& input) {
    size_t n = input.size();
    std::vector<double> magnitude(n / 2 + 1, 0.0);

#ifdef USE_KISSFFT
    kiss_fftr_cfg cfg = kiss_fftr_alloc(static_cast<int>(n), 0, nullptr, nullptr);
    if (!cfg) {
        // Fall through to the built-in FFT below.
    } else {
        std::vector<kiss_fft_cpx> out(n / 2 + 1);
        kiss_fftr(cfg, input.data(), out.data());
        for (size_t i = 0; i <= n / 2; ++i) {
            magnitude[i] = std::hypot(out[i].r, out[i].i);
        }
        free(cfg);
        return magnitude;
    }
#endif

    return real_fft_magnitude_fallback(input);
}

} // namespace

std::vector<double> bin_values(const std::vector<double>& values, int bin_rate, bool modular) {
    std::vector<double> counts(bin_rate, 0.0);
    if (values.empty()) return counts;

    std::vector<int64_t> indices;
    indices.reserve(values.size());
    for (double v : values) {
        int64_t idx = static_cast<int64_t>(std::llround(v * bin_rate));
        if (modular) {
            idx = ((idx % bin_rate) + bin_rate) % bin_rate;
            indices.push_back(idx);
        } else {
            if (idx >= 0 && idx < bin_rate) {
                indices.push_back(idx);
            }
        }
    }

    if (indices.empty()) return counts;

    for (int64_t idx : indices) {
        counts[idx] += 1.0;
    }
    return counts;
}

std::vector<double> compound_diffs(const std::vector<double>& times) {
    std::vector<double> t = times;
    std::sort(t.begin(), t.end());
    size_t n = t.size();
    if (n < 2) return {};

    std::vector<double> ends(n);
    for (size_t i = 0; i < n; ++i) {
        double target = t[i] + 1.0;
        size_t lo = i + 1, hi = n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t[mid] < target) lo = mid + 1; else hi = mid;
        }
        ends[i] = static_cast<double>(lo);
    }

    std::vector<double> result;
    for (size_t i = 0; i < n - 1; ++i) {
        size_t end = static_cast<size_t>(ends[i]);
        if (end <= i + 1) end = i + 2;
        end = std::min(end, n);
        for (size_t j = i + 1; j < end; ++j) {
            result.push_back(t[j] - t[i]);
        }
    }
    return result;
}

std::vector<double> compound_counts(const std::vector<double>& times, int bin_rate) {
    std::vector<double> t = times;
    std::sort(t.begin(), t.end());
    std::vector<double> counts(bin_rate, 0.0);
    size_t n = t.size();
    if (n < 2) return counts;

    std::vector<size_t> ends(n);
    for (size_t i = 0; i < n; ++i) {
        double target = t[i] + 1.0;
        size_t lo = i + 1, hi = n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t[mid] < target) lo = mid + 1; else hi = mid;
        }
        ends[i] = lo;
    }

    for (size_t i = 0; i < n - 1; ++i) {
        size_t end = ends[i];
        if (end <= i + 1) end = i + 2;
        end = std::min(end, n);
        for (size_t j = i + 1; j < end; ++j) {
            double diff = t[j] - t[i];
            int64_t idx = static_cast<int64_t>(std::llround(diff * bin_rate));
            if (idx >= 0 && idx < bin_rate) {
                counts[idx] += 1.0;
            }
        }
    }
    return counts;
}

std::vector<double> rotate_to_peak(const std::vector<double>& counts) {
    if (counts.empty()) return counts;
    size_t pivot = 0;
    double max_val = counts[0];
    for (size_t i = 1; i < counts.size(); ++i) {
        if (counts[i] > max_val) {
            max_val = counts[i];
            pivot = i;
        }
    }
    std::vector<double> result;
    result.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); ++i) {
        result.push_back(counts[(pivot + i) % counts.size()]);
    }
    return result;
}

std::vector<double> magnitude_spectrum(const std::vector<double>& counts) {
    return real_fft_magnitude(counts);
}

std::vector<double> apply_low_cut(const std::vector<double>& data) {
    std::vector<double> out = data;
    size_t limit = std::min<size_t>(70, out.size());
    if (limit == 0) return out;
    for (size_t i = 0; i < limit; ++i) {
        double exponent = -(static_cast<double>(i) - 25.0) / 4.0;
        out[i] /= (1.0 + std::exp(exponent));
    }
    return out;
}

std::vector<double> harmonic_product(const std::vector<double>& data, int hps) {
    std::vector<double> out = data;
    if (hps <= 0) return out;
    std::vector<double> copy = out;
    for (int j = 0; j < hps; ++j) {
        for (size_t i = 0; i < out.size(); ++i) {
            size_t factor_idx = (i * (j + 1)) / (hps + 1);
            out[i] *= copy[factor_idx];
        }
    }
    return out;
}

std::vector<double> process_spectrum(const std::vector<double>& counts, const AnalysisSettings& settings) {
    std::vector<double> data = magnitude_spectrum(counts);
    if (settings.low_cut) {
        data = apply_low_cut(data);
    }
    if (settings.hps > 0) {
        data = harmonic_product(data, settings.hps);
    }
    double maximum = 1.0;
    for (double v : data) {
        if (v > maximum) maximum = v;
    }
    for (double& v : data) {
        v /= maximum;
    }
    return data;
}

ChartData chart_time(const std::vector<double>& counts, int bin_rate) {
    ChartData cd;
    cd.values = counts;
    cd.x_factor = 1000.0 / bin_rate;
    cd.y_max = 1.0;
    for (double v : counts) {
        if (v > cd.y_max) cd.y_max = v;
    }
    cd.unit = "ms";
    cd.is_frequency = false;
    return cd;
}

ChartData chart_frequency(const std::vector<double>& processed, int hps) {
    ChartData cd;
    cd.values = processed;
    cd.x_factor = 1.0 / (hps + 1);
    cd.y_max = 1.0;
    cd.unit = "Hz";
    cd.is_frequency = true;
    return cd;
}

// ---- Rate estimation internals ----

struct Candidate {
    double freq = 0.0;
    double peak = 0.0;
    double prominence = 0.0;
    double ratio = 0.0;
    std::string chart;
    int evidence = 1;
};

static double phase_strength(const std::vector<double>& times, double hz) {
    if (hz < 20.0 || times.size() < 2) return 0.0;
    double sum_re = 0.0, sum_im = 0.0;
    const double two_pi = 6.2831853071795864769;
    for (double t : times) {
        double angle = two_pi * hz * t;
        sum_re += std::cos(angle);
        sum_im += std::sin(angle);
    }
    double n = static_cast<double>(times.size());
    return std::hypot(sum_re / n, sum_im / n);
}

static std::pair<double,double> phase_search(
    const std::vector<double>& times, double approximate, double duration)
{
    if (approximate < 20.0 || !std::isfinite(approximate)) return {0.0, 0.0};
    double span = 0.02 * approximate;
    double step = std::max(0.01, 0.25 / std::max(duration, 0.5));
    int count = static_cast<int>(2.0 * span / step) + 1;
    if (count > 200000) {
        step = 2.0 * span / 200000.0;
        count = 200001;
    }
    double lo = std::max(20.0, approximate - span);
    size_t chunk = std::max<size_t>(1, 1000000 / std::max<size_t>(1, times.size()));
    double best_hz = approximate, best_strength = 0.0;
    for (int start = 0; start < count; start += static_cast<int>(chunk)) {
        int end = std::min(start + static_cast<int>(chunk), count);
        for (int s = start; s < end; ++s) {
            double freq = lo + step * s;
            double strength = phase_strength(times, freq);
            if (strength > best_strength) {
                best_strength = strength;
                best_hz = freq;
            }
        }
    }
    return {best_hz, best_strength};
}

static std::pair<double,double> phase_discover(const std::vector<double>& times, double duration) {
    if (times.size() < 20 || times.size() > 2000) return {0.0, 0.0};
    double step = 1.0;
    double lo = 20.0, hi = 20000.0;
    int count = static_cast<int>((hi - lo) / step) + 1;
    size_t chunk = std::max<size_t>(1, 1000000 / std::max<size_t>(1, times.size()));
    double best_hz = 0.0, best_strength = 0.0;
    for (int start = 0; start < count; start += static_cast<int>(chunk)) {
        int end = std::min(start + static_cast<int>(chunk), count);
        for (int s = start; s < end; ++s) {
            double freq = lo + step * s;
            double strength = phase_strength(times, freq);
            if (strength > best_strength) {
                best_strength = strength;
                best_hz = freq;
            }
        }
    }
    if (best_hz <= 0.0) return {0.0, 0.0};
    return phase_search(times, best_hz, duration);
}

static std::pair<double,double> phase_refine(
    const std::vector<double>& times, double approximate, double duration,
    double multiply_limit_hz = 0.0)
{
    std::vector<double> ts = times;
    if (ts.size() > 2000) {
        size_t step = std::max<size_t>(1, ts.size() / 2000);
        std::vector<double> subsampled;
        subsampled.reserve(ts.size() / step + 1);
        for (size_t i = 0; i < ts.size(); i += step) {
            subsampled.push_back(ts[i]);
        }
        ts = std::move(subsampled);
    }

    auto [best_hz, best_strength] = phase_search(ts, approximate, duration);

    std::vector<double> candidates;
    for (int divisor = 2; divisor < 5; ++divisor) {
        candidates.push_back(approximate / divisor);
    }
    for (int factor = 2; factor < 5; ++factor) {
        // When a grid-coherence correction already picked the fundamental,
        // do not let a stronger grid harmonic pull the result back up.
        if (multiply_limit_hz > 0.0 && approximate * factor > multiply_limit_hz) {
            continue;
        }
        candidates.push_back(approximate * factor);
    }
    for (double candidate : candidates) {
        if (candidate < 20.0 || candidate > 20000.0) continue;
        auto [chz, cstr] = phase_search(ts, candidate, duration);
        if (cstr >= 0.35 && cstr > best_strength + 0.12) {
            best_hz = chz;
            best_strength = cstr;
        }
    }
    return {best_hz, best_strength};
}

static std::vector<Candidate> spectrum_candidates(
    const std::string& chart, const std::vector<double>& data, double x_factor)
{
    std::vector<Candidate> result;
    size_t length = data.size();
    if (length < 20) return result;

    bool any_finite = false;
    for (double v : data) {
        if (std::isfinite(v)) { any_finite = true; break; }
    }
    if (!any_finite) return result;

    size_t min_index = std::max<size_t>(1, static_cast<size_t>(std::ceil(50.0 / x_factor)));
    size_t max_index = length - 3;
    if (max_index <= min_index + 1) return result;

    double floor_val = 1e-12;
    {
        std::vector<double> core(data.begin() + min_index, data.begin() + max_index + 1);
        std::sort(core.begin(), core.end());
        floor_val = core[core.size() / 2];
        if (floor_val < 1e-12) floor_val = 1e-12;
    }

    for (size_t i = min_index; i <= max_index; ++i) {
        double peak = data[i];
        if (!std::isfinite(peak) || peak <= 0.0 || peak < floor_val) continue;

        double left = (i > 0) ? data[i - 1] : 0.0;
        double right = (i < length - 1) ? data[i + 1] : 0.0;
        if (peak < left || peak < right) continue;

        double freq = static_cast<double>(i) * x_factor;
        if (freq < 20.0) continue;

        double second = 0.0;
        double min_dist = std::max(1.5 * x_factor, freq * 0.05);
        for (size_t j = 0; j < length; ++j) {
            double fj = static_cast<double>(j) * x_factor;
            if (std::abs(fj - freq) > min_dist) {
                if (data[j] > second) second = data[j];
            }
        }

        double prominence = peak / floor_val;
        double ratio = peak / std::max(second, 1e-12);
        result.push_back(Candidate{freq, peak, prominence, ratio, chart, 1});
    }
    return result;
}

static std::pair<double,double> rank_key(const Candidate& c) {
    return {c.prominence, c.ratio};
}

static std::vector<Candidate> combine_candidates(std::vector<Candidate> candidates) {
    // Stable sort matters: an evenly spaced event train produces equal-rank
    // candidates at f, 2f, 3f... The Python reference keeps the lowest
    // frequency first (stable sorted()), so we must too; otherwise 2f wins
    // and the estimate reports twice the real rate.
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            auto ra = rank_key(a), rb = rank_key(b);
            if (ra.first != rb.first) return ra.first > rb.first;
            return ra.second > rb.second;
        });

    std::vector<Candidate> groups;
    for (const auto& cand : candidates) {
        bool merged = false;
        for (auto& g : groups) {
            double tolerance = 0.03 * std::max(cand.freq, g.freq);
            if (std::abs(cand.freq - g.freq) <= tolerance) {
                g.evidence += cand.evidence;
                g.prominence = std::max(g.prominence, cand.prominence);
                g.ratio = std::max(g.ratio, cand.ratio);
                g.peak = std::max(g.peak, cand.peak);
                if (g.chart.find(cand.chart) == std::string::npos) {
                    if (!g.chart.empty()) g.chart += ", ";
                    g.chart += cand.chart;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            groups.push_back(cand);
        }
    }
    return groups;
}

RateEstimate estimate_rate(
    const std::vector<std::tuple<std::string, std::vector<double>, double>>& spectra,
    int bin_rate, int hps, int event_count, double duration,
    const std::vector<double>* times, bool tentative)
{
    std::vector<std::string> warnings;

    if (event_count < 20) {
        return RateEstimate{std::nullopt, "insufficient", 0.0, "", warnings, tentative, std::nullopt};
    }
    if (duration < 0.5) {
        return RateEstimate{std::nullopt, "insufficient", 0.0, "", warnings, tentative, std::nullopt};
    }

    std::vector<Candidate> candidates;
    for (const auto& [chart, data, factor] : spectra) {
        auto cs = spectrum_candidates(chart, data, factor);
        candidates.insert(candidates.end(), cs.begin(), cs.end());
    }

    if (candidates.empty()) {
        return RateEstimate{std::nullopt, "insufficient", 0.0, "", warnings, tentative, std::nullopt};
    }

    auto groups = combine_candidates(std::move(candidates));
    auto best_it = std::max_element(groups.begin(), groups.end(),
        [](const Candidate& a, const Candidate& b) { return rank_key(a) < rank_key(b); });
    Candidate best = *best_it;

    // Fundamental selection. Host timing grids (for example the USB 1 ms frame
    // clock visible in evdev timestamps) make every event coherent with the
    // grid, which inflates the apparent prominence of k·f over the true report
    // cadence f: an 8 ms-cadence mouse lands on the 2 ms frame sub-grid and
    // grows a 500 Hz peak taller than its own 125 Hz fundamental. A whole
    // harmonic family at multiples of the cadence is the fingerprint. When the
    // top group is an integer multiple of another comparably prominent group,
    // the slower rate is the report cadence. Thresholds separate the cases:
    // frame-grid sub-harmonics reach ~0.88 of the top prominence, while
    // unrelated sidebands stay below ~0.3.
    for (int k = 16; k >= 2; --k) {
        double sub = best.freq / static_cast<double>(k);
        if (sub < 20.0) continue;
        for (const auto& g : groups) {
            if (std::abs(g.freq - sub) <= 0.025 * sub &&
                g.prominence >= 0.5 * best.prominence) {
                LOG_INFO("Analysis") << "Fundamental selection: " << best.freq
                                     << " Hz is " << k << "x the prominent "
                                     << g.freq << " Hz cadence; reporting the fundamental.";
                best = g;
                warnings.push_back(
                    "Peak family at multiples of the reported rate suggests a host "
                    "timing grid (e.g. USB frame quantization); the reported value is "
                    "the fundamental report cadence.");
                break;
            }
        }
        if (best.freq < sub * 1.5) break;  // downgraded; one step is enough
    }

    // Modal-gap cadence reconciliation. The consecutive-delta histogram
    // measures actual report-to-report gaps and is immune to host timing-grid
    // coherence (unlike the circular/all-pairs spectra). When the spectral
    // winner is an integer multiple of the rate implied by the modal
    // consecutive gap, the grid inflated a harmonic; report the gap cadence.
    // (The prominence-based pass above already fixed the real-recording case;
    // this catches cleaner data where the grid harmonics tower over the
    // cadence harmonics, e.g. continuous motion with little jitter.)
    double multiply_limit_hz = 0.0;
    if (times && times->size() >= 40) {
        std::vector<long> votes(bin_rate, 0);
        long total = 0;
        for (size_t i = 1; i < times->size(); ++i) {
            double d = (*times)[i] - (*times)[i - 1];
            if (d <= 0.0 || d >= 1.0) continue;
            int64_t idx = static_cast<int64_t>(std::llround(d * bin_rate));
            if (idx > 0 && idx < bin_rate) {
                votes[idx] += 1;
                total += 1;
            }
        }
        if (total >= 20) {
            size_t p = std::max_element(votes.begin(), votes.end()) - votes.begin();
            double share = votes[p] / static_cast<double>(total);
            if (p > 0 && share >= 0.2) {
                // Center of mass over the modal cluster for sub-bin precision.
                double sum = 0.0, weight = 0.0;
                for (int q = std::max(1, static_cast<int>(p) - 1);
                     q <= std::min(bin_rate - 1, static_cast<int>(p) + 1); ++q) {
                    sum += static_cast<double>(votes[q]) * q;
                    weight += static_cast<double>(votes[q]);
                }
                double modal_rate = bin_rate / (sum / weight);  // Hz
                double mult = best.freq / modal_rate;
                double m_round = std::round(mult);
                if (m_round >= 2.0 && m_round <= 16.0 &&
                    std::fabs(mult - m_round) <= 0.02 * m_round) {
                    LOG_INFO("Analysis") << "Cadence reconciliation: spectral winner "
                                         << best.freq << " Hz is " << static_cast<int>(m_round)
                                         << "x the modal consecutive gap (" << modal_rate
                                         << " Hz); reporting the gap cadence.";
                    best.freq = modal_rate;
                    best.chart += ", modal gap";
                    warnings.push_back(
                        "Spectral peaks were harmonics of the report cadence (host "
                        "timing-grid coherence); the reported value is the cadence "
                        "from the modal consecutive gap.");
                    multiply_limit_hz = 1.5 * modal_rate;
                }
            }
        }
    }

    if (best.prominence < 2.5) {
        return RateEstimate{std::nullopt, "insufficient", best.prominence, best.chart, warnings, tentative, std::nullopt};
    }

    std::optional<double> phase_strength_val;
    if (times && times->size() >= 20) {
        auto [refined_hz, ps] = phase_refine(*times, best.freq, duration, multiply_limit_hz);
        phase_strength_val = ps;
        if (ps < 0.35 && !tentative) {
            auto [disc_hz, disc_str] = phase_discover(*times, duration);
            if (disc_str > ps) {
                refined_hz = disc_hz;
                phase_strength_val = disc_str;
            }
        }
        if (*phase_strength_val >= 0.35) {
            best.freq = refined_hz; // refined_hz is in scope
        } else {
            warnings.push_back("Raw timestamps did not confirm a stable timing grid; treat this rate as a hint.");
            if (!(best.prominence >= 8.0 && event_count >= 200)) {
                return RateEstimate{std::nullopt, "insufficient", best.prominence, best.chart, warnings, tentative, phase_strength_val};
            }
        }
    }

    std::string confidence;
    if (phase_strength_val && *phase_strength_val >= 0.55 && event_count >= 60) {
        confidence = "strong";
    } else if (phase_strength_val && *phase_strength_val >= 0.35) {
        confidence = "moderate";
    } else if (phase_strength_val) {
        confidence = "weak";
    } else if (best.prominence >= 8.0 && event_count >= 200) {
        confidence = "strong";
    } else if (best.prominence >= 4.0 && event_count >= 60) {
        confidence = "moderate";
    } else {
        confidence = "weak";
    }

    if (hps > 0) {
        warnings.push_back("HPS " + std::to_string(hps) + " is active; frequencies are shown after harmonic scaling.");
    }
    if (event_count < 100) {
        warnings.push_back("Small sample (fewer than 100 events).");
    }
    if (best.freq >= 0.4 * bin_rate) {
        warnings.push_back("Peak is near the frequency-axis end; raise the binning rate for headroom.");
    }
    if (confidence == "weak") {
        warnings.push_back("Low confidence: treat this as a hint, not a hardware specification.");
    }

    return RateEstimate{best.freq, confidence, best.prominence, best.chart, warnings, tentative, phase_strength_val};
}

AnalysisResult empty_analysis(
    const AnalysisSettings* settings_in,
    MeasurementKind measurement_kind,
    bool tentative)
{
    AnalysisSettings s = settings_in ? *settings_in : AnalysisSettings();
    s = s.validated();

    AnalysisResult result;
    result.event_count = 0;
    result.duration = 0.0;
    result.settings = s;
    result.measurement_kind = measurement_kind;

    int br = s.bin_rate;
    result.diffs = ChartData{std::vector<double>(br, 0.0), 1000.0 / br, 1.0, "ms", false};
    result.compound = ChartData{std::vector<double>(br, 0.0), 1000.0 / br, 1.0, "ms", false};
    result.circular = ChartData{std::vector<double>(br, 0.0), 1000.0 / br, 1.0, "ms", false};

    int flen = br / 2 + 1;
    result.f_diffs = ChartData{std::vector<double>(flen, 0.0), 1.0 / (s.hps + 1), 1.0, "Hz", true};
    result.f_compound = ChartData{std::vector<double>(flen, 0.0), 1.0 / (s.hps + 1), 1.0, "Hz", true};
    result.f_circular = ChartData{std::vector<double>(flen, 0.0), 1.0 / (s.hps + 1), 1.0, "Hz", true};

    result.zero_ms_count = 0;
    result.zero_ms_percent = 0.0;
    result.estimate = RateEstimate{std::nullopt, "insufficient", 0.0, "", {"No visible events."}, tentative, std::nullopt};
    return result;
}

AnalysisResult analyze_times(
    const std::vector<double>& times_in,
    const AnalysisSettings* settings_in,
    MeasurementKind measurement_kind,
    bool tentative)
{
    AnalysisSettings s = settings_in ? *settings_in : AnalysisSettings();
    s = s.validated();

    std::vector<double> values = times_in;
    values.erase(std::remove_if(values.begin(), values.end(),
        [](double v){ return !std::isfinite(v); }), values.end());

    if (values.empty()) {
        return empty_analysis(&s, measurement_kind, tentative);
    }

    std::sort(values.begin(), values.end());
    if (values[0] < 0) {
        double offset = values[0];
        for (double& v : values) v -= offset;
    }

    int bin_rate = s.bin_rate;

    // Consecutive diffs
    std::vector<double> diffs;
    diffs.reserve(values.size() - 1);
    for (size_t i = 1; i < values.size(); ++i) {
        diffs.push_back(values[i] - values[i - 1]);
    }

    // Circular
    std::vector<double> circular;
    circular.reserve(values.size());
    for (double v : values) {
        circular.push_back(std::fmod(v, 1.0));
    }

    auto diff_counts = bin_values(diffs, bin_rate, false);
    auto compound_hist = compound_counts(values, bin_rate);
    auto circular_counts = bin_values(circular, bin_rate, true);
    auto circular_display = rotate_to_peak(circular_counts);

    auto p0 = process_spectrum(diff_counts, s);
    auto p1 = process_spectrum(compound_hist, s);
    auto p2 = process_spectrum(circular_counts, s);

    double duration = values.back() - values.front();

    LOG_DEBUG("Analysis") << "Starting analysis on " << values.size()
                          << " events (duration: " << duration
                          << "s, bin_rate: " << bin_rate
                          << " Hz, HPS: " << s.hps
                          << ", low_cut: " << (s.low_cut ? "on" : "off")
                          << ", tentative: " << (tentative ? "yes" : "no") << ")";

    std::vector<std::tuple<std::string, std::vector<double>, double>> spectra = {
        {"consecutive", p0, 1.0 / (s.hps + 1)},
        {"all pairs", p1, 1.0 / (s.hps + 1)},
        {"circular", p2, 1.0 / (s.hps + 1)},
    };

    auto estimate = estimate_rate(spectra, bin_rate, s.hps,
        static_cast<int>(values.size()), duration, &values, tentative);

    if (estimate.hz.has_value()) {
        LOG_INFO("Analysis") << "Estimate result: " << *estimate.hz << " Hz (" << estimate.confidence << ")"
                             << (estimate.phase_coherence.has_value() ? " [phase: " + std::to_string(*estimate.phase_coherence) + "]" : "");
    } else {
        LOG_DEBUG("Analysis") << "Estimate result: insufficient (" << estimate.confidence << ")";
    }

    int zero_count = 0;
    for (double d : diffs) {
        if (std::abs(d) <= 1e-12) ++zero_count;
    }
    double zero_percent = diffs.empty() ? 0.0 : (100.0 * zero_count / diffs.size());

    AnalysisResult result;
    result.event_count = static_cast<int>(values.size());
    result.duration = duration;
    result.settings = s;
    result.measurement_kind = measurement_kind;
    result.diffs = chart_time(diff_counts, bin_rate);
    result.compound = chart_time(compound_hist, bin_rate);
    result.circular = chart_time(circular_display, bin_rate);
    result.f_diffs = chart_frequency(p0, s.hps);
    result.f_compound = chart_frequency(p1, s.hps);
    result.f_circular = chart_frequency(p2, s.hps);
    result.zero_ms_count = zero_count;
    result.zero_ms_percent = zero_percent;
    result.estimate = estimate;
    return result;
}

AnalysisResult analyze(
    const Recording& recording,
    const AnalysisSettings* settings_in,
    bool tentative)
{
    AnalysisSettings s = settings_in ? *settings_in : recording.settings;
    s = s.validated();
    std::vector<double> times;
    for (const auto& e : recording.visible_events()) {
        times.push_back(e.time);
    }
    return analyze_times(times, &s, recording.measurement_kind, tentative);
}

} // namespace keyboard_inspector
