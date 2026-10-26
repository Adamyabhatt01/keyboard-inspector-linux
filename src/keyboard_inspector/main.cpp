#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QPixmap>
#include <iostream>
#include <string>
#include <memory>

#include "core/logger.h"
#include "ui/main_window.h"
#include "ui/theme.h"

namespace {

std::pair<int, int> parse_size(const std::string& text) {
    auto pos = text.find('x');
    if (pos == std::string::npos) {
        throw std::runtime_error("expected WIDTHxHEIGHT, e.g. 1294x836");
    }
    int w = std::stoi(text.substr(0, pos));
    int h = std::stoi(text.substr(pos + 1));
    if (w < 320 || h < 240) {
        throw std::runtime_error("window size is too small");
    }
    return {w, h};
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace keyboard_inspector;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--list-devices") {
            auto backend = std::make_unique<keyboard_inspector::EvdevBackend>();
            auto devices = backend->list_devices();
            std::cout << "Linux input devices (evdev):\n";
            if (devices.empty()) {
                std::cout << "  none found under /dev/input\n";
            }
            for (const auto& d : devices) {
                std::string state = d.accessible ? "readable" : "locked";
                std::string category = d.kind == "keyboard" ? "" : " (" + d.kind + ")";
                std::cout << "  [" << state << "] " << (d.name.empty() ? "(unnamed)" : d.name) << category << "\n";
                std::cout << "             path: " << d.path << "\n";
                if (d.usb) {
                    std::cout << "             USB: " << d.usb->summary() << "\n";
                }
            }
            bool any_readable = false;
            for (const auto& d : devices) {
                if (d.accessible && d.capture_supported()) { any_readable = true; break; }
            }
            if (!any_readable) {
                std::cout << "\nNo readable input device. Install the udev rule or add your user to the input group.\n";
                std::cout << "Do not run the GUI as root.\n";
            }
            return 0;
        }
    }

    auto& logger = Logger::instance();
    logger.install_crash_handler();
    logger.install_qt_handler();

    QApplication app(argc, argv);
    app.setApplicationName(APP_NAME);
    app.setApplicationDisplayName(APP_NAME);
    app.setOrganizationName("KeyboardInspector");

    keyboard_inspector::Theme::apply_theme(&app);

    QCommandLineParser parser;
    parser.setApplicationDescription("Measure keyboard event timing and estimate the report rate.");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOption(QCommandLineOption(QStringList{"list-devices"}, "List input devices and exit"));
    parser.addOption(QCommandLineOption(QStringList{"open", "o"}, "Open a .kbi file on startup", "PATH"));
    parser.addOption(QCommandLineOption(QStringList{"screenshot"}, "Render once, save PNG, exit", "PNG"));
    parser.addOption(QCommandLineOption(QStringList{"window-size"}, "Window size for screenshot (default 1294x836)", "WxH", "1294x836"));
    parser.addOption(QCommandLineOption(QStringList{"d", "debug", "verbose"}, "Enable verbose debug logging to console"));
    parser.addOption(QCommandLineOption(QStringList{"log-file"}, "Specify custom path for debug log file", "PATH"));
    parser.addOption(QCommandLineOption(QStringList{"q", "quiet"}, "Disable console log output"));
    parser.addPositionalArgument("file", "Recording or file to open");

    parser.process(app);

    LogLevel console_lvl = LogLevel::Info;
    if (parser.isSet("debug")) {
        console_lvl = LogLevel::Debug;
    }
    std::string log_file = parser.isSet("log-file") ? parser.value("log-file").toStdString() : "";
    logger.init(console_lvl, LogLevel::Debug, log_file);
    if (parser.isSet("quiet")) {
        logger.set_console_enabled(false);
    }

    LOG_INFO("Main") << APP_NAME << " " << APP_VERSION << " starting...";
    LOG_INFO("Main") << "Qt platform: " << QGuiApplication::platformName().toStdString();
    LOG_INFO("Main") << "Log file: " << logger.log_file_path();
#ifndef USE_KISSFFT
    LOG_WARN("Main") << "kissfft is not available; the FFT pipeline falls back to a "
                        "naive O(n^2) DFT. Analysis of long recordings will be very slow. "
                        "Install kissfft and rebuild for full-speed analysis.";
#endif

    keyboard_inspector::MainWindow window;
    window.resize(1294, 836);
    window.setMinimumSize(720, 500);

    QString open_target;
    if (parser.isSet("open")) {
        open_target = parser.value("open");
    } else if (!parser.positionalArguments().isEmpty()) {
        open_target = parser.positionalArguments().first();
    }

    if (!open_target.isEmpty()) {
        window.open_path(open_target);
    }

    if (parser.isSet("screenshot")) {
        window.set_screenshot_mode(true);
        auto target = parser.value("screenshot");
        auto size_opt = parser.value("window-size");
        try {
            auto [w, h] = parse_size(size_opt.toStdString());
            window.resize(w, h);
        } catch (...) {
            window.resize(1294, 836);
        }

        window.show();

        // Wait long enough for the initial async analysis to land (the GUI
        // applies finished results on a poll timer) before grabbing.
        QTimer::singleShot(1200, [&window, target, &app]() {
            window.repaint();
            QPixmap pixmap = window.grab();
            if (!pixmap.save(target, "PNG")) {
                std::cerr << "error: could not write " << target.toStdString() << "\n";
                app.exit(1);
                return;
            }
            std::cout << "saved " << pixmap.width() << "x" << pixmap.height()
                      << " screenshot to " << target.toStdString() << "\n";
            app.quit();
        });
        return app.exec();
    }

    window.show();
    return app.exec();
}
