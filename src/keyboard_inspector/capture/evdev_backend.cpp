#include "evdev_backend.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <map>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <dirent.h>
#include <errno.h>
#include <sstream>
#include <sys/select.h>
#include <unistd.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <linux/input.h>

#include "../core/keynames.h"
#include "../core/logger.h"
#include "../capture/usb_info.h"

namespace keyboard_inspector {

namespace {

std::string read_sysfs(const std::string& path, const std::string& def = "") {
    std::ifstream f(path);
    if (!f) return def;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t start = content.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return def;
    size_t end = content.find_last_not_of(" \t\n\r");
    return content.substr(start, end - start + 1);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<uint16_t> decode_key_bitmap(const std::string& text) {
    std::vector<uint16_t> codes;
    std::istringstream iss(text);
    std::vector<std::string> words;
    std::string word;
    while (iss >> word) words.push_back(word);
    
    for (size_t position = 0; position < words.size(); ++position) {
        const std::string& w = words[words.size() - 1 - position];
        uint64_t value = 0;
        try {
            value = std::stoull(w, nullptr, 16);
        } catch (...) {
            continue;
        }
        uint64_t base = position * 64;
        while (value) {
            uint64_t bit = value & (-value);
            codes.push_back(static_cast<uint16_t>(base + (63 - __builtin_clzll(bit))));
            value ^= bit;
        }
    }
    return codes;
}

} // namespace

std::string device_identity(const DeviceInfo& device) {
    if (!device.uniq.empty()) return device.uniq;
    std::string phys = device.phys;
    auto pos = phys.rfind("/input");
    if (pos != std::string::npos) phys.erase(pos);
    if (!phys.empty()) return phys;
    return device.id;
}

static const char* hat_direction_name(unsigned bit) {
    switch (bit) {
        case 1: return "HatN";
        case 2: return "HatE";
        case 4: return "HatS";
        default: return "HatW";
    }
}

static int hat_direction_code(unsigned bit) {
    switch (bit) {
        case 1: return 0x300;  // synthetic codes; never collide with kernel codes
        case 2: return 0x301;
        case 4: return 0x302;
        default: return 0x303;
    }
}

std::vector<EvdevReportTracker::SynthEvent> EvdevReportTracker::process(
        const struct input_event& ev, const std::string& device_kind) {
    std::vector<SynthEvent> out;

    if (ev.type == EV_SYN) {
        if (ev.code == SYN_REPORT) {
            if (axis_seen_) {
                axis_seen_ = false;
                // Alternating pressed state: consecutive same-state events
                // would be dropped by dedupe, and the timeline shows activity
                // rather than one endless hold.
                movement_pressed_ = !movement_pressed_;
                out.push_back(SynthEvent{0x304, "Movement", movement_pressed_});
            }
        } else if (ev.code == SYN_DROPPED) {
            axis_seen_ = false;
            hat_valid_ = false;
        }
        return out;
    }

    if (ev.type == EV_REL || ev.type == EV_ABS) {
        axis_seen_ = true;
    }

    if (device_kind == "gamepad" && ev.type == EV_ABS &&
        (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y)) {
        if (ev.code == ABS_HAT0X) {
            hat_x_ = static_cast<int>(ev.value);
        } else {
            hat_y_ = static_cast<int>(ev.value);
        }
        unsigned bits = 0;
        if (hat_y_ < 0) bits |= 1;  // N
        if (hat_x_ > 0) bits |= 2;  // E
        if (hat_y_ > 0) bits |= 4;  // S
        if (hat_x_ < 0) bits |= 8;  // W
        if (!hat_valid_) {
            hat_valid_ = true;
            hat_bits_ = bits;
            return out;
        }
        const unsigned diff = bits ^ hat_bits_;
        hat_bits_ = bits;
        for (unsigned bit : {1u, 2u, 4u, 8u}) {
            if (diff & bit) {
                out.push_back(
                    SynthEvent{hat_direction_code(bit), hat_direction_name(bit),
                               (bits & bit) != 0});
            }
        }
    }

    return out;
}

bool prefers_as_default(const DeviceInfo& candidate, const DeviceInfo& current) {
    bool candidate_boot = candidate.usb && candidate.usb->interface_protocol &&
                          *candidate.usb->interface_protocol == 1;
    bool current_boot = current.usb && current.usb->interface_protocol &&
                        *current.usb->interface_protocol == 1;
    if (candidate_boot != current_boot) return !candidate_boot;
    if (candidate.key_count != current.key_count) return candidate.key_count > current.key_count;
    return candidate.path < current.path;
}

std::set<std::string> default_device_selection(const std::vector<DeviceInfo>& devices) {
    std::map<std::string, DeviceInfo> keyboards;
    std::map<std::string, DeviceInfo> gamepads;
    std::map<std::string, DeviceInfo> mice;
    for (const auto& device : devices) {
        if (!device.accessible || !device.capture_supported()) continue;
        auto* bucket = device.kind == "keyboard" ? &keyboards :
                       (device.kind == "gamepad" ? &gamepads : &mice);
        auto identity = device_identity(device);
        auto current = bucket->find(identity);
        if (current == bucket->end() || prefers_as_default(device, current->second)) {
            (*bucket)[identity] = device;
        }
    }

    std::set<std::string> chosen;
    for (const auto& [_, device] : keyboards) chosen.insert(device.id);
    for (const auto& [_, device] : gamepads) chosen.insert(device.id);
    if (chosen.empty()) {
        for (const auto& [_, device] : mice) chosen.insert(device.id);
    }
    return chosen;
}

EvdevBackend::EvdevBackend(const std::string& sysfs_root, const std::string& input_root)
    : sysfs_root_(sysfs_root), input_root_(input_root) {}

std::vector<DeviceInfo> EvdevBackend::list_devices() {
    std::vector<DeviceInfo> result;
    
    DIR* dir = opendir(input_root_.c_str());
    if (!dir) return result;
    
    std::vector<std::string> paths;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.rfind("event", 0) == 0) {
            paths.push_back(join_path(input_root_, name));
        }
    }
    closedir(dir);
    
