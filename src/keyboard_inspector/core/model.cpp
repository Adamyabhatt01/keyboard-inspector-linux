#include "model.h"

#include <ctime>

namespace keyboard_inspector {

std::vector<Event> dedupe_events(const std::vector<Event>& events) {
    std::vector<Event> ordered = events;
    std::sort(ordered.begin(), ordered.end(), [](const Event& a, const Event& b) {
        return a.time < b.time;
    });

    std::map<InputId, bool> last;
    std::vector<Event> out;
    for (const auto& event : ordered) {
        auto it = last.find(event.input_id);
        if (it != last.end() && it->second == event.pressed) {
            continue;
        }
        last[event.input_id] = event.pressed;
        out.push_back(event);
    }
    return out;
}

std::map<InputId, std::vector<Event>> group_input_events(const std::vector<Event>& events) {
    std::map<InputId, std::vector<Event>> groups;
    for (const auto& event : events) {
        groups[event.input_id].push_back(event);
    }
    return groups;
}

void default_visibility(Recording& recording, double threshold) {
    const SourceInfo* best = recording.get_best_source(threshold);
    if (best == nullptr) {
        for (auto& info : recording.inputs) {
            info.visible = true;
        }
        return;
    }
    for (auto& info : recording.inputs) {
        info.visible = (info.input_id.source_id == best->id);
    }
}

} // namespace keyboard_inspector
