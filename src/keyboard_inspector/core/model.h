#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace keyboard_inspector {

constexpr int DEFAULT_INPUT_COLOR = 0xFFA9A9A9;

enum class MeasurementKind {
    EVDEV,
    USB_REPORT,
    TETRIO,
    FILE,
};

inline const char* measurement_kind_short(MeasurementKind kind) {
    switch (kind) {
        case MeasurementKind::EVDEV:   return "evdev event timing";
        case MeasurementKind::USB_REPORT: return "USB HID report timing";
        case MeasurementKind::TETRIO:  return "TETR.IO replay, 600 Hz grid";
        case MeasurementKind::FILE:    return "file, method unknown";
    }
    return "unknown";
}

struct InputId {
    int64_t source_id;
    std::string key;

    bool operator==(const InputId&) const = default;
    bool operator<(const InputId& other) const {
        if (source_id != other.source_id) return source_id < other.source_id;
        return key < other.key;
    }
};

struct Event {
    double time;
    bool pressed;
    InputId input_id;
};

struct SourceInfo {
    int64_t id;
    std::string name;
    std::string device_interface;
    int64_t count = 0;
};

struct InputInfo {
    InputId input_id;
    int color = DEFAULT_INPUT_COLOR;
    bool visible = true;
    std::vector<Event> events;
};

struct AnalysisSettings {
    int bin_rate = 4000;
    int hps = 0;
    bool low_cut = true;

    static constexpr int MIN_BIN_RATE = 20;
    static constexpr int MAX_BIN_RATE = 128000;
    static constexpr int MIN_HPS = 0;
    static constexpr int MAX_HPS = 9;

    AnalysisSettings validated() const {
        AnalysisSettings s = *this;
        s.bin_rate = std::max(MIN_BIN_RATE, std::min(s.bin_rate, MAX_BIN_RATE));
        s.hps = std::max(MIN_HPS, std::min(s.hps, MAX_HPS));
        return s;
    }
};

inline std::pair<int, bool> clamp_bin_rate(int value) {
    int clamped = std::max(0, std::min(value, 128000));
    return {clamped, clamped >= 20};
}

struct Recording {
    std::string title;
    std::chrono::system_clock::time_point recorded = std::chrono::system_clock::now();
    double duration = 0.0;
    std::vector<Event> events;
    std::map<int64_t, SourceInfo> sources;
    std::vector<InputInfo> inputs;
    AnalysisSettings settings;
    MeasurementKind measurement_kind = MeasurementKind::EVDEV;

    std::vector<InputInfo> visible_inputs() const {
        std::vector<InputInfo> result;
        for (const auto& i : inputs) {
            if (i.visible) result.push_back(i);
        }
        return result;
    }

    std::set<InputId> visible_input_ids() const {
        std::set<InputId> result;
        for (const auto& i : inputs) {
            if (i.visible) result.insert(i.input_id);
        }
        return result;
    }

    std::vector<Event> visible_events() const {
        auto visible = visible_input_ids();
        std::vector<Event> result;
        for (const auto& e : events) {
            if (visible.count(e.input_id)) result.push_back(e);
        }
        return result;
    }

    const InputInfo* get_info(const InputId& input_id) const {
        for (const auto& i : inputs) {
            if (i.input_id == input_id) return &i;
        }
        return nullptr;
    }

    const SourceInfo* get_best_source(double threshold = 0.0) const {
        const SourceInfo* best = nullptr;
        for (const auto& [id, source] : sources) {
            if (best == nullptr || source.count > best->count) {
                best = &source;
            }
        }
        if (best == nullptr || best->count <= static_cast<int>(events.size() * threshold)) {
            return nullptr;
        }
        return best;
    }

    void sort_inputs_by_device() {
        std::sort(inputs.begin(), inputs.end(), [this](const InputInfo& a, const InputInfo& b) {
            const SourceInfo* sa = sources.count(a.input_id.source_id) ? &sources.at(a.input_id.source_id) : nullptr;
            const SourceInfo* sb = sources.count(b.input_id.source_id) ? &sources.at(b.input_id.source_id) : nullptr;
            std::string na = sa ? sa->name : "";
            std::string nb = sb ? sb->name : "";
            if (na != nb) return na < nb;
            if (a.input_id.source_id != b.input_id.source_id) return a.input_id.source_id < b.input_id.source_id;
            return a.input_id.key < b.input_id.key;
        });
    }

    std::string display_title() const {
        if (!title.empty() && title.find_first_not_of(" \t\n\r") != std::string::npos) {
            return title;
        }
        const SourceInfo* best = get_best_source(0.66);
        std::string name = best ? best->name : "Multiple devices";
        auto time_t = std::chrono::system_clock::to_time_t(recorded);
        std::tm tm = *std::localtime(&time_t);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%m/%d/%Y, %I:%M:%S %p", &tm);
        return name + " on " + std::string(buf);
    }

    int event_count() const { return static_cast<int>(events.size()); }
};

std::vector<Event> dedupe_events(const std::vector<Event>& events);
std::map<InputId, std::vector<Event>> group_input_events(const std::vector<Event>& events);
void default_visibility(Recording& recording, double threshold = 0.8);

} // namespace keyboard_inspector
