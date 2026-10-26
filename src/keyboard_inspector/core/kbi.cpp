#include "kbi.h"
#include "keynames.h"

#include <fstream>
#include <cstring>
#include <cmath>
#include <chrono>
#include <sstream>
#include <iostream>

namespace keyboard_inspector {

namespace {

constexpr char HEADER[4] = {'K', 'B', 'I', '\0'};
constexpr uint32_t FILE_VERSION = 3;
constexpr int64_t MAX_RECORDS = 20000000;

constexpr int64_t DAYS_0001_TO_1970 = 719162;
constexpr int64_t TICKS_PER_SECOND = 10000000;
constexpr int64_t TICKS_PER_DAY = 86400 * TICKS_PER_SECOND;

int64_t datetime_to_binary(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto duration = tp.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto micros = duration_cast<microseconds>(duration - seconds);
    
    int64_t days_from_1970 = seconds.count() / 86400;
    int64_t day_seconds = seconds.count() % 86400;
    if (day_seconds < 0) {
        day_seconds += 86400;
        days_from_1970 -= 1;
    }
    
    int64_t days_from_0001 = days_from_1970 + DAYS_0001_TO_1970;
    int64_t ticks = days_from_0001 * TICKS_PER_DAY + day_seconds * TICKS_PER_SECOND + micros.count() * 10;
    
    int64_t value = ticks | (static_cast<int64_t>(2) << 62);
    if (value >= (int64_t(1) << 63)) {
        value = value - (int64_t(1) << 63) - (int64_t(1) << 63);
    }
    return value;
}

std::chrono::system_clock::time_point binary_to_datetime(int64_t value) {
    using namespace std::chrono;
    int64_t ticks = value & ((int64_t(1) << 62) - 1);
    int64_t micros = ticks / 10;
    // .NET epoch 0001-01-01 is 719162 days before Unix epoch (1970-01-01)
    auto dotnet_epoch = system_clock::time_point() - days{DAYS_0001_TO_1970};
    return dotnet_epoch + microseconds(micros);
}

void read_exact(std::istream& stream, char* buf, size_t count) {
    stream.read(buf, static_cast<std::streamsize>(count));
    if (stream.gcount() != static_cast<std::streamsize>(count)) {
        throw KbiError("The file is truncated.");
    }
}

std::string read_string(std::istream& stream) {
    uint64_t length = 0;
    int shift = 0;
    while (true) {
        char raw;
        read_exact(stream, &raw, 1);
        length |= static_cast<uint64_t>(raw & 0x7F) << shift;
        if (!(raw & 0x80)) break;
        shift += 7;
        if (shift > 28) throw KbiError("Invalid string length in file.");
    }
    if (length > 64ull * 1024 * 1024) throw KbiError("String in file is too large.");
    std::string result(length, '\0');
    read_exact(stream, &result[0], length);
    return result;
}

void write_string(std::ostream& stream, const std::string& value) {
    size_t length = value.size();
    while (length >= 0x80) {
        char b = static_cast<char>((length & 0x7F) | 0x80);
        stream.write(&b, 1);
        length >>= 7;
    }
    char b = static_cast<char>(length);
    stream.write(&b, 1);
    stream.write(value.data(), value.size());
}

// Legacy .kbi v0 sources are typed integer ids, not device handles.
constexpr int64_t kLegacySourceKeyboard = 0;
constexpr int64_t kLegacySourceGamepad = 1;
constexpr int64_t kLegacySourceMouse = 2;
constexpr int64_t kLegacySourceTetrio = 3;

// Read an Input as upstream Input.FromBinary does. For v0 the key is a 2-byte
// UTF-16 char (as read by .NET BinaryReader.ReadChar) followed by a typed
// int32 id; for v1+ it is a 7-bit length string plus an int64 source handle.
InputId read_input(std::istream& stream, uint32_t version) {
    if (version == 0) {
        char discriminant[2];
        read_exact(stream, discriminant, 2);
        int32_t value;
        read_exact(stream, reinterpret_cast<char*>(&value), 4);
        switch (discriminant[0]) {
            case 'k':
                return InputId{kLegacySourceKeyboard, windows_key_name(value)};
            case 'g':
            case 'w':
                return InputId{kLegacySourceGamepad, gamepad_key_name(value)};
            case 'm':
                return InputId{kLegacySourceMouse, mouse_key_name(value)};
            case 't':
                return InputId{kLegacySourceTetrio, tetrio_key_name(value)};
            default:
                throw KbiError("Invalid legacy input type in file.");
        }
    }

    std::string key = read_string(stream);
    int64_t source_id;
    read_exact(stream, reinterpret_cast<char*>(&source_id), 8);
    return InputId{source_id, key};
}

void write_input(std::ostream& stream, const InputId& input_id) {
    write_string(stream, input_id.key);
    int64_t sid = input_id.source_id;
    stream.write(reinterpret_cast<const char*>(&sid), 8);
}

int32_t read_count(std::istream& stream) {
    int32_t count;
    read_exact(stream, reinterpret_cast<char*>(&count), 4);
    if (count < 0 || count > MAX_RECORDS) throw KbiError("Invalid record count in file.");
    return count;
}

int32_t read_color(std::istream& stream) {
    int32_t val;
    read_exact(stream, reinterpret_cast<char*>(&val), 4);
    return val & 0xFFFFFFFF;
}

void write_color(std::ostream& stream, int32_t argb) {
    int32_t signed_val = (argb >= (1 << 31)) ? argb - (int64_t(1) << 32) : argb;
    stream.write(reinterpret_cast<const char*>(&signed_val), 4);
}

Recording read_kbi_stream(std::istream& stream) {
    char header[4];
    read_exact(stream, header, 4);
    if (memcmp(header, HEADER, 4) != 0) {
        throw KbiError("This is not a Keyboard Inspector .kbi file.");
    }
    
    uint32_t version;
    read_exact(stream, reinterpret_cast<char*>(&version), 4);
    
    // v<3 files predate the creator string; upstream hardcodes this value.
    std::string creator = version >= 3 ? read_string(stream) : "Keyboard Inspector 0.5.0";
    if (version > FILE_VERSION) {
        throw KbiError("This file was created in a newer version (" + creator + ").");
    }
    std::string title = read_string(stream);
    
    int64_t raw_time;
    read_exact(stream, reinterpret_cast<char*>(&raw_time), 8);
    auto recorded = binary_to_datetime(raw_time);
    
    double duration;
    read_exact(stream, reinterpret_cast<char*>(&duration), 8);
    if (!std::isfinite(duration) || duration < 0) duration = 0.0;
    
    int32_t event_count = read_count(stream);
    std::vector<Event> raw_events;
    raw_events.reserve(event_count);
    for (int32_t i = 0; i < event_count; ++i) {
        double time;
        read_exact(stream, reinterpret_cast<char*>(&time), 8);
        if (!std::isfinite(time) || time < 0) throw KbiError("File contains an invalid event timestamp.");
        
        char pressed_byte;
        read_exact(stream, &pressed_byte, 1);
        bool pressed = pressed_byte != 0;
        
        InputId input_id = read_input(stream, version);
        raw_events.push_back(Event{time, pressed, input_id});
    }
    
    std::map<int64_t, SourceInfo> sources;
    if (version == 0) {
        // v0 has no source dictionary; it is reconstructed from the typed
        // legacy ids present in the event stream, with raw (pre-dedupe)
        // per-source event counts, matching upstream LegacySources.
        static const std::map<int64_t, std::string> legacy_names = {
            {kLegacySourceKeyboard, "Keyboard"},
            {kLegacySourceGamepad, "Gamepad"},
            {kLegacySourceMouse, "Mouse"},
            {kLegacySourceTetrio, "TETR.IO Replay"},
        };
        std::map<int64_t, int64_t> counts;
        for (const auto& event : raw_events) {
            ++counts[event.input_id.source_id];
        }
        for (const auto& [id, name] : legacy_names) {
            auto it = counts.find(id);
            if (it == counts.end()) continue;
            sources[id] = SourceInfo{id, name, "", it->second};
        }
    } else {
        int32_t source_count = read_count(stream);
        for (int32_t i = 0; i < source_count; ++i) {
            int64_t source_id;
            read_exact(stream, reinterpret_cast<char*>(&source_id), 8);
            
            int32_t count;
            read_exact(stream, reinterpret_cast<char*>(&count), 4);
            
            std::string name = read_string(stream);
            std::string interface = read_string(stream);
            
            sources[source_id] = SourceInfo{source_id, name, interface, count};
        }
    }
    
    int32_t bin_rate, hps;
    read_exact(stream, reinterpret_cast<char*>(&bin_rate), 4);
    read_exact(stream, reinterpret_cast<char*>(&hps), 4);
    
    char low_cut_byte;
    read_exact(stream, &low_cut_byte, 1);
    bool low_cut = low_cut_byte != 0;

    AnalysisSettings settings{bin_rate, hps, low_cut};
    settings = settings.validated();
    
    std::vector<InputInfo> inputs;
    if (version >= 2) {
        int32_t input_count = read_count(stream);
        inputs.reserve(input_count);
        for (int32_t i = 0; i < input_count; ++i) {
            InputId input_id = read_input(stream, version);
            
            int32_t color = read_color(stream);
            
            char visible_byte;
            read_exact(stream, &visible_byte, 1);
            bool visible = visible_byte != 0;
            
            inputs.push_back(InputInfo{input_id, color, visible, {}});
        }
    }
    
    auto deduped = dedupe_events(raw_events);
    auto groups = group_input_events(deduped);
    
    std::set<InputId> known;
    for (auto& info : inputs) {
        known.insert(info.input_id);
    }
    for (const auto& [input_id, group] : groups) {
        if (known.count(input_id) == 0) {
            inputs.push_back(InputInfo{input_id, DEFAULT_INPUT_COLOR, true, group});
            known.insert(input_id);
        }
    }
    for (auto& info : inputs) {
        auto it = groups.find(info.input_id);
        if (it != groups.end()) {
            info.events = it->second;
        }
    }
    
    std::set<int64_t> used_sources;
    for (const auto& e : deduped) {
        used_sources.insert(e.input_id.source_id);
    }
    for (int64_t source_id : used_sources) {
        if (sources.count(source_id) == 0) {
            int64_t cnt = 0;
            for (const auto& e : deduped) {
                if (e.input_id.source_id == source_id) ++cnt;
            }
            sources[source_id] = SourceInfo{source_id, "Unknown device " + std::to_string(source_id), "", cnt};
        }
    }
    
    if (deduped.empty()) {
        duration = 0.0;
    } else if (duration <= 0) {
        duration = 0.0;
        for (const auto& e : deduped) {
            if (e.time > duration) duration = e.time;
        }
    }
    
    return Recording{
        title, recorded, duration, deduped, sources, inputs, settings, MeasurementKind::FILE
    };
}

void write_kbi_stream(std::ostream& stream, const Recording& recording) {
    stream.write(HEADER, 4);
    
    uint32_t version = FILE_VERSION;
    stream.write(reinterpret_cast<const char*>(&version), 4);
    
    write_string(stream, "Keyboard Inspector Linux " + std::string(APP_VERSION));
    write_string(stream, recording.title);
    
    int64_t raw_time = datetime_to_binary(recording.recorded);
    stream.write(reinterpret_cast<const char*>(&raw_time), 8);
    
    double duration = static_cast<double>(recording.duration);
    stream.write(reinterpret_cast<const char*>(&duration), 8);
    
    const auto& events = recording.events;
    int32_t event_count = static_cast<int32_t>(events.size());
    stream.write(reinterpret_cast<const char*>(&event_count), 4);
    for (const auto& event : events) {
        double t = event.time;
        stream.write(reinterpret_cast<const char*>(&t), 8);
        char pb = event.pressed ? 1 : 0;
        stream.write(&pb, 1);
        write_string(stream, event.input_id.key);
        int64_t sid = event.input_id.source_id;
        stream.write(reinterpret_cast<const char*>(&sid), 8);
    }
    
    int32_t source_count = static_cast<int32_t>(recording.sources.size());
    stream.write(reinterpret_cast<const char*>(&source_count), 4);
    for (const auto& [sid, source] : recording.sources) {
        stream.write(reinterpret_cast<const char*>(&sid), 8);
        int32_t cnt = static_cast<int32_t>(source.count);
        stream.write(reinterpret_cast<const char*>(&cnt), 4);
        write_string(stream, source.name);
        write_string(stream, source.device_interface);
    }
    
    AnalysisSettings settings = recording.settings.validated();
    stream.write(reinterpret_cast<const char*>(&settings.bin_rate), 4);
    stream.write(reinterpret_cast<const char*>(&settings.hps), 4);
    char lc = settings.low_cut ? 1 : 0;
    stream.write(&lc, 1);
    
    int32_t input_count = static_cast<int32_t>(recording.inputs.size());
    stream.write(reinterpret_cast<const char*>(&input_count), 4);
    for (const auto& info : recording.inputs) {
        write_string(stream, info.input_id.key);
        int64_t sid = info.input_id.source_id;
        stream.write(reinterpret_cast<const char*>(&sid), 8);
        write_color(stream, info.color);
        char vb = info.visible ? 1 : 0;
        stream.write(&vb, 1);
    }
}

} // namespace

Recording read_kbi_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw KbiError("Unable to open the file.");
    return read_kbi_stream(stream);
}

void write_kbi_file(const Recording& recording, const std::string& path) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw KbiError("Unable to save the file.");
    write_kbi_stream(stream, recording);
}

std::string kbi_dumps(const Recording& recording) {
    std::ostringstream oss(std::ios::binary);
    write_kbi_stream(oss, recording);
    return oss.str();
}

Recording kbi_loads(const std::string& data) {
    std::istringstream iss(data, std::ios::binary);
    return read_kbi_stream(iss);
}

} // namespace keyboard_inspector
