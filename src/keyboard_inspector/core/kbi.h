#pragma once

#include <string>
#include <cstdint>
#include <optional>

#include "model.h"

namespace keyboard_inspector {

class KbiError : public std::exception {
public:
    explicit KbiError(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

Recording read_kbi_file(const std::string& path);
void write_kbi_file(const Recording& recording, const std::string& path);
std::string kbi_dumps(const Recording& recording);
Recording kbi_loads(const std::string& data);

// Binary format constants
constexpr char KBI_HEADER[4] = {'K', 'B', 'I', '\0'};
constexpr uint32_t KBI_FILE_VERSION = 3;

} // namespace keyboard_inspector
