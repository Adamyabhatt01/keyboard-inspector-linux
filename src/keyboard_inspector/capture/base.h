#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <functional>

#include "../core/model.h"
#include "../capture/usb_info.h"

namespace keyboard_inspector {

int64_t stable_source_id(const std::vector<std::string>& parts);

struct DeviceInfo {
    std::string id;
    std::string path;
    std::string name;
    std::string phys;
    std::string uniq;
    std::optional<struct UsbInfo> usb;
    bool accessible = false;
    std::string error;
    int key_count = 0;
    bool is_keyboard = true;
    std::string kind = "keyboard"; // keyboard | mouse | gamepad | other
    int64_t source_id = 0;

    bool capture_supported() const {
        return kind == "keyboard" || kind == "mouse" || kind == "gamepad";
    }

    std::string display_name() const {
        if (kind == "keyboard") {
            return !name.empty() ? name : (!path.empty() ? path : id);
        }
        std::string suffix;
        if (kind == "mouse") suffix = "mouse";
        else if (kind == "gamepad") suffix = "gamepad";
        else if (kind == "other") suffix = "not an input device";
        else suffix = kind;
        return (!name.empty() ? name : (!path.empty() ? path : id)) + " (" + suffix + ")";
    }

    std::optional<std::string> usb_summary() const {
        if (!usb) return std::nullopt;
        return usb->summary();
    }
};

struct CaptureEvent {
    int64_t source_id = 0;
    std::string source_name;
    std::string source_interface;
    double timestamp = 0.0;
    int key_code = 0;
    std::string key_name;
    bool pressed = false;
    MeasurementKind measurement_kind = MeasurementKind::EVDEV;
};

struct CaptureError {
    std::string code;      // permission | disconnected | backend_unavailable | parse_error
    std::string message;
    std::optional<int64_t> source_id;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual std::string name() const { return "capture"; }
    virtual MeasurementKind measurement_kind() const { return MeasurementKind::EVDEV; }
    virtual std::vector<DeviceInfo> list_devices() = 0;
    virtual void start(const std::vector<DeviceInfo>& devices) = 0;
    virtual void stop() = 0;
    virtual std::vector<CaptureEvent> drain_events() = 0;
    virtual std::vector<CaptureError> drain_errors() = 0;
    virtual bool is_running() const = 0;
};

} // namespace keyboard_inspector
