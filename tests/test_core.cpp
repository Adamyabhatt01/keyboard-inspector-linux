#include <catch2/catch_all.hpp>

#include <cmath>
#include <set>
#include <tuple>
#include <linux/input.h>

#include "../src/keyboard_inspector/core/model.h"

#include <QJsonArray>
#include <QJsonObject>
#include "../src/keyboard_inspector/core/analysis.h"
#include "../src/keyboard_inspector/core/kbi.h"
#include "../src/keyboard_inspector/core/recording.h"
#include "../src/keyboard_inspector/core/keynames.h"
#include "../src/keyboard_inspector/core/logger.h"
#include "../src/keyboard_inspector/core/tetrio.h"
#include "../src/keyboard_inspector/capture/base.h"
#include "../src/keyboard_inspector/capture/evdev_backend.h"

using namespace keyboard_inspector;

TEST_CASE("dedupe_events removes consecutive duplicates") {
    std::vector<Event> events = {
        {0.0, true, {1, "a"}},
        {0.1, true, {1, "a"}},   // dup: same source/key/pressed
        {0.2, false, {1, "a"}},
        {0.3, false, {1, "a"}},  // dup
        {0.4, true, {2, "b"}},
    };
    auto out = dedupe_events(events);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].time == 0.0);
    REQUIRE(out[1].time == 0.2);
    REQUIRE(out[2].time == 0.4);
}

TEST_CASE("Recording visible_inputs filters hidden") {
    Recording rec;
    rec.inputs = {
        { {1, "a"}, DEFAULT_INPUT_COLOR, true, {} },
        { {2, "b"}, DEFAULT_INPUT_COLOR, false, {} },
        { {3, "c"}, DEFAULT_INPUT_COLOR, true, {} },
    };
    auto vis = rec.visible_inputs();
    REQUIRE(vis.size() == 2);
}

TEST_CASE("get_best_source returns busiest source") {
    Recording rec;
    rec.sources = {
        {1, SourceInfo{1, "dev1", "", 10}},
        {2, SourceInfo{2, "dev2", "", 50}},
        {3, SourceInfo{3, "dev3", "", 5}},
    };
    rec.events = std::vector<Event>(60);
    auto best = rec.get_best_source();
    REQUIRE(best != nullptr);
    REQUIRE(best->id == 2);
}

TEST_CASE("analysis empty times returns insufficient") {
    auto result = analyze_times({}, nullptr, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 0);
    REQUIRE(!result.estimate.available());
    REQUIRE(result.estimate.confidence == "insufficient");
}

TEST_CASE("analysis single event returns insufficient") {
    auto result = analyze_times({0.5}, nullptr, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 1);
    REQUIRE(!result.estimate.available());
}

TEST_CASE("bin_rate clamping") {
    auto [clamped, valid] = clamp_bin_rate(5);
    REQUIRE(clamped == 5);
    REQUIRE(!valid);

    auto [c2, v2] = clamp_bin_rate(200000);
    REQUIRE(c2 == 128000);
    REQUIRE(v2);

    auto [c3, v3] = clamp_bin_rate(4000);
    REQUIRE(c3 == 4000);
    REQUIRE(v3);
}

TEST_CASE("AnalysisSettings validated") {
    AnalysisSettings s{5, 0, true};
    s = s.validated();
    REQUIRE(s.bin_rate == 20);

    s = AnalysisSettings{200000, 0, true};
    s = s.validated();
    REQUIRE(s.bin_rate == 128000);

    s = AnalysisSettings{4000, 15, true};
    s = s.validated();
    REQUIRE(s.hps == 9);
}

TEST_CASE("kbi roundtrip") {
    Recording rec;
    rec.title = "Test Recording";
    rec.duration = 1.5;
    rec.events = {
        {0.0, true, {1, "a"}},
        {0.5, false, {1, "a"}},
        {0.2, true, {2, "b"}},
        {0.8, false, {2, "b"}},
    };
    rec.sources = {
        {1, SourceInfo{1, "Keyboard 1", "/dev/input/event0", 2}},
        {2, SourceInfo{2, "Keyboard 2", "/dev/input/event1", 2}},
    };
    rec.inputs = {
        { {1, "a"}, DEFAULT_INPUT_COLOR, true, {{0.0, true, {1,"a"}}, {0.5, false, {1,"a"}}} },
        { {2, "b"}, DEFAULT_INPUT_COLOR, true, {{0.2, true, {2,"b"}}, {0.8, false, {2,"b"}}} },
    };
    rec.settings = AnalysisSettings{4000, 0, true};
    rec.measurement_kind = MeasurementKind::EVDEV;

    std::string data = kbi_dumps(rec);
    Recording rec2 = kbi_loads(data);

    REQUIRE(rec2.title == rec.title);
    REQUIRE(rec2.duration == rec.duration);
    REQUIRE(rec2.event_count() == rec.event_count());
    REQUIRE(rec2.settings.bin_rate == rec.settings.bin_rate);
}

