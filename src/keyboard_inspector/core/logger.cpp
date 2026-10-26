#include "logger.h"

#include <chrono>
#include <iomanip>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <csignal>
#include <execinfo.h>
#include <cxxabi.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <QtGlobal>
#include <QString>

namespace keyboard_inspector {

namespace {

const char* color_code(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[36m";  // Cyan
        case LogLevel::Info:  return "\033[32m";  // Green
        case LogLevel::Warn:  return "\033[33m";  // Yellow
        case LogLevel::Error: return "\033[31m";  // Red
        case LogLevel::Fatal: return "\033[35;1m";// Bright Magenta
        default: return "";
    }
}

const char* color_reset() {
    return "\033[0m";
}

bool is_tty(FILE* stream) {
    return isatty(fileno(stream)) != 0;
}

void terminate_handler() {
    try {
        LOG_FATAL("Crash") << "=== FATAL: std::terminate called ===";
        void* buffer[128];
        int n = backtrace(buffer, 128);
        char** symbols = backtrace_symbols(buffer, n);
        if (symbols) {
            for (int i = 0; i < n; ++i) {
                char* mangled = nullptr;
                char* offset = nullptr;
                char* end = nullptr;
                for (char* p = symbols[i]; *p; ++p) {
                    if (*p == '(') mangled = p + 1;
                    else if (*p == '+') offset = p;
                    else if (*p == ')') { end = p; break; }
                }
                if (mangled && offset && end && mangled < offset) {
                    *offset = '\0';
                    int status = 0;
                    char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
                    *offset = '+';
                    if (status == 0 && demangled) {
                        LOG_FATAL("Crash") << "  [" << i << "] " << demangled;
                        free(demangled);
                        continue;
                    }
                }
                LOG_FATAL("Crash") << "  [" << i << "] " << symbols[i];
            }
            free(symbols);
        }
        Logger::instance().flush();
    } catch (...) {
        const char fallback[] = "=== FATAL: std::terminate (logging failed) ===\n";
        (void)!::write(STDERR_FILENO, fallback, sizeof(fallback) - 1);
    }
    std::abort();
}

void signal_handler(int sig) {
    // Only async-signal-safe calls here: taking the logger mutex or allocating
    // can deadlock when the fault happened inside logging, and a second fault
    // inside the handler would abort without producing a useful core.
    const char prefix[] = "\n=== FATAL: signal ";
    const char suffix[] = " - terminating (core dump follows) ===\n";
    char digits[16];
    int count = 0;
    int value = sig < 0 ? -sig : sig;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value > 0);

    (void)!::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    for (int i = count - 1; i >= 0; --i) {
        (void)!::write(STDERR_FILENO, &digits[i], 1);
    }
    (void)!::write(STDERR_FILENO, suffix, sizeof(suffix) - 1);

    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void qt_message_router(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    // Never let a logging allocation/unwind escape back into Qt's message
    // dispatch (it runs from destructors and platform callbacks).
    try {
        LogLevel level = LogLevel::Debug;
        switch (type) {
            case QtDebugMsg:    level = LogLevel::Debug; break;
            case QtInfoMsg:     level = LogLevel::Info;  break;
            case QtWarningMsg:  level = LogLevel::Warn;  break;
            case QtCriticalMsg: level = LogLevel::Error; break;
            case QtFatalMsg:    level = LogLevel::Fatal; break;
        }
        std::string module = "Qt";
        if (context.category && std::strlen(context.category) > 0 && std::strcmp(context.category, "default") != 0) {
            module = context.category;
        }
        Logger::instance().log(level, module, msg.toStdString());
    } catch (...) {
        const char fallback[] = "[Qt] message suppressed (logging failure)\n";
        (void)!::write(STDERR_FILENO, fallback, sizeof(fallback) - 1);
    }
}

} // namespace

const char* log_level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "UNK  ";
    }
}

