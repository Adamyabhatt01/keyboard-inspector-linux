#pragma once

#include <string>
#include <sstream>
#include <fstream>
#include <mutex>
#include <memory>
#include <iostream>

namespace keyboard_inspector {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
    Fatal = 4
};

const char* log_level_to_string(LogLevel level);
LogLevel log_level_from_string(const std::string& str);

class Logger {
public:
    static Logger& instance();

    void init(LogLevel console_level = LogLevel::Info,
              LogLevel file_level = LogLevel::Debug,
              const std::string& log_file_path = "");

    void set_console_level(LogLevel level);
    void set_file_level(LogLevel level);
    void set_console_enabled(bool enabled);
    void set_log_file(const std::string& path);

    LogLevel console_level() const { return console_level_; }
    LogLevel file_level() const { return file_level_; }
    std::string log_file_path() const { return log_file_path_; }

    void log(LogLevel level, const std::string& module_name, const std::string& message);

    void install_crash_handler();
    void install_qt_handler();

    void flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string resolve_default_log_path() const;

    LogLevel console_level_{LogLevel::Info};
    LogLevel file_level_{LogLevel::Debug};
    bool console_enabled_{true};
    std::string log_file_path_;
    std::ofstream file_stream_;
    std::mutex mutex_;
    bool crash_handler_installed_{false};
    bool qt_handler_installed_{false};
};

class LogStream {
public:
    LogStream(LogLevel level, const std::string& module_name)
        : level_(level), module_(module_name) {}

    ~LogStream() {
        Logger::instance().log(level_, module_, ss_.str());
    }

    template <typename T>
    LogStream& operator<<(const T& val) {
        ss_ << val;
        return *this;
    }

private:
    LogLevel level_;
    std::string module_;
    std::ostringstream ss_;
};

#define LOG_DEBUG(module) ::keyboard_inspector::LogStream(::keyboard_inspector::LogLevel::Debug, module)
#define LOG_INFO(module)  ::keyboard_inspector::LogStream(::keyboard_inspector::LogLevel::Info, module)
#define LOG_WARN(module)  ::keyboard_inspector::LogStream(::keyboard_inspector::LogLevel::Warn, module)
#define LOG_ERROR(module) ::keyboard_inspector::LogStream(::keyboard_inspector::LogLevel::Error, module)
#define LOG_FATAL(module) ::keyboard_inspector::LogStream(::keyboard_inspector::LogLevel::Fatal, module)

} // namespace keyboard_inspector