namespace {

void push_u32(std::string& s, uint32_t v) {
    s.append(reinterpret_cast<const char*>(&v), 4);
}

void push_i32(std::string& s, int32_t v) { push_u32(s, static_cast<uint32_t>(v)); }

void push_i64(std::string& s, int64_t v) {
    s.append(reinterpret_cast<const char*>(&v), 8);
}

void push_f64(std::string& s, double v) {
    s.append(reinterpret_cast<const char*>(&v), 8);
}

void push_str(std::string& s, const std::string& value) {
    size_t length = value.size();
    while (length >= 0x80) {
        s.push_back(static_cast<char>((length & 0x7F) | 0x80));
        length >>= 7;
    }
    s.push_back(static_cast<char>(length));
    s.append(value);
}

std::string kbi_header(uint32_t version, const std::string& title) {
    std::string s = "KBI\0";
    s.resize(4);
    push_u32(s, version);
    if (version >= 3) push_str(s, "test-creator");
    push_str(s, title);
    push_i64(s, 638000000000000000LL);  // .NET DateTime.ToBinary
    push_f64(s, 1.25);
    return s;
}

// Legacy v1/v2 events store Input as a 7-bit-length string plus an int64.
// v2 carries the InputInfo block; v1 does not.
std::string v2_blob(const std::vector<std::tuple<double, bool, std::string, int64_t>>& events,
                    const std::vector<std::tuple<int64_t, int32_t, std::string, std::string>>& sources,
                    uint32_t version = 2) {
    std::string s = kbi_header(version, "legacy2");
    push_u32(s, static_cast<uint32_t>(events.size()));
    for (const auto& [time, pressed, key, source] : events) {
        push_f64(s, time);
        s.push_back(pressed ? 1 : 0);
        push_str(s, key);
        push_i64(s, source);
    }
    push_u32(s, static_cast<uint32_t>(sources.size()));
    for (const auto& [id, count, name, iface] : sources) {
        push_i64(s, id);
        push_i32(s, count);
        push_str(s, name);
        push_str(s, iface);
    }
    push_i32(s, 4000);  // bin rate
    push_i32(s, 0);     // HPS
    s.push_back(1);     // low cut
    if (version >= 2) {
        // InputInfo: Input (string key, int64 source), int32 color, bool visible.
        std::set<int64_t> seen;
        std::vector<std::tuple<std::string, int64_t, int32_t, char>> inputs;
        for (const auto& [time, pressed, key, source] : events) {
            if (!seen.insert(source).second) continue;
            inputs.emplace_back(key, source, 0xFFA9A9A9u, 1);
        }
        push_u32(s, static_cast<uint32_t>(inputs.size()));
        for (const auto& [key, source, color, visible] : inputs) {
            push_str(s, key);
            push_i64(s, source);
            push_i32(s, static_cast<int32_t>(color));
            s.push_back(visible);
        }
    }
    return s;
}

// v0 events use a 2-byte UTF-16 typed char followed by an int32 id.
std::string v0_blob(const std::vector<std::tuple<double, bool, char, int32_t>>& events) {
    std::string s = kbi_header(0, "legacy0");
    push_u32(s, static_cast<uint32_t>(events.size()));
    for (const auto& [time, pressed, kind, value] : events) {
        push_f64(s, time);
        s.push_back(pressed ? 1 : 0);
        s.push_back(kind);
        s.push_back('\0');  // .NET BinaryReader.ReadChar reads UTF-16LE
        push_i32(s, value);
    }
    push_i32(s, 4000);
    push_i32(s, 0);
    s.push_back(1);
    return s;
}

} // namespace

TEST_CASE("kbi v2 reader synthesizes inputs with defaults") {
    std::string data = v2_blob(
        {{0.0, true, "a", 1}, {0.5, false, "a", 1}, {0.2, true, "b", 2}, {0.8, false, "b", 2}},
        {{1, 2, "Old Keyboard", ""}, {2, 2, "Mouse", ""}}
    );
    Recording rec = kbi_loads(data);

    REQUIRE(rec.title == "legacy2");
    REQUIRE(rec.event_count() == 4);
    REQUIRE(rec.settings.bin_rate == 4000);
    REQUIRE(rec.inputs.size() == 2);
    for (const auto& info : rec.inputs) {
        REQUIRE(info.color == DEFAULT_INPUT_COLOR);
        REQUIRE(info.visible);
        REQUIRE_FALSE(info.events.empty());
    }
    REQUIRE(rec.sources.at(1).name == "Old Keyboard");
    // v<3 files never carried a creator string.
    REQUIRE(rec.inputs[0].events.size() == 2);
}

TEST_CASE("kbi v1 reader synthesizes inputs from events only") {
    std::string data = v2_blob(
        {{0.0, true, "a", 7}, {0.5, false, "a", 7}},
        {{7, 2, "Solo Keyboard", ""}},
        1
    );
    Recording rec = kbi_loads(data);

    REQUIRE(rec.event_count() == 2);
    REQUIRE(rec.inputs.size() == 1);
    REQUIRE(rec.inputs[0].input_id == InputId{7, "a"});
    REQUIRE(rec.inputs[0].visible);
    REQUIRE(rec.inputs[0].events.size() == 2);
}

TEST_CASE("kbi v0 reader decodes typed legacy inputs and sources") {
    // 'k' + VK 65 -> "A" on legacy keyboard source 0.
    // 't' + 0 -> "moveLeft" on legacy TETR.IO source 3.
    // 'g' + 0 -> "Button1" on legacy gamepad source 1.
    std::string data = v0_blob({
        {0.0, true, 'k', 65},
        {0.4, false, 'k', 65},
        {0.1, true, 't', 0},
        {0.6, false, 't', 0},
    });
    Recording rec = kbi_loads(data);

    REQUIRE(rec.event_count() == 4);
    REQUIRE(rec.sources.size() == 2);
    REQUIRE(rec.sources.at(0).name == "Keyboard");
    REQUIRE(rec.sources.at(0).count == 2);
    REQUIRE(rec.sources.at(3).name == "TETR.IO Replay");
    REQUIRE(rec.sources.at(3).count == 2);

    const InputInfo* a = rec.get_info({0, "A"});
    REQUIRE(a != nullptr);
    REQUIRE(a->color == DEFAULT_INPUT_COLOR);
    const InputInfo* move_left = rec.get_info({3, "moveLeft"});
    REQUIRE(move_left != nullptr);
}

TEST_CASE("kbi v0 reader decodes gamepad and mouse typed inputs") {
    std::string data = v0_blob({
        {0.0, true, 'g', 32},   // HatN on legacy gamepad source 1
        {0.5, false, 'g', 32},
        {0.1, true, 'm', 0},    // LeftClick on legacy mouse source 2
        {0.7, false, 'm', 0},
    });
    Recording rec = kbi_loads(data);

    REQUIRE(rec.sources.size() == 2);
    REQUIRE(rec.sources.at(1).name == "Gamepad");
    REQUIRE(rec.sources.at(2).name == "Mouse");
    REQUIRE(rec.get_info({1, "HatN"}) != nullptr);
    REQUIRE(rec.get_info({2, "LeftClick"}) != nullptr);
}

TEST_CASE("legacy key name decoders match .NET Keys enum names") {
    REQUIRE(windows_key_name(13) == "Return");
    REQUIRE(windows_key_name(20) == "Capital");
    REQUIRE(windows_key_name(33) == "PageUp");
    REQUIRE(windows_key_name(34) == "PageDown");
    REQUIRE(windows_key_name(44) == "Snapshot");
    REQUIRE(windows_key_name(65) == "A");
    REQUIRE(windows_key_name(90) == "Z");
    REQUIRE(windows_key_name(48) == "D0");
    REQUIRE(windows_key_name(32) == "Space");
    REQUIRE(windows_key_name(96) == "NumPad0");
    REQUIRE(windows_key_name(112) == "F1");
    REQUIRE(windows_key_name(135) == "F24");
    REQUIRE(windows_key_name(160) == "LShiftKey");
    REQUIRE(windows_key_name(186) == "OemSemicolon");
    REQUIRE(windows_key_name(219) == "OemOpenBrackets");
    REQUIRE(windows_key_name(226) == "OemBackslash");
    REQUIRE(windows_key_name(224) == "224");  // undefined VK prints numerically
    REQUIRE(gamepad_key_name(0) == "Button1");
    REQUIRE(gamepad_key_name(31) == "Button32");
    REQUIRE(gamepad_key_name(32) == "HatN");
    REQUIRE(gamepad_key_name(35) == "HatW");
    REQUIRE(mouse_key_name(0) == "LeftClick");
    REQUIRE(mouse_key_name(4) == "MouseForward");
    REQUIRE(tetrio_key_name(0) == "moveLeft");
    REQUIRE(tetrio_key_name(7) == "hold");
}