LogLevel log_level_from_string(const std::string& str) {
    std::string s = str;
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "debug" || s == "dbg" || s == "0") return LogLevel::Debug;
    if (s == "info"  || s == "1") return LogLevel::Info;
    if (s == "warn"  || s == "warning" || s == "2") return LogLevel::Warn;
    if (s == "error" || s == "err" || s == "3") return LogLevel::Error;
    if (s == "fatal" || s == "4") return LogLevel::Fatal;
    return LogLevel::Info;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    flush();
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

std::string Logger::resolve_default_log_path() const {
    const char* env_path = std::getenv("KEYBOARD_INSPECTOR_LOG_FILE");
    if (env_path && *env_path) {
        return env_path;
    }

    const char* xdg_state = std::getenv("XDG_STATE_HOME");
    std::filesystem::path base_dir;
    if (xdg_state && *xdg_state) {
        base_dir = std::filesystem::path(xdg_state) / "keyboard-inspector";
    } else {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            base_dir = std::filesystem::path(home) / ".local" / "state" / "keyboard-inspector";
        } else {
            base_dir = "/tmp";
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(base_dir, ec);
    if (!ec) {
        return (base_dir / "keyboard-inspector.log").string();
    }
    return "/tmp/keyboard-inspector.log";
}

void Logger::init(LogLevel console_level, LogLevel file_level, const std::string& log_file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_level_ = console_level;
    file_level_ = file_level;

    std::string target_path = log_file_path.empty() ? resolve_default_log_path() : log_file_path;
    set_log_file(target_path);
}

void Logger::set_console_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_level_ = level;
}

void Logger::set_file_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_level_ = level;
}

void Logger::set_console_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_enabled_ = enabled;
}

void Logger::set_log_file(const std::string& path) {
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
    log_file_path_ = path;
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        file_stream_.open(path, std::ios::out | std::ios::app);
        if (!file_stream_) {
            std::cerr << "Warning: could not open log file " << path << std::endl;
        }
    }
}

void Logger::install_crash_handler() {
    if (crash_handler_installed_) return;
    std::set_terminate(terminate_handler);
    std::signal(SIGSEGV, signal_handler);
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGBUS, signal_handler);
    std::signal(SIGFPE, signal_handler);
    crash_handler_installed_ = true;
}

void Logger::install_qt_handler() {
    if (qt_handler_installed_) return;
    qInstallMessageHandler(qt_message_router);
    qt_handler_installed_ = true;
}

void Logger::log(LogLevel level, const std::string& module_name, const std::string& message) {
    // Guard against re-entry on the same thread: mutex_ is not recursive, and
    // a Qt message emitted from inside logging would otherwise deadlock.
    thread_local bool in_log = false;
    if (in_log) return;
    in_log = true;
    struct ResetFlag {
        bool& flag;
        ~ResetFlag() { flag = false; }
    } reset{in_log};

    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&timer, &tm_buf);

    std::ostringstream header;
    header << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
           << "[" << log_level_to_string(level) << "] "
           << "[" << module_name << "] ";

    std::string header_str = header.str();

    std::lock_guard<std::mutex> lock(mutex_);

    // File output
    if (file_stream_.is_open() && level >= file_level_) {
        file_stream_ << header_str << message << "\n";
        file_stream_.flush();
    }

    // Console output
    if (console_enabled_ && level >= console_level_) {
        FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
        if (is_tty(out)) {
            std::string colored_lvl = std::string(color_code(level)) + log_level_to_string(level) + color_reset();
            std::ostringstream colored_header;
            colored_header << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
                           << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
                           << "[" << colored_lvl << "] "
                           << "[\033[1m" << module_name << "\033[0m] ";
            std::fputs(colored_header.str().c_str(), out);
            std::fputs(message.c_str(), out);
            std::fputc('\n', out);
        } else {
            std::fputs(header_str.c_str(), out);
            std::fputs(message.c_str(), out);
            std::fputc('\n', out);
        }
        std::fflush(out);
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
    }
    std::fflush(stdout);
    std::fflush(stderr);
}

} // namespace keyboard_inspector
