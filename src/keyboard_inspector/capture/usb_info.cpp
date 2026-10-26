#include "usb_info.h"

#include <string>
#include <optional>
#include <fstream>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>

namespace keyboard_inspector {

std::string read_file(const std::string& path, const std::string& default_val = "") {
    std::ifstream f(path);
    if (!f) return default_val;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Trim whitespace
    size_t start = content.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return default_val;
    size_t end = content.find_last_not_of(" \t\n\r");
    return content.substr(start, end - start + 1);
}

std::optional<int64_t> parse_int(const std::string& value, int base = 10) {
    if (value.empty()) return std::nullopt;
    try {
        size_t pos = 0;
        long long v = std::stoll(value, &pos, base);
        if (pos != value.size()) return std::nullopt;
        return static_cast<int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> parse_double(const std::string& value) {
    if (value.empty()) return std::nullopt;
    try {
        size_t pos = 0;
        double v = std::stod(value, &pos);
        if (pos != value.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> parse_interval_ms(const std::string& value) {
    std::string v = value;
    // Trim
    size_t start = v.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::nullopt;
    size_t end = v.find_last_not_of(" \t\n\r");
    v = v.substr(start, end - start + 1);
    
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    if (v.empty()) return std::nullopt;
    try {
        if (v.size() >= 2 && v.substr(v.size()-2) == "ms") {
            return std::stod(v.substr(0, v.size()-2));
        }
        if (v.size() >= 2 && v.substr(v.size()-2) == "us") {
            return std::stod(v.substr(0, v.size()-2)) / 1000.0;
        }
        if (v.size() >= 1 && v.back() == 's') {
            return std::stod(v.substr(0, v.size()-1)) * 1000.0;
        }
        size_t pos = 0;
        double parsed = std::stod(v, &pos);
        if (pos != v.size()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> configured_max_hz_impl(
    std::optional<double> speed_mbps,
    std::optional<double> interval_ms,
    std::optional<int64_t> binterval)
{
    if (!speed_mbps) return std::nullopt;
    if (*speed_mbps <= 12.0) {
        if (interval_ms && *interval_ms > 0) {
            return 1000.0 / *interval_ms;
        }
        return std::nullopt;
    }
    if (binterval && *binterval > 0) {
        return 8000.0 / std::pow(2.0, *binterval - 1);
    }
    if (interval_ms && *interval_ms > 0) {
        return 1000.0 / *interval_ms;
    }
    return std::nullopt;
}

int endpoint_address_from_name(const std::string& name) {
    if (name.size() < 4) return -1;
    try {
        return std::stoi(name.substr(3), nullptr, 16);
    } catch (...) {
        return -1;
    }
}

bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

struct EndpointData {
    std::optional<int64_t> address;
    std::string interval;
    std::string binterval;
    std::string max_packet;
    std::string type;
};

std::pair<std::string, EndpointData> find_interrupt_in_endpoint(const std::string& interface_dir) {
    EndpointData empty{};
    // Scan ep_* directories using POSIX opendir/readdir

    std::string best_name;
    int best_addr = 0xFF;
    EndpointData best_data{};
    
    DIR* dir = opendir(interface_dir.c_str());
    if (!dir) return {"", empty};
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 3 || name.substr(0, 3) != "ep_") continue;
        
        std::string ep_dir = join_path(interface_dir, name);
        if (!is_directory(ep_dir)) continue;
        
        std::string direction = read_file(join_path(ep_dir, "direction"));
        for (auto& c : direction) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (direction != "in") continue;
        
        std::string type = read_file(join_path(ep_dir, "type"));
        for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (type != "interrupt") continue;
        
        int addr = endpoint_address_from_name(name);
        if (addr < 0) addr = 0xFF;
        
        EndpointData data;
        data.address = addr;
        data.interval = read_file(join_path(ep_dir, "interval"));
        data.binterval = read_file(join_path(ep_dir, "bInterval"));
        data.max_packet = read_file(join_path(ep_dir, "wMaxPacketSize"));
        data.type = type;
        
        if (addr < best_addr) {
            best_addr = addr;
            best_name = ep_dir;
            best_data = data;
        }
    }
    closedir(dir);
    
    if (best_name.empty()) return {"", empty};
    return {best_name, best_data};
}

std::optional<double> configured_max_hz(
    std::optional<double> speed_mbps,
    std::optional<double> interval_ms,
    std::optional<int64_t> binterval)
{
    return configured_max_hz_impl(speed_mbps, interval_ms, binterval);
}

std::optional<UsbInfo> find_usb_info(const std::string& event_path, const std::string& sysfs_root) {
    using namespace std;
    
    string event_name;
    {
        size_t pos = event_path.find_last_of("/");
        event_name = (pos == string::npos) ? event_path : event_path.substr(pos + 1);
    }
    
    string device_dir = join_path(join_path(join_path(sysfs_root, "class"), "input"), join_path(event_name, "device"));
    
    string current = device_dir;
    // Resolve symlinks
    char resolved[4096];
    if (realpath(current.c_str(), resolved)) {
        current = resolved;
    }
    
    if (!is_directory(current)) return std::nullopt;
    
    string usb_dir;
    string interface_dir;
    
    string walk = current;
    while (true) {
        if (is_file(join_path(walk, "idVendor")) && is_file(join_path(walk, "idProduct"))) {
            usb_dir = walk;
            break;
        }
        if (interface_dir.empty() && is_file(join_path(walk, "bInterfaceNumber"))) {
            interface_dir = walk;
        }
        string parent = walk;
        size_t slash = parent.find_last_of("/");
        if (slash == string::npos || slash == 0) break;
        parent = parent.substr(0, slash);
        if (parent == walk) break;
        walk = parent;
    }
    
    if (usb_dir.empty()) return std::nullopt;
    
    UsbInfo info;
    info.vendor_id = read_file(join_path(usb_dir, "idVendor"));
    info.product_id = read_file(join_path(usb_dir, "idProduct"));
    info.manufacturer = read_file(join_path(usb_dir, "manufacturer"));
    info.product_name = read_file(join_path(usb_dir, "product"));
    info.serial = read_file(join_path(usb_dir, "serial"));
    info.speed_mbps = parse_double(read_file(join_path(usb_dir, "speed")));
    info.busnum = parse_int(read_file(join_path(usb_dir, "busnum")));
    info.devnum = parse_int(read_file(join_path(usb_dir, "devnum")));
    info.sysfs_path = usb_dir;
    
    if (!interface_dir.empty()) {
        info.interface_number = parse_int(read_file(join_path(interface_dir, "bInterfaceNumber")));
        info.interface_class = parse_int(read_file(join_path(interface_dir, "bInterfaceClass")));
        info.interface_protocol = parse_int(read_file(join_path(interface_dir, "bInterfaceProtocol")));
        
        auto [ep_dir, ep_data] = find_interrupt_in_endpoint(interface_dir);
        if (ep_data.address) {
            info.endpoint_address = *ep_data.address;
        }
        info.endpoint_type = ep_data.type;
        info.endpoint_interval_ms = parse_interval_ms(ep_data.interval);
        info.endpoint_binterval = parse_int(ep_data.binterval, 16);
        info.endpoint_max_packet = parse_int(ep_data.max_packet, 16);
    }
    
    info.configured_max_hz = configured_max_hz_impl(info.speed_mbps, info.endpoint_interval_ms, info.endpoint_binterval);
    
    return info;
}

} // namespace keyboard_inspector