TEST_CASE("kbi rejects files from a newer version with the creator") {
    std::string s = "KBI\0";
    s.resize(4);
    push_u32(s, 4);
    push_str(s, "Keyboard Inspector 9.9.9");
    try {
        kbi_loads(s);
        REQUIRE(false);
    } catch (const KbiError& exc) {
        REQUIRE(std::string(exc.what()).find("Keyboard Inspector 9.9.9") != std::string::npos);
    }
}

TEST_CASE("Theme nice_step") {
    // Skipped in C++ test build (Theme requires Qt6::Widgets)
}

TEST_CASE("Theme format_seconds") {
    // Skipped in C++ test build (Theme requires Qt6::Widgets)
}

TEST_CASE("analysis detects 1000 Hz from 1 ms spacing") {
    std::vector<double> times;
    for (int i = 0; i < 600; ++i) {
        times.push_back(i * 0.001);  // 1 ms spacing = 1000 Hz, 0.6 s duration
    }
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 600);
    REQUIRE(result.estimate.available());
    REQUIRE(*result.estimate.hz > 900.0);
    REQUIRE(*result.estimate.hz < 1100.0);
}

TEST_CASE("analysis detects 500 Hz from 2 ms spacing") {
    std::vector<double> times;
    for (int i = 0; i < 300; ++i) {
        times.push_back(i * 0.002);  // 2 ms spacing = 500 Hz, 0.6 s duration
    }
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 300);
    REQUIRE(result.estimate.available());
    // May detect fundamental or a harmonic; check it's a multiple of 500 Hz
    double hz = *result.estimate.hz;
    REQUIRE(hz > 400.0);
    REQUIRE(std::fabs(std::round(hz / 500.0) * 500.0 - hz) < 50.0);
}

TEST_CASE("analysis identical timestamps produces zero ms events") {
    std::vector<double> times(10, 0.5);  // all same timestamp
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 10);
    REQUIRE(result.zero_ms_count == 9);  // 9 diffs of 0 between 10 events
    REQUIRE(result.zero_ms_percent == 100.0);
}

TEST_CASE("RecordingSession add and build") {
    RecordingSession session(MeasurementKind::EVDEV);
    session.add(CaptureEvent{0, "dev1", "/dev/input/event0", 0.0, 30, "a", true, MeasurementKind::EVDEV});
    session.add(CaptureEvent{0, "dev1", "/dev/input/event0", 0.5, 30, "a", false, MeasurementKind::EVDEV});

    auto rec = session.build(0.5);
    REQUIRE(rec.has_value());
    REQUIRE(rec->event_count() == 2);
    REQUIRE(rec->duration == 0.5);
}

TEST_CASE("RecordingSession synthesize_releases for held key") {
    RecordingSession session(MeasurementKind::EVDEV);
    session.add(CaptureEvent{0, "dev1", "/dev/input/event0", 0.0, 30, "a", true, MeasurementKind::EVDEV});

    int count = session.synthesize_releases(0, 0.5);
    REQUIRE(count == 1);

    auto rec = session.build(0.5);
    REQUIRE(rec.has_value());
    REQUIRE(rec->event_count() == 2);  // press + synthesized release
}

TEST_CASE("RecordingSession dirty flag") {
    RecordingSession session(MeasurementKind::EVDEV);
    REQUIRE(!session.is_dirty());

    session.add(CaptureEvent{0, "dev1", "/dev/input/event0", 0.0, 30, "a", true, MeasurementKind::EVDEV});
    REQUIRE(session.is_dirty());

    session.build();
    REQUIRE(!session.is_dirty());
}

TEST_CASE("RecordingSession tolerates freeze enabled without a source") {
    // The UI only enables freeze when a previous result exists, but the session
    // must not depend on that invariant.
    AnalysisSettings settings;
    RecordingSession session(MeasurementKind::EVDEV, &settings, nullptr, true);
    session.add(CaptureEvent{0, "dev1", "/dev/input/event0", 0.0, 30, "a", true, MeasurementKind::EVDEV});

    auto rec = session.build();
    REQUIRE(rec.has_value());
    REQUIRE(rec->event_count() == 1);
    REQUIRE(rec->visible_inputs().size() == 1);
}

TEST_CASE("default_device_selection groups nodes by physical device") {
    DeviceInfo high = {};
    high.id = "evdev:event3";
    high.phys = "usb-0000:00:14.0-3/input0";
    high.kind = "keyboard";
    high.accessible = true;
    high.key_count = 90;

    DeviceInfo low = high;
    low.id = "evdev:event4";
    low.phys = "usb-0000:00:14.0-3/input1";
    low.key_count = 5;

    auto selected = default_device_selection({high, low});
    REQUIRE(selected.size() == 1);
    REQUIRE(selected.count(high.id) == 1);
}

TEST_CASE("default_device_selection keeps virtual devices without phys or uniq") {
    DeviceInfo physical = {};
    physical.id = "evdev:event3";
    physical.phys = "usb-0000:00:14.0-3/input0";
    physical.kind = "keyboard";
    physical.accessible = true;
    physical.key_count = 90;

    DeviceInfo virtual_device = {};
    virtual_device.id = "evdev:event28";
    virtual_device.kind = "keyboard";
    virtual_device.accessible = true;
    virtual_device.key_count = 50;

    auto selected = default_device_selection({physical, virtual_device});
    REQUIRE(selected.count(physical.id) == 1);
    REQUIRE(selected.count(virtual_device.id) == 1);
}

TEST_CASE("default_device_selection prefers runtime over boot protocol") {
    DeviceInfo boot = {};
    boot.id = "evdev:event3";
    boot.phys = "usb-0000:00:14.0-3/input0";
    boot.kind = "keyboard";
    boot.accessible = true;
    boot.key_count = 120;
    boot.usb = UsbInfo{};
    boot.usb->interface_protocol = 1;

    DeviceInfo runtime_node = boot;
    runtime_node.id = "evdev:event4";
    runtime_node.phys = "usb-0000:00:14.0-3/input1";
    runtime_node.key_count = 10;
    runtime_node.usb->interface_protocol = 0;

    auto selected = default_device_selection({boot, runtime_node});
    REQUIRE(selected.size() == 1);
    REQUIRE(selected.count(runtime_node.id) == 1);
}