    std::sort(paths.begin(), paths.end());
    
    for (const auto& path : paths) {
        DeviceInfo info;
        info.path = path;
        
        bool accessible = false;
        std::string error;
        
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES) {
                error = "Permission denied for " + path + ". Add your user to the input group or install the udev rule.";
            } else {
                error = "Unable to open " + path + ": " + std::strerror(errno);
            }
        } else {
            accessible = true;
            close(fd);
        }
        
        info.accessible = accessible;
        info.error = error;
        
        size_t pos = path.find_last_of("/");
        std::string event_name = (pos == std::string::npos) ? path : path.substr(pos + 1);
        
        std::string name_path = join_path(join_path(join_path(sysfs_root_, "class"), "input"), join_path(event_name, "device/name"));
        info.name = read_sysfs(name_path, event_name);
        
        info.phys = read_sysfs(join_path(join_path(join_path(sysfs_root_, "class"), "input"), join_path(event_name, "device/phys")));
        info.uniq = read_sysfs(join_path(join_path(join_path(sysfs_root_, "class"), "input"), join_path(event_name, "device/uniq")));
        
        std::string key_caps = read_sysfs(join_path(join_path(join_path(sysfs_root_, "class"), "input"), join_path(event_name, "device/capabilities/key")));
        auto codes = decode_key_bitmap(key_caps);
        info.key_count = static_cast<int>(codes.size());
        
        bool has_keyboard_code = false;
        bool has_mouse_button = false;
        bool is_pointer_name = false;
        std::string lower_name = info.name;
        for (auto& c : lower_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        
        for (uint16_t code : codes) {
            if (code < 256) has_keyboard_code = true;
            if (code >= 0x110 && code <= 0x117) has_mouse_button = true;
        }
        if (lower_name.find("touchpad") != std::string::npos ||
            lower_name.find("trackball") != std::string::npos ||
            lower_name.find("trackpoint") != std::string::npos ||
            lower_name.find("pointer") != std::string::npos) {
            is_pointer_name = true;
        }
        
        if (has_mouse_button && (is_pointer_name || lower_name.find("mouse") != std::string::npos || !has_keyboard_code)) {
            info.kind = "mouse";
        } else if (has_keyboard_code) {
            info.kind = "keyboard";
        } else if (has_mouse_button) {
            info.kind = "mouse";
        } else {
            info.kind = "other";
        }
        
        info.is_keyboard = (info.kind == "keyboard");
        
        auto usb = find_usb_info(path, sysfs_root_);
        if (usb) {
            info.usb = *usb;
        }
        
        info.id = "evdev:" + event_name;
        info.source_id = stable_source_id({info.uniq, info.phys, info.name, info.usb ? info.usb->sysfs_path : ""});
        
        result.push_back(info);
    }
    
    return result;
}

