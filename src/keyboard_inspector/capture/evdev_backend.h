#pragma once

#include "base.h"

#include <string>
#include <vector>
#include <set>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

#include <libevdev-1.0/libevdev/libevdev.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>

namespace keyboard_inspector {

// Group the event nodes that belong to one physical device. Virtual devices
// (uinput, ydotoold) often expose neither uniq nor phys, so the evdev id is the
// only stable identity available.
std::string device_identity(const DeviceInfo& device);

// Default selection prefers a runtime report-protocol node over a boot-protocol
// one, then the node with more key capabilities (matches the Python port).
bool prefers_as_default(const DeviceInfo& candidate, const DeviceInfo& current);

// Pick one node per physical keyboard/gamepad; mice only when no keyboard or
// gamepad is readable.
std::set<std::string> default_device_selection(const std::vector<DeviceInfo>& devices);

// Per-reader state machine for the non-button event layers:
// - The gamepad hat switch (ABS_HAT0X/ABS_HAT0Y) becomes HatN/HatE/HatS/HatW
//   press/release inputs, mirroring upstream UnprocessedInput.ProcessHID
//   (GamepadKeys 32..35; a bitmap diffed against the previous report).
// - Every SYN-delimited report that carries at least one EV_REL/EV_ABS axis
//   event produces one synthetic "Movement" input with an alternating pressed
//   state. Upstream ignores mouse/stick movement entirely (RawMouse events
//   with ButtonFlags == 0 are dropped); this is a documented Linux addition
//   that makes mouse/touchpad/gamepad polling rate measurable from evdev
//   (see docs/MEASUREMENT_LIMITS.md and docs/COMPATIBILITY.md).
struct EvdevReportTracker {
    struct SynthEvent {
        int code = 0;        // synthetic key code (0x300+), never a kernel code
        std::string name;    // "HatN"/"HatE"/"HatS"/"HatW"/"Movement"
        bool pressed = false;
    };

    // Process one raw evdev event; hat transitions are returned immediately,
    // one movement event per SYN_REPORT that contained axis activity.
    std::vector<SynthEvent> process(const struct input_event& ev,
                                    const std::string& device_kind);

private:
    bool axis_seen_ = false;
    bool movement_pressed_ = false;
    int hat_x_ = 0;
    int hat_y_ = 0;
    bool hat_valid_ = false;   // first hat values adopt state without emitting
    unsigned hat_bits_ = 0;    // 1=N, 2=E, 4=S, 8=W (upstream hat_bm order)
};

class EvdevBackend : public CaptureBackend {
public:
    std::string name() const override { return "Linux input events (evdev)"; }
    MeasurementKind measurement_kind() const override { return MeasurementKind::EVDEV; }

    EvdevBackend(const std::string& sysfs_root = "/sys", const std::string& input_root = "/dev/input");
    ~EvdevBackend() override { stop(); }

    std::vector<DeviceInfo> list_devices() override;
    void start(const std::vector<DeviceInfo>& devices) override;
    void stop() override;
    std::vector<CaptureEvent> drain_events() override;
    std::vector<CaptureError> drain_errors() override;
    bool is_running() const override;

    int repeat_dropped() const { return repeat_dropped_; }

private:
    void reader_thread(DeviceInfo info, int fd);

    std::string sysfs_root_;
    std::string input_root_;

    std::queue<CaptureEvent> events_queue_;
    std::queue<CaptureError> errors_queue_;
    mutable std::mutex events_mutex_;
    mutable std::mutex errors_mutex_;

    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
    std::vector<int> device_fds_;
    mutable std::mutex fds_mutex_;
    std::atomic<bool> running_{false};

    int repeat_dropped_ = 0;
    mutable std::mutex repeat_mutex_;
};

} // namespace keyboard_inspector