TEST_CASE("kbi roundtrip preserves settings") {
    Recording rec;
    rec.title = "Settings Test";
    rec.duration = 2.0;
    rec.settings = AnalysisSettings{8000, 3, false};
    rec.measurement_kind = MeasurementKind::USB_REPORT;

    std::string data = kbi_dumps(rec);
    Recording rec2 = kbi_loads(data);

    REQUIRE(rec2.settings.bin_rate == 8000);
    REQUIRE(rec2.settings.hps == 3);
    REQUIRE(!rec2.settings.low_cut);
    // .kbi v3 does not store the capture method; loaded files are always FILE.
    REQUIRE(rec2.measurement_kind == MeasurementKind::FILE);
}

TEST_CASE("measurement_kind_short labels") {
    REQUIRE(std::string(measurement_kind_short(MeasurementKind::EVDEV)) == "evdev event timing");
    REQUIRE(std::string(measurement_kind_short(MeasurementKind::USB_REPORT)) == "USB HID report timing");
    REQUIRE(std::string(measurement_kind_short(MeasurementKind::TETRIO)) == "TETR.IO replay, 600 Hz grid");
    REQUIRE(std::string(measurement_kind_short(MeasurementKind::FILE)) == "file, method unknown");
}

TEST_CASE("default_visibility shows busiest source") {
    Recording rec;
    rec.sources = {
        {1, SourceInfo{1, "dev1", "", 100}},
        {2, SourceInfo{2, "dev2", "", 10}},
    };
    rec.inputs = {
        { {1, "a"}, DEFAULT_INPUT_COLOR, true, {} },
        { {2, "b"}, DEFAULT_INPUT_COLOR, true, {} },
    };
    default_visibility(rec, 0.8);
    REQUIRE(rec.inputs[0].visible == true);   // dev1 has 100 events > 80% threshold
    REQUIRE(rec.inputs[1].visible == false);  // dev2 has only 10 events
}

TEST_CASE("key_label and input_label Windows mapping") {
    REQUIRE(key_label(KEY_A) == "A");
    REQUIRE(key_label(KEY_Z) == "Z");
    REQUIRE(key_label(KEY_1) == "D1");
    REQUIRE(key_label(KEY_0) == "D0");
    REQUIRE(key_label(KEY_SPACE) == "Space");
    REQUIRE(key_label(KEY_ENTER) == "Return");
    REQUIRE(key_label(KEY_ESC) == "Escape");
    REQUIRE(key_label(KEY_LEFTSHIFT) == "LShiftKey");
    REQUIRE(key_label(KEY_KPENTER) == "Return");
    REQUIRE(key_label(KEY_MINUS) == "OemMinus");
    REQUIRE(key_label(999999) == "KEY_999999");

    // Mouse buttons
    REQUIRE(input_label(BTN_LEFT, "keyboard") == "LButton");
    REQUIRE(input_label(BTN_LEFT, "mouse") == "LeftClick");
    REQUIRE(input_label(BTN_RIGHT, "mouse") == "RightClick");
    REQUIRE(input_label(BTN_MIDDLE, "mouse") == "MiddleClick");

    // Gamepad buttons
    REQUIRE(input_label(BTN_SOUTH, "gamepad") == "Button1");
    REQUIRE(input_label(BTN_EAST, "gamepad") == "Button2");
}

TEST_CASE("Logger levels and stream writing") {
    auto& log = Logger::instance();
    log.init(LogLevel::Debug, LogLevel::Debug, "/tmp/test-keyboard-inspector.log");

    LOG_INFO("TestModule") << "Testing info message " << 42;
    LOG_DEBUG("TestModule") << "Testing debug message";

    log.flush();

    std::ifstream in("/tmp/test-keyboard-inspector.log");
    REQUIRE(in.is_open());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("[INFO ]") != std::string::npos);
    REQUIRE(content.find("[TestModule]") != std::string::npos);
    REQUIRE(content.find("Testing info message 42") != std::string::npos);
}

TEST_CASE("analysis recovers 4000 Hz with kissfft") {
    std::vector<double> times;
    for (int i = 0; i < 3001; ++i) {
        times.push_back(i * 0.00025); // 0.25 ms = 4000 Hz, 0.75 s duration
    }
    AnalysisSettings s{16000, 0, true};
    auto result = analyze_times(times, &s, MeasurementKind::EVDEV, false);
    REQUIRE(result.event_count == 3001);
    REQUIRE(result.estimate.available());
    REQUIRE(*result.estimate.hz > 3900.0);
    REQUIRE(*result.estimate.hz < 4100.0);
}

TEST_CASE("analysis picks the 500 Hz fundamental for a 2 ms grid") {
    std::vector<double> times;
    for (int i = 0; i < 3001; ++i) times.push_back(i * 0.002);
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    INFO("hz=" << (result.estimate.hz ? *result.estimate.hz : -1.0)
               << " confidence=" << result.estimate.confidence
               << " chart=" << result.estimate.chart
               << " prominence=" << result.estimate.prominence
               << " phase=" << (result.estimate.phase_coherence ? *result.estimate.phase_coherence : -1.0));
    REQUIRE(result.estimate.available());
    REQUIRE(std::abs(*result.estimate.hz - 500.0) < 2.0);
}

TEST_CASE("analysis reports report cadence, not the USB frame grid") {
    // Regression for the 2.4G Mouse recording: the device reports every 4th
    // USB frame (8 ms = 125 Hz) but evdev timestamps are SOF-locked to the
    // 2 ms frame grid, so every timestamp is 500 Hz-coherent and the circular
    // /all-pairs spectra grow a taller 500 Hz peak than the 125 Hz
    // fundamental. The estimate must report the report cadence.
    std::vector<double> times;
    unsigned seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % 1000;
    };
    double t = 0.0;
    for (int i = 0; i < 2100; ++i) {
        double frames = 4.0;
        if (rnd() < 20) frames += (rnd() < 500) ? 1.0 : -1.0;  // rare frame slips
        t += frames * 0.002;
        double jitter = (static_cast<int>(rnd() % 161) - 80) * 1e-6;  // ±80 µs
        times.push_back(t + jitter);
    }
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    INFO("hz=" << (result.estimate.hz ? *result.estimate.hz : -1.0)
               << " confidence=" << result.estimate.confidence
               << " chart=" << result.estimate.chart);
    REQUIRE(result.estimate.available());
    REQUIRE(std::abs(*result.estimate.hz - 125.0) < 5.0);
}

