#pragma once

#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <vector>
#include <optional>
#include <mutex>

#include "model.h"
#include "../capture/base.h"

namespace keyboard_inspector {

class RecordingSession {
public:
    RecordingSession(MeasurementKind measurement_kind,
                     const AnalysisSettings* settings_in = nullptr,
                     const Recording* freeze_source = nullptr,
                     bool freeze_enabled = false);

    void add(const CaptureEvent& event);
    int synthesize_releases(int64_t source_id, std::optional<double> at_time = std::nullopt);
    double elapsed() const;
    std::optional<Recording> build(std::optional<double> duration = std::nullopt);
    int repeat_count() const { return repeat_dropped; }
    int synthetic_release_count() const { return _synthetic_releases; }
    bool is_dirty() const { return _dirty; }

    MeasurementKind measurement_kind;
    AnalysisSettings settings;
    const Recording* freeze_source = nullptr;
    bool freeze_enabled = false;

    std::vector<Event> events;
    std::vector<double> _raw_timestamps;
    std::map<int64_t, SourceInfo> sources;
    std::optional<double> origin = std::nullopt;
    double last_time = 0.0;
    int repeat_dropped = 0;
    int64_t started_monotonic = 0;
    std::optional<double> first_seen_monotonic = std::nullopt;
    int _synthetic_releases = 0;
    bool _dirty = false;

private:
    std::map<InputId, bool> _last_state;
};

} // namespace keyboard_inspector