void EvdevBackend::start(const std::vector<DeviceInfo>& devices) {
    if (running_) {
        stop();
    }
    
    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        while (!events_queue_.empty()) events_queue_.pop();
    }
    {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        while (!errors_queue_.empty()) errors_queue_.pop();
    }
    device_fds_.clear();
    repeat_dropped_ = 0;
    
    if (devices.empty()) {
        LOG_WARN("Capture") << "No devices selected for capture.";
        std::lock_guard<std::mutex> lock(errors_mutex_);
        errors_queue_.push(CaptureError{"permission", "No devices selected.", std::nullopt});
        return;
    }
    
    LOG_INFO("Capture") << "Starting capture on " << devices.size() << " device(s)...";
    for (const auto& info : devices) {
        if (!info.accessible || info.path.empty()) {
            LOG_WARN("Capture") << "Skipping inaccessible device: " << info.name << " (" << info.path << ")";
            std::lock_guard<std::mutex> lock(errors_mutex_);
            errors_queue_.push(CaptureError{
                "permission", info.error.empty() ? (info.name + " is not readable.") : info.error, info.source_id
            });
            continue;
        }
        
        int fd = open(info.path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            LOG_WARN("Capture") << "Failed to open device " << info.path << ": " << std::strerror(errno);
            std::lock_guard<std::mutex> lock(errors_mutex_);
            if (errno == EACCES) {
                errors_queue_.push(CaptureError{"permission", "Permission denied for " + info.path, info.source_id});
            } else {
                errors_queue_.push(CaptureError{"disconnected", "Unable to open " + info.name + ": " + std::strerror(errno), info.source_id});
            }
            continue;
        }
        
        {
            std::lock_guard<std::mutex> lock(fds_mutex_);
            device_fds_.push_back(fd);
        }
        
        LOG_INFO("Capture") << "Opened " << info.name << " on " << info.path << " (fd " << fd << ")";
        threads_.emplace_back(&EvdevBackend::reader_thread, this, info, fd);
    }
    
    running_ = !threads_.empty();
    if (!running_) {
        LOG_ERROR("Capture") << "No device could be opened for capture.";
    }
}