TEST_CASE("analysis keeps a true 500 Hz cadence on the frame grid") {
    // Same frame grid, but the device really reports every 2nd frame (2 ms):
    // the fundamental-selection pass must not downgrade to 250/125 Hz.
    std::vector<double> times;
    for (int i = 0; i < 2100; ++i) {
        times.push_back(i * 0.002 + 0.00005);  // on-grid with a small offset
    }
    auto result = analyze_times(times, nullptr, MeasurementKind::EVDEV, false);
    INFO("hz=" << (result.estimate.hz ? *result.estimate.hz : -1.0));
    REQUIRE(result.estimate.available());
    REQUIRE(std::abs(*result.estimate.hz - 500.0) < 5.0);
}

// ---------------------------------------------------------------------------
// USB descriptor discovery (capture/usb_info.cpp). These are *configured
// capabilities*, never measurements — the tests pin that labeling too.
// ---------------------------------------------------------------------------

#include "../src/keyboard_inspector/capture/usb_info.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

void write_sysfs_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

std::filesystem::path make_usb_sysfs_tree() {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("ki-usb-test-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::remove_all(root);

    auto device_dir = root / "devices" / "bus" / "1-2";
    auto interface_dir = device_dir / "1-2:1.0";
    auto event_dir = interface_dir / "HID" / "input" / "input9" / "event7";

    write_sysfs_file(device_dir / "idVendor", "36b0\n");
    write_sysfs_file(device_dir / "idProduct", "3002\n");
    write_sysfs_file(device_dir / "speed", "12\n");
    write_sysfs_file(device_dir / "manufacturer", "Feenx\n");
    write_sysfs_file(device_dir / "product", "Core75\n");
    write_sysfs_file(device_dir / "serial", "SN123\n");
    write_sysfs_file(device_dir / "busnum", "1\n");
    write_sysfs_file(device_dir / "devnum", "7\n");
    write_sysfs_file(interface_dir / "bInterfaceNumber", "00\n");
    write_sysfs_file(interface_dir / "bInterfaceClass", "03\n");
    write_sysfs_file(interface_dir / "bInterfaceProtocol", "01\n");
    std::filesystem::create_directories(event_dir / "device");
    write_sysfs_file(interface_dir / "ep_81" / "direction", "in\n");
    write_sysfs_file(interface_dir / "ep_81" / "type", "Interrupt\n");
    write_sysfs_file(interface_dir / "ep_81" / "bInterval", "01\n");
    write_sysfs_file(interface_dir / "ep_81" / "interval", "1000us\n");
    write_sysfs_file(interface_dir / "ep_81" / "wMaxPacketSize", "0008\n");

    std::filesystem::create_directories(root / "class" / "input");
    std::filesystem::create_symlink(
        root / "devices" / "bus" / "1-2" / "1-2:1.0" / "HID" / "input" / "input9" / "event7",
        root / "class" / "input" / "event7");
    return root;
}

} // namespace

TEST_CASE("parse_interval_ms accepts sysfs interval formats") {
    REQUIRE(parse_interval_ms("1") == 1.0);
    REQUIRE(parse_interval_ms("1ms") == 1.0);
    REQUIRE(parse_interval_ms("2 ms") == 2.0);
    REQUIRE(parse_interval_ms("1000us") == 1.0);
    REQUIRE(parse_interval_ms("125 us") == 0.125);
    REQUIRE(parse_interval_ms("0.5") == 0.5);
    REQUIRE(parse_interval_ms("") == std::nullopt);
    REQUIRE(parse_interval_ms("   ") == std::nullopt);
    REQUIRE(parse_interval_ms("abc") == std::nullopt);
    REQUIRE(parse_interval_ms("1x") == std::nullopt);
}

TEST_CASE("configured_max_hz follows USB speed rules") {
    // Full-speed: interval in ms is direct; 1 ms -> 1000 Hz.
    REQUIRE(configured_max_hz(12.0, 1.0, std::nullopt) == 1000.0);
    REQUIRE(configured_max_hz(1.5, 0.125, std::nullopt) == 8000.0);
    // High-speed: bInterval is an exponent; 8000 / 2^(bInterval-1).
    REQUIRE(configured_max_hz(480.0, 0.125, 1) == 8000.0);
    REQUIRE(configured_max_hz(480.0, 0.125, 2) == 4000.0);
    REQUIRE(configured_max_hz(480.0, 1.0, std::nullopt) == 1000.0);
    // Missing data stays nullopt — never guess a rate.
    REQUIRE(configured_max_hz(std::nullopt, 1.0, 1) == std::nullopt);
    REQUIRE(configured_max_hz(12.0, std::nullopt, std::nullopt) == std::nullopt);
}

TEST_CASE("find_usb_info walks a synthetic sysfs tree") {
    auto root = make_usb_sysfs_tree();

    auto info = find_usb_info("/dev/input/event7", root.string());
    REQUIRE(info.has_value());
    REQUIRE(info->vendor_id == "36b0");
    REQUIRE(info->product_id == "3002");
    REQUIRE(info->product_name == "Core75");
    REQUIRE(info->speed_mbps.has_value());
    REQUIRE(*info->speed_mbps == 12.0);
    REQUIRE(info->interface_number.has_value());
    REQUIRE(*info->interface_number == 0);
    REQUIRE(info->interface_class.has_value());
    REQUIRE(*info->interface_class == 3);
    REQUIRE(info->interface_protocol.has_value());
    REQUIRE(*info->interface_protocol == 1);
    REQUIRE(info->interface_label() == "boot keyboard");
    REQUIRE(info->endpoint_address.has_value());
    REQUIRE(*info->endpoint_address == 0x81);
    REQUIRE(info->endpoint_type == "interrupt");
    REQUIRE(info->endpoint_interval_ms.has_value());
    REQUIRE(*info->endpoint_interval_ms == 1.0);
    REQUIRE(info->endpoint_binterval.has_value());
    REQUIRE(*info->endpoint_binterval == 1);
    REQUIRE(info->endpoint_max_packet.has_value());
    REQUIRE(*info->endpoint_max_packet == 8);
    REQUIRE(info->configured_max_hz.has_value());
    REQUIRE(*info->configured_max_hz == 1000.0);
    // The label must always say "configured", never "measured".
    REQUIRE(info->interval_text().find("configured") != std::string::npos);
    REQUIRE(info->interval_text().find("not measured") != std::string::npos);

    // Unknown event node yields nullopt instead of garbage.
    REQUIRE(!find_usb_info("/dev/input/event99", root.string()).has_value());

    std::filesystem::remove_all(root);
}

