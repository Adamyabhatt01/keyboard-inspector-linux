#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace keyboard_inspector {

struct UsbInfo {
    std::string vendor_id;
    std::string product_id;
    std::string manufacturer;
    std::string product_name;
    std::string serial;
    std::optional<double> speed_mbps;
    std::optional<int64_t> busnum;
    std::optional<int64_t> devnum;
    std::optional<int64_t> interface_number;
    std::optional<int64_t> interface_class;
    std::optional<int64_t> interface_protocol;
    std::optional<int64_t> endpoint_address;
    std::string endpoint_type;
    std::optional<double> endpoint_interval_ms;
    std::optional<int64_t> endpoint_binterval;
    std::optional<int64_t> endpoint_max_packet;
    std::optional<double> configured_max_hz;
    std::string sysfs_path;

    std::string interface_label() const {
        if (interface_class && *interface_class == 3 && interface_protocol && *interface_protocol == 1) {
            return "boot keyboard";
        }
        if (interface_class && *interface_class == 3) {
            return "HID";
        }
        if (!interface_class) {
            return "interface ?";
        }
        return "class " + std::to_string(*interface_class);
    }

    std::string interval_text() const {
        if (!endpoint_interval_ms) return "interval unknown";
        std::string value;
        if (*endpoint_interval_ms >= 1.0) {
            value = std::to_string(*endpoint_interval_ms) + " ms";
        } else {
            value = std::to_string(*endpoint_interval_ms * 1000.0) + " µs";
        }
        if (configured_max_hz) {
            value += " configured (" + std::to_string(*configured_max_hz) + " Hz max, not measured)";
        } else {
            value += " configured (rate unknown, not measured)";
        }
        return value;
    }

    std::string summary() const {
        std::string result = vendor_id + ":" + product_id;
        if (!product_name.empty()) result += " · " + product_name;
        if (interface_number) {
            result += " · interface " + std::to_string(*interface_number) + " (" + interface_label() + ")";
        }
        if (endpoint_address) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s IN 0x%02X", endpoint_type.empty() ? "endpoint" : endpoint_type.c_str(), *endpoint_address);
            result += " · " + std::string(buf);
        }
        result += " · " + interval_text();
        return result;
    }
};

std::optional<double> parse_interval_ms(const std::string& value);
std::optional<double> configured_max_hz(
    std::optional<double> speed_mbps,
    std::optional<double> interval_ms,
    std::optional<int64_t> binterval);

std::optional<UsbInfo> find_usb_info(const std::string& event_path, const std::string& sysfs_root = "/sys");

} // namespace keyboard_inspector
