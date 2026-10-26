#pragma once

#include <cstdint>
#include <complex>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cmath>
#include <algorithm>

#include "model.h"

#ifdef USE_KISSFFT
#include "kiss_fft.h"
#endif

namespace keyboard_inspector {

struct ChartData {
    std::vector<double> values;
    double x_factor = 1.0;
    double y_max = 1.0;
    std::string unit = "ms";
    bool is_frequency = false;
};

struct RateEstimate {
    std::optional<double> hz;
    std::string confidence = "insufficient";
    double prominence = 0.0;
    std::string chart;
    std::vector<std::string> warnings;
    bool tentative = false;
    std::optional<double> phase_coherence;

    bool available() const { return hz.has_value(); }
};

struct AnalysisResult {
    int event_count = 0;
    double duration = 0.0;
    AnalysisSettings settings;
    MeasurementKind measurement_kind = MeasurementKind::EVDEV;
    ChartData diffs;
    ChartData compound;
    ChartData circular;
    ChartData f_diffs;
    ChartData f_compound;
    ChartData f_circular;
    int zero_ms_count = 0;
    double zero_ms_percent = 0.0;
    RateEstimate estimate;
};

std::vector<double> bin_values(const std::vector<double>& values, int bin_rate, bool modular = false);
std::vector<double> compound_diffs(const std::vector<double>& times);
std::vector<double> compound_counts(const std::vector<double>& times, int bin_rate);

// Built-in FFT (no kissfft dependency): in-place radix-2 for power-of-two
// sizes, Bluestein chirp-z for arbitrary sizes, and a real-input magnitude
// spectrum helper. Exposed for tests and for reuse when kissfft is absent.
void fft_radix2_inplace(std::vector<std::complex<double>>& data, bool inverse);
void dft_bluestein_inplace(std::vector<std::complex<double>>& x);
std::vector<double> real_fft_magnitude_fallback(const std::vector<double>& input);

std::vector<double> rotate_to_peak(const std::vector<double>& counts);
std::vector<double> magnitude_spectrum(const std::vector<double>& counts);
std::vector<double> apply_low_cut(const std::vector<double>& data);
std::vector<double> harmonic_product(const std::vector<double>& data, int hps);
std::vector<double> process_spectrum(const std::vector<double>& counts, const AnalysisSettings& settings);

ChartData chart_time(const std::vector<double>& counts, int bin_rate);
ChartData chart_frequency(const std::vector<double>& processed, int hps);

RateEstimate estimate_rate(
    const std::vector<std::tuple<std::string, std::vector<double>, double>>& spectra,
    int bin_rate, int hps, int event_count, double duration,
    const std::vector<double>* times = nullptr, bool tentative = false);

AnalysisResult empty_analysis(
    const AnalysisSettings* settings = nullptr,
    MeasurementKind measurement_kind = MeasurementKind::EVDEV,
    bool tentative = false);

AnalysisResult analyze_times(
    const std::vector<double>& times,
    const AnalysisSettings* settings = nullptr,
    MeasurementKind measurement_kind = MeasurementKind::EVDEV,
    bool tentative = false);

AnalysisResult analyze(
    const Recording& recording,
    const AnalysisSettings* settings = nullptr,
    bool tentative = false);

} // namespace keyboard_inspector