TEST_CASE("usb info without a USB parent has no configured rate") {
    // Built-in (non-USB) device: no idVendor anywhere up the tree.
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("ki-usb-test-none-" + std::to_string(static_cast<long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "class" / "input" / "event5" / "device");
    auto info = find_usb_info("/dev/input/event5", root.string());
    REQUIRE(!info.has_value());
    std::filesystem::remove_all(root);
}

// ---------------------------------------------------------------------------
// TETR.IO replay parser (core/tetrio.cpp)
// ---------------------------------------------------------------------------

namespace {

QJsonObject tetrio_key_event(const char* type, double frame, const char* key,
                             double subframe, bool hoisted = false) {
    QJsonObject data;
    data.insert("key", key);
    data.insert("subframe", subframe);
    if (hoisted) data.insert("hoisted", true);
    QJsonObject entry;
    entry.insert("type", type);
    entry.insert("frame", frame);
    entry.insert("data", data);
    return entry;
}

QJsonObject tetrio_user(const char* id, const char* username, int wins) {
    QJsonObject user;
    user.insert("id", id);
    user.insert("username", username);
    user.insert("wins", wins);
    return user;
}

QJsonObject tetrio_stats(double pps, double apm, double vs) {
    QJsonObject stats;
    stats.insert("pps", pps);
    stats.insert("apm", apm);
    stats.insert("vsscore", vs);
    return stats;
}

// Leaderboard-style side: id, username, wins, aggregate stats, full replay.
QJsonObject tetrio_side(const char* id, const char* username, int wins,
                        double pps, double apm, double vs,
                        const QJsonArray& events, double frames) {
    QJsonObject side = tetrio_user(id, username, wins);
    side.insert("stats", tetrio_stats(pps, apm, vs));
    QJsonObject replay;
    replay.insert("events", events);
    replay.insert("frames", frames);
    side.insert("replay", replay);
    return side;
}

// Round-side entry: id, alive flag, per-round stats, replay.
QJsonObject tetrio_round_side(const char* id, bool alive,
                              const QJsonArray& events, double frames) {
    QJsonObject side;
    side.insert("id", id);
    side.insert("alive", alive);
    side.insert("stats", tetrio_stats(1.0, 20.0, 50.0));
    QJsonObject replay;
    replay.insert("events", events);
    replay.insert("frames", frames);
    side.insert("replay", replay);
    return side;
}

QJsonDocument make_solo_doc() {
    QJsonArray events;
    events.append(tetrio_key_event("keydown", 12, "left", 0.4));
    events.append(tetrio_key_event("keyup", 60, "left", 0.9));
    events.append(tetrio_key_event("keydown", 120, "z", 0.0));
    events.append(tetrio_key_event("keyup", 121, "z", 0.0));
    events.append(tetrio_key_event("igoto", 130, "", 0.0));  // non-key: dropped
    events.append(tetrio_key_event("keydown", 140, "right", 0.2, true));  // hoisted: dropped

    QJsonObject stats;
    stats.insert("finaltime", 30000.0);  // ms
    QJsonObject results;
    results.insert("stats", stats);

    QJsonObject replay;
    replay.insert("events", events);
    replay.insert("frames", 600.0);
    replay.insert("results", results);

    QJsonArray users;
    users.append(tetrio_user("u1", "Tester", 0));

    QJsonObject root;
    root.insert("version", 1);
    root.insert("ts", "2024-03-01T12:00:00.000Z");
    root.insert("users", users);
    root.insert("replay", replay);
    root.insert("gamemode", "40l");
    return QJsonDocument(root);
}

QJsonDocument make_multi_doc() {
    QJsonArray e1;
    e1.append(tetrio_key_event("keydown", 0, "left", 0.0));
    e1.append(tetrio_key_event("keyup", 30, "left", 0.0));
    QJsonArray e2;
    e2.append(tetrio_key_event("keydown", 0, "z", 0.0));
    e2.append(tetrio_key_event("keyup", 60, "z", 0.0));

    QJsonArray leaderboard;
    leaderboard.append(tetrio_side("u1", "Alice", 2, 1.5, 30.0, 100.0, e1, 1200.0));
    leaderboard.append(tetrio_side("u2", "Bob", 1, 2.0, 40.0, 120.0, e2, 1000.0));

    // Round sides in reversed order to exercise the id matching.
    QJsonArray round;
    round.append(tetrio_round_side("u2", true, e2, 900.0));
    round.append(tetrio_round_side("u1", false, e1, 1000.0));
    QJsonArray rounds;
    rounds.append(round);
    // A degenerate round with a single side must be skipped.
    QJsonArray broken;
    broken.append(tetrio_round_side("u1", true, e1, 100.0));
    rounds.append(broken);

    QJsonObject replay;
    replay.insert("leaderboard", leaderboard);
    replay.insert("rounds", rounds);

    QJsonArray users;
    users.append(tetrio_user("u1", "Alice", 2));
    users.append(tetrio_user("u2", "Bob", 1));

    QJsonObject root;
    root.insert("version", 1);
    root.insert("ts", "2024-03-01T12:00:00.000Z");
    root.insert("users", users);
    root.insert("replay", replay);
    root.insert("gamemode", "league");
    return QJsonDocument(root);
}

} // namespace

TEST_CASE("tetrio solo replay builds a recording on the 600 Hz grid") {
    auto doc = make_solo_doc();
    REQUIRE_FALSE(tetrio_is_multiplayer(doc));

    Recording rec = tetrio_build_recording(doc);

    CHECK(rec.title.rfind("[40 LINES] 30.000 played by Tester on ", 0) == 0);
    CHECK(rec.duration == 10.0);  // 600 frames / 60 fps
    CHECK(rec.measurement_kind == MeasurementKind::TETRIO);
    CHECK(rec.settings.bin_rate == 600);
    CHECK(rec.settings.low_cut);
    REQUIRE(rec.sources.size() == 1);
    CHECK(rec.sources.at(0).name == "TETR.IO Replay");
    CHECK(rec.sources.at(0).count == 4);  // non-key + hoisted dropped

    // Events keep the subframe grid: round(frame + subframe) / 60.
    REQUIRE(rec.events.size() == 4);
    CHECK(rec.events[0].time == 12.0 / 60.0);
    CHECK(rec.events[0].pressed);
    CHECK(rec.events[1].time == 61.0 / 60.0);
    CHECK_FALSE(rec.events[1].pressed);
    CHECK(rec.events[2].time == 2.0);
    CHECK(rec.events[3].time == 121.0 / 60.0);

    // Inputs are grouped per key with their own event lists.
    REQUIRE(rec.inputs.size() == 2);
    std::map<std::string, size_t> per_key;
    for (const auto& input : rec.inputs) per_key[input.input_id.key] = input.events.size();
    REQUIRE(per_key.count("left") == 1);
    REQUIRE(per_key.count("z") == 1);
    CHECK(per_key["left"] == 2);
    CHECK(per_key["z"] == 2);
    for (const auto& input : rec.inputs) CHECK(input.input_id.source_id == 0);
}

TEST_CASE("tetrio multiplayer picker entries and matchup") {
    auto doc = make_multi_doc();
    REQUIRE(tetrio_is_multiplayer(doc));

    auto info = tetrio_inspect_multiplayer(doc);
    CHECK(info.matchup == "ALICE (2) versus (1) BOB");

    // Two entries for the valid round; the degenerate round is skipped.
    REQUIRE(info.rounds.size() == 2);

    const auto& left = info.rounds[0];
    CHECK(left.round_index == 0);
    CHECK(left.player_id == "u1");
    // Round labels use the per-round stats of the matched side.
    CHECK(left.label == "1.00 PPS · 20.00 APM · 50.00 VS");
    CHECK(left.detail.find("Round 1/2") != std::string::npos);
    CHECK(left.detail.find("0:15") != std::string::npos);      // min(1200, 1000→900) frames
    CHECK(left.detail.find("· 2 inputs") != std::string::npos);  // leaderboard replay events
    CHECK(left.detail.find(" · P2 wins") != std::string::npos);  // u1 not alive

    const auto& right = info.rounds[1];
    CHECK(right.player_id == "u2");
    CHECK(right.label == "1.00 PPS · 20.00 APM · 50.00 VS");
    CHECK(right.detail.find(" · P2 wins") != std::string::npos);
}

TEST_CASE("tetrio multiplayer builds a recording for the selected player") {
    auto doc = make_multi_doc();

    Recording rec = tetrio_build_recording(doc, 0, "u1");
    CHECK(rec.title.find("ALICE (2) versus (1) BOB, round 1/2") != std::string::npos);
    CHECK(rec.title.find("[TETRA LEAGUE] ") == 0);
    CHECK(rec.duration == 1000.0 / 60.0);  // chosen side's frames
    REQUIRE(rec.events.size() == 2);
    CHECK(rec.events[0].time == 0.0);
    CHECK(rec.events[0].pressed);
    CHECK(rec.events[1].time == 0.5);  // frame 30 / 60
    CHECK(rec.settings.bin_rate == 600);
    CHECK(rec.measurement_kind == MeasurementKind::TETRIO);

    // Selecting the other side yields that player's replay.
    Recording rec2 = tetrio_build_recording(doc, 0, "u2");
    REQUIRE(rec2.events.size() == 2);
    CHECK(rec2.events[1].time == 1.0);  // frame 60 / 60
}

TEST_CASE("tetrio parser rejects malformed replays") {
    // Missing/old version.
    QJsonObject stale;
    stale.insert("version", 0);
    REQUIRE_THROWS_AS(tetrio_build_recording(QJsonDocument(stale)), TetrioError);
    REQUIRE_THROWS_AS(tetrio_inspect_multiplayer(QJsonDocument(stale)), TetrioError);

    // Multiplayer without a round selection, or with a bad round index.
    auto multi = make_multi_doc();
    REQUIRE_THROWS_AS(tetrio_build_recording(multi), TetrioError);
    REQUIRE_THROWS_AS(tetrio_build_recording(multi, 5, "u1"), TetrioError);

    // Player id not present in the selected round.
    REQUIRE_THROWS_AS(tetrio_build_recording(multi, 0, "nobody"), TetrioError);

    // Solo replay with no key events at all.
    QJsonObject empty_replay;
    empty_replay.insert("events", QJsonArray());
    empty_replay.insert("frames", 60.0);
    QJsonObject empty_root;
    empty_root.insert("version", 1);
    empty_root.insert("users", QJsonArray{tetrio_user("u1", "Tester", 0)});
    empty_root.insert("replay", empty_replay);
    REQUIRE_THROWS_AS(tetrio_build_recording(QJsonDocument(empty_root)), TetrioError);

    // Multiplayer with fewer than two leaderboard entries.
    QJsonObject thin_replay;
    thin_replay.insert("leaderboard", QJsonArray{tetrio_user("u1", "Alice", 0)});
    QJsonObject thin;
    thin.insert("version", 1);
    thin.insert("users", QJsonArray{tetrio_user("u1", "Alice", 0)});
    thin.insert("replay", thin_replay);
    REQUIRE_THROWS_AS(tetrio_inspect_multiplayer(QJsonDocument(thin)), TetrioError);
}

// ---------------------------------------------------------------------------
// Built-in FFT fallback (core/analysis.cpp)
// ---------------------------------------------------------------------------

namespace {

std::vector<std::complex<double>> naive_dft(const std::vector<double>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> out(n);
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> sum;
        for (size_t t = 0; t < n; ++t) {
            const double angle = -2.0 * M_PI * static_cast<double>(k) *
                                 static_cast<double>(t) / static_cast<double>(n);
            sum += x[t] * std::polar(1.0, angle);
        }
        out[k] = sum;
    }
    return out;
}

std::vector<double> random_signal(size_t n, unsigned seed) {
    std::vector<double> x(n);
    unsigned state = seed;
    for (size_t i = 0; i < n; ++i) {
        // xorshift for deterministic pseudo-random input.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        x[i] = static_cast<double>(state % 2001) / 1000.0 - 1.0;
    }
    return x;
}

} // namespace

