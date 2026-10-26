#include "recording.h"

#include <chrono>
#include <ctime>
#include <algorithm>
#include <sstream>

namespace keyboard_inspector {

RecordingSession::RecordingSession(MeasurementKind measurement_kind,
                                   const AnalysisSettings* settings_in,
                                   const Recording* freeze_source,
                                   bool freeze_enabled)
    : measurement_kind(measurement_kind),
      settings(settings_in ? *settings_in : AnalysisSettings()),
      freeze_source(freeze_source),
      freeze_enabled(freeze_enabled)
{
    settings = settings.validated();
    started_monotonic = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void RecordingSession::add(const CaptureEvent& event) {
    double raw = event.timestamp;
    _raw_timestamps.push_back(raw);
    if (!origin) {
        origin = raw;
        first_seen_monotonic = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    }

    double relative = std::max(0.0, raw - *origin);
    InputId input_id{event.source_id, event.key_name};
    Event item{relative, event.pressed, input_id};
    events.push_back(item);
    last_time = std::max(last_time, relative);

    auto it = sources.find(input_id.source_id);
    if (it == sources.end()) {
        SourceInfo src{
            input_id.source_id,
            event.source_name.empty() ? ("Device " + std::to_string(input_id.source_id)) : event.source_name,
            event.source_interface,
            0
        };
        sources[input_id.source_id] = src;
        it = sources.find(input_id.source_id);
    }
    it->second.count += 1;
    _dirty = true;
}

int RecordingSession::synthesize_releases(int64_t source_id, std::optional<double> at_time) {
    double release_time = at_time.value_or(last_time);
    release_time = std::max(0.0, release_time);

    std::map<InputId, bool> states;
    for (const auto& event : events) {
        if (event.input_id.source_id == source_id) {
            states[event.input_id] = event.pressed;
        }
    }

    int count = 0;
    for (const auto& [input_id, pressed] : states) {
        if (!pressed) continue;
        Event release{release_time, false, input_id};
        events.push_back(release);

        double raw_for_release = origin ? (*origin + release_time) : release_time;
        _raw_timestamps.push_back(raw_for_release);

        auto it = sources.find(source_id);
        if (it != sources.end()) {
            it->second.count += 1;
        }
        count += 1;
    }
    _synthetic_releases += count;
    return count;
}

double RecordingSession::elapsed() const {
    return std::max(last_time, 0.0);
}

std::optional<Recording> RecordingSession::build(std::optional<double> duration) {
    if (events.empty()) {
        _dirty = false;
        return std::nullopt;
    }

    // Recompute origin as min raw timestamp
    if (!_raw_timestamps.empty()) {
        double min_raw = _raw_timestamps[0];
        for (double v : _raw_timestamps) {
            if (v < min_raw) min_raw = v;
        }
        origin = min_raw;

        size_t n_raw = _raw_timestamps.size();
        std::vector<Event> normalized;
        normalized.reserve(n_raw);
        for (size_t i = 0; i < n_raw; ++i) {
            normalized.push_back(Event{
                std::max(0.0, _raw_timestamps[i] - *origin),
                events[i].pressed,
                events[i].input_id
            });
        }
        std::vector<Event> times_for_analysis = normalized;
        if (events.size() > n_raw) {
            times_for_analysis.insert(times_for_analysis.end(),
                events.begin() + static_cast<long>(n_raw), events.end());
        }
        events = std::move(times_for_analysis);
    }

    auto deduped = dedupe_events(events);
    if (deduped.empty()) {
        return std::nullopt;
    }

    auto sources_copy = sources;
    // Append unique names for duplicates
    {
        std::map<std::string, std::vector<SourceInfo*>> by_name;
        for (auto& [id, src] : sources_copy) {
            by_name[src.name].push_back(&src);
        }
        for (auto& [name, srcs] : by_name) {
            if (srcs.size() < 2) continue;
            std::sort(srcs.begin(), srcs.end(), [](SourceInfo* a, SourceInfo* b) {
                return a->id < b->id;
            });
            for (size_t i = 0; i < srcs.size(); ++i) {
                srcs[i]->name = name + " [" + std::to_string(i + 1) + "]";
            }
        }
    }

    auto groups = group_input_events(deduped);

    std::vector<InputInfo> inputs;
    if (freeze_enabled && freeze_source) {
        std::set<InputId> frozen_ids;
        for (const auto& frozen : freeze_source->inputs) {
            frozen_ids.insert(frozen.input_id);
            if (frozen.visible) {
                auto it = groups.find(frozen.input_id);
                if (it != groups.end()) {
                    inputs.push_back(InputInfo{frozen.input_id, frozen.color, true, it->second});
                }
            } else {
                auto it = groups.find(frozen.input_id);
                if (it != groups.end()) {
                    inputs.push_back(InputInfo{frozen.input_id, frozen.color, false, it->second});
                }
            }
        }
        for (const auto& [input_id, group] : groups) {
            if (frozen_ids.count(input_id)) continue;
            inputs.push_back(InputInfo{input_id, DEFAULT_INPUT_COLOR, true, group});
        }
    } else {
        for (const auto& [input_id, group] : groups) {
            inputs.push_back(InputInfo{input_id, DEFAULT_INPUT_COLOR, true, group});
        }
    }

    Recording rec;
    rec.title = "";
    rec.recorded = std::chrono::system_clock::now();
    rec.duration = duration.value_or(elapsed());
    rec.events = deduped;
    rec.sources = sources_copy;
    rec.inputs = inputs;
    rec.settings = settings.validated();
    rec.measurement_kind = measurement_kind;

    if (!freeze_enabled || !freeze_source) {
        rec.sort_inputs_by_device();
        default_visibility(rec, 0.8);
    }

    _dirty = false;
    return rec;
}

} // namespace keyboard_inspector
