#include "base.h"

#include <cstring>
#include <sstream>

namespace keyboard_inspector {

int64_t stable_source_id(const std::vector<std::string>& parts) {
    // Simple hash: FNV-1a 64-bit
    std::string identity;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) identity += "|";
        identity += parts[i];
    }
    
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : identity) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    int64_t value = static_cast<int64_t>(hash);
    return value == 0 ? 1 : value;
}

} // namespace keyboard_inspector