TEST_CASE("built-in radix-2 FFT matches a naive DFT and round-trips") {
    const size_t n = 64;
    auto x = random_signal(n, 42);

    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = {x[i], 0.0};
    fft_radix2_inplace(a, false);

    auto reference = naive_dft(x);
    for (size_t k = 0; k < n; ++k) {
        CHECK(std::abs(a[k] - reference[k]) < 1e-9);
    }

    // Inverse recovers the original real signal.
    fft_radix2_inplace(a, true);
    for (size_t i = 0; i < n; ++i) {
        CHECK(std::abs(a[i].real() - x[i]) < 1e-9);
        CHECK(std::abs(a[i].imag()) < 1e-9);
    }
}

TEST_CASE("Bluestein transform matches a naive DFT for arbitrary sizes") {
    for (size_t n : {size_t{100}, size_t{127}, size_t{600}}) {
        auto x = random_signal(n, 1337 + static_cast<unsigned>(n));

        std::vector<std::complex<double>> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = {x[i], 0.0};
        dft_bluestein_inplace(a);

        auto reference = naive_dft(x);
        double worst = 0.0;
        for (size_t k = 0; k < n; ++k) {
            worst = std::max(worst, std::abs(a[k] - reference[k]));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("fallback magnitude spectrum recovers spectral lines") {
    SECTION("power-of-two length: line at bin 97 of 4096") {
        const size_t n = 4096;
        std::vector<double> x(n);
        for (size_t t = 0; t < n; ++t) {
            x[t] = std::cos(2.0 * M_PI * 97.0 * static_cast<double>(t) / static_cast<double>(n));
        }
        auto magnitude = real_fft_magnitude_fallback(x);
        size_t peak = 0;
        for (size_t i = 1; i < magnitude.size(); ++i) {
            if (magnitude[i] > magnitude[peak]) peak = i;
        }
        CHECK(peak == 97);
        CHECK(magnitude[97] > 0.99 * n / 2.0);
    }

    SECTION("non-power-of-two length: line at bin 1000 of 4000") {
        const size_t n = 4000;  // upstream default bin rate
        std::vector<double> x(n);
        for (size_t t = 0; t < n; ++t) {
            x[t] = std::cos(2.0 * M_PI * 1000.0 * static_cast<double>(t) / static_cast<double>(n));
        }
        auto magnitude = real_fft_magnitude_fallback(x);
        size_t peak = 0;
        for (size_t i = 1; i < magnitude.size(); ++i) {
            if (magnitude[i] > magnitude[peak]) peak = i;
        }
        CHECK(peak == 1000);
        CHECK(magnitude[1000] > 0.99 * n / 2.0);
    }
}

// ---------------------------------------------------------------------------
// EvdevReportTracker: hat switch + per-report movement events
// ---------------------------------------------------------------------------

namespace {

input_event make_ev(int type, int code, int value) {
    input_event ev{};
    ev.type = static_cast<uint16_t>(type);
    ev.code = static_cast<uint16_t>(code);
    ev.value = value;
    return ev;
}

} // namespace

TEST_CASE("report tracker emits one Movement per axis report") {
    EvdevReportTracker tracker;
    const std::string kind = "mouse";

    // A report with two axis events still yields a single Movement.
    CHECK(tracker.process(make_ev(EV_REL, REL_X, 3), kind).empty());
    CHECK(tracker.process(make_ev(EV_REL, REL_Y, -2), kind).empty());
    auto first = tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind);
    REQUIRE(first.size() == 1);
    CHECK(first[0].name == "Movement");
    CHECK(first[0].pressed);

    // The pressed state alternates so dedupe keeps every report.
    CHECK(tracker.process(make_ev(EV_REL, REL_X, 1), kind).empty());
    auto second = tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind);
    REQUIRE(second.size() == 1);
    CHECK(second[0].name == "Movement");
    CHECK_FALSE(second[0].pressed);

    // A report without axis activity yields nothing.
    CHECK(tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind).empty());

    // EV_KEY events are not movement.
    CHECK(tracker.process(make_ev(EV_KEY, BTN_LEFT, 1), kind).empty());
    auto after_key = tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind);
    CHECK(after_key.empty());
}