void EvdevBackend::reader_thread(DeviceInfo info, int fd) {
    LOG_DEBUG("Capture") << "Reader thread started for " << info.name << " (fd " << fd << ")";
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        LOG_WARN("Capture") << "libevdev_new_from_fd error for " << info.name << ": " << rc;
        std::lock_guard<std::mutex> lock(errors_mutex_);
        errors_queue_.push(CaptureError{"disconnected", "Unable to open " + info.name + ": libevdev error", info.source_id});
        close(fd);
        return;
    }

    // Kernel evdev timestamps follow CLOCK_MONOTONIC on current kernels but
    // were CLOCK_REALTIME historically. Sample both clocks at reader start and
    // lock the fallback clock to whichever domain the first real event
    // timestamp matches, so a synthetic fallback can never mix domains within
    // one capture (which would corrupt interval deltas).
    auto clock_seconds = [](clockid_t clock) {
        struct timespec ts;
        clock_gettime(clock, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    };
    double realtime_reference = clock_seconds(CLOCK_REALTIME);
    double monotonic_reference = clock_seconds(CLOCK_MONOTONIC);
    enum class TimestampDomain { Unknown, Realtime, Monotonic };
    TimestampDomain domain = TimestampDomain::Unknown;

    // Hat directions and per-report movement events (see EvdevReportTracker).
    EvdevReportTracker report_tracker;

    while (!stop_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        int ready = select(fd + 1, &fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("Capture") << "select error on " << info.name << ": " << std::strerror(errno);
            std::lock_guard<std::mutex> lock(errors_mutex_);
            errors_queue_.push(CaptureError{"disconnected", info.name + " was disconnected.", info.source_id});
            break;
        }
        if (ready == 0) continue;
        
        struct input_event ev;
        while (true) {
            rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == -EAGAIN) break;
            if (rc < 0) {
                LOG_WARN("Capture") << "Device read error / disconnect on " << info.name << ": " << rc;
                std::lock_guard<std::mutex> lock(errors_mutex_);
                errors_queue_.push(CaptureError{"disconnected", info.name + " was disconnected.", info.source_id});
                break;
            }
            
            double timestamp = static_cast<double>(ev.time.tv_sec) + static_cast<double>(ev.time.tv_usec) / 1000000.0;
            if (domain == TimestampDomain::Unknown && std::isfinite(timestamp) && timestamp > 0.0) {
                double rt_distance = std::fabs(timestamp - realtime_reference);
                double mono_distance = std::fabs(timestamp - monotonic_reference);
                domain = rt_distance <= mono_distance ? TimestampDomain::Realtime : TimestampDomain::Monotonic;
                LOG_DEBUG("Capture") << "Timestamp domain for " << info.name << ": "
                                     << (domain == TimestampDomain::Realtime ? "CLOCK_REALTIME" : "CLOCK_MONOTONIC");
            }
            if (!std::isfinite(timestamp) || timestamp <= 0.0) {
                if (domain == TimestampDomain::Monotonic) {
                    timestamp = clock_seconds(CLOCK_MONOTONIC);
                } else {
                    // Unknown or realtime: realtime is the safe default because
                    // historical evdev timestamps are wall-clock based.
                    timestamp = clock_seconds(CLOCK_REALTIME);
                }
            }
            
            if (ev.type == EV_KEY) {
                if (ev.value == 2) {
                    std::lock_guard<std::mutex> lock(repeat_mutex_);
                    repeat_dropped_++;
                    continue;
                }
                if (ev.value != 0 && ev.value != 1) continue;
                
                CaptureEvent event;
                event.source_id = info.source_id;
                event.source_name = info.name;
                event.source_interface = info.path;
                event.timestamp = timestamp;
                event.key_code = ev.code;
                event.key_name = input_label(ev.code, info.kind);
                event.pressed = (ev.value == 1);
                event.measurement_kind = MeasurementKind::EVDEV;
                
                {
                    std::lock_guard<std::mutex> lock(events_mutex_);
                    events_queue_.push(event);
                }
            } else {
                // Hat-switch directions and per-report movement events
                // (EvdevReportTracker); buttons above keep the upstream model.
                for (const auto& synth : report_tracker.process(ev, info.kind)) {
                    CaptureEvent event;
                    event.source_id = info.source_id;
                    event.source_name = info.name;
                    event.source_interface = info.path;
                    event.timestamp = timestamp;
                    event.key_code = synth.code;
                    event.key_name = synth.name;
                    event.pressed = synth.pressed;
                    event.measurement_kind = MeasurementKind::EVDEV;
                    
                    {
                        std::lock_guard<std::mutex> lock(events_mutex_);
                        events_queue_.push(event);
                    }
                }
            }
        }
    }
    
    LOG_DEBUG("Capture") << "Reader thread terminating for " << info.name << " (fd " << fd << ")";
    libevdev_free(dev);
    close(fd);
}

void EvdevBackend::stop() {
    bool was_running = running_.exchange(false);
    stop_ = true;
    
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
    
    {
        std::lock_guard<std::mutex> lock(fds_mutex_);
        device_fds_.clear();
    }
    
    if (was_running) {
        LOG_INFO("Capture") << "Capture stopped. Dropped auto-repeats: " << repeat_dropped_;
    }
}

std::vector<CaptureEvent> EvdevBackend::drain_events() {
    std::vector<CaptureEvent> out;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        while (!events_queue_.empty()) {
            out.push_back(events_queue_.front());
            events_queue_.pop();
        }
    }
    return out;
}

std::vector<CaptureError> EvdevBackend::drain_errors() {
    std::vector<CaptureError> out;
    {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        while (!errors_queue_.empty()) {
            out.push_back(errors_queue_.front());
            errors_queue_.pop();
        }
    }
    return out;
}

bool EvdevBackend::is_running() const {
    return running_;
}

} // namespace keyboard_inspector