TEST_CASE("report tracker maps the hat switch to upstream directions") {
    EvdevReportTracker tracker;
    const std::string kind = "gamepad";

    // First hat values adopt state without emitting (mirrors upstream's
    // last == inputs initial comparison).
    CHECK(tracker.process(make_ev(EV_ABS, ABS_HAT0X, -1), kind).empty());
    CHECK(tracker.process(make_ev(EV_ABS, ABS_HAT0Y, 0), kind).empty());

    // Center → West.
    auto west = tracker.process(make_ev(EV_ABS, ABS_HAT0X, -1), kind);
    // x already -1: no change → nothing.
    CHECK(west.empty());
    // West → North-West: N appears while W stays.
    auto nw = tracker.process(make_ev(EV_ABS, ABS_HAT0Y, -1), kind);
    REQUIRE(nw.size() == 1);
    CHECK(nw[0].name == "HatN");
    CHECK(nw[0].pressed);

    // NW → E: N releases and E presses (upstream iterates bits N,E,S,W).
    auto east = tracker.process(make_ev(EV_ABS, ABS_HAT0X, 1), kind);
    REQUIRE(east.size() == 2);
    CHECK(east[0].name == "HatE");
    CHECK(east[0].pressed);
    CHECK(east[1].name == "HatW");
    CHECK_FALSE(east[1].pressed);

    // E → N (y axis back to center, x still east): N releases.
    auto center = tracker.process(make_ev(EV_ABS, ABS_HAT0Y, 0), kind);
    REQUIRE(center.size() == 1);
    CHECK(center[0].name == "HatN");
    CHECK_FALSE(center[0].pressed);

    // Hat reports also count as axis activity (one Movement per report).
    auto movement = tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind);
    REQUIRE(movement.size() == 1);
    CHECK(movement[0].name == "Movement");
}

TEST_CASE("report tracker hats are gamepad-only and resync-safe") {
    EvdevReportTracker tracker;
    const std::string kind = "mouse";

    // A mouse with hat-shaped ABS events gets movement, not Hat inputs.
    CHECK(tracker.process(make_ev(EV_ABS, ABS_HAT0X, -1), kind).empty());
    auto report = tracker.process(make_ev(EV_SYN, SYN_REPORT, 0), kind);
    REQUIRE(report.size() == 1);
    CHECK(report[0].name == "Movement");

    // SYN_DROPPED resets state; the next hat values adopt silently again.
    CHECK(tracker.process(make_ev(EV_SYN, SYN_DROPPED, 0), "gamepad").empty());
    tracker.process(make_ev(EV_ABS, ABS_HAT0Y, 1), "gamepad");
    CHECK(tracker.process(make_ev(EV_ABS, ABS_HAT0Y, 1), "gamepad").empty());
    // Held South → North: S releases, N presses.
    auto north = tracker.process(make_ev(EV_ABS, ABS_HAT0Y, -1), "gamepad");
    REQUIRE(north.size() == 2);
    CHECK(north[0].name == "HatN");
    CHECK(north[0].pressed);
    CHECK(north[1].name == "HatS");
    CHECK_FALSE(north[1].pressed);
}
