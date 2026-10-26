#include "main_window.h"
#include "../core/logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QPainter>
#include <QFontMetrics>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <QTimer>
#include <QStyle>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <cmath>
#include <iostream>
#include <sstream>

namespace keyboard_inspector {

namespace {

QString format_hz(double value) {
    double tolerance = std::max(0.05, std::abs(value) * 0.0005);
    if (std::abs(value - std::round(value)) <= tolerance) {
        return QString::number(static_cast<long long>(std::round(value)));
    }
    QString text = QString::number(value, 'f', 3);
    text = text.remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"));
    return text.isEmpty() ? "0" : text;
}

} // namespace

// ---------- RateReadout ----------

RateReadout::RateReadout(QWidget* parent) : QWidget(parent) {
    _main = "No recording";
    _sub = "Select devices under Capture, then press Start Recording.";
    _main_color = Theme::TEXT;
    setMinimumWidth(160);
    setMaximumWidth(620);
    setFixedHeight(28);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName("Estimated report rate");
}

void RateReadout::set_state(const QString& main, const QString& sub,
                            const QString& tooltip, const QColor* color) {
    _main = main;
    _sub = sub;
    _main_color = color ? *color : Theme::TEXT;
    setToolTip(tooltip);
    update();
}

void RateReadout::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    QFont main_font = font();
    main_font.setPointSizeF(10.0);
    QFont sub_font = font();
    sub_font.setPointSizeF(7.5);

    QFontMetrics main_metrics(main_font);
    QFontMetrics sub_metrics(sub_font);
    QString main = main_metrics.elidedText(_main, Qt::ElideRight, std::max(0, width() - 8));
    QString sub = sub_metrics.elidedText(_sub, Qt::ElideRight, std::max(0, width() - 8));

    painter.setFont(main_font);
    painter.setPen(_main_color);
    painter.drawText(QRect(4, 1, width() - 8, 16),
                     Qt::AlignHCenter | Qt::AlignVCenter, main);
    painter.setFont(sub_font);
    painter.setPen(Theme::TEXT_SECONDARY);
    painter.drawText(QRect(4, 16, width() - 8, 11),
                     Qt::AlignHCenter | Qt::AlignVCenter, sub);
}

// ---------- Banner ----------

Banner::Banner(QWidget* parent) : QFrame(parent) {
    setObjectName("banner");
    setMaximumHeight(32);
    _timer.setSingleShot(true);
    _copy_reset_timer.setSingleShot(true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 3, 8, 3);
    layout->setSpacing(8);

    label = new QLabel("");
    label->setObjectName("bannerText");
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(label, 1);

    copy_button = new QPushButton("Copy command");
    copy_button->setFixedHeight(20);
    layout->addWidget(copy_button);

    retry_button = new QPushButton("Retry");
    retry_button->setFixedHeight(20);
    layout->addWidget(retry_button);

    dismiss_button = new QPushButton("Dismiss");
    dismiss_button->setFixedHeight(20);
    layout->addWidget(dismiss_button);

    connect(copy_button, &QPushButton::clicked, this, &Banner::_copy);
    connect(retry_button, &QPushButton::clicked, this, &Banner::_retry);
    connect(dismiss_button, &QPushButton::clicked, this, &Banner::hide_message);
    connect(&_copy_reset_timer, &QTimer::timeout,
            [this](){ copy_button->setText("Copy command"); });
    connect(&_timer, &QTimer::timeout, this, &Banner::hide_message);

    hide();
}

void Banner::show_message(const QString& text, const QString& kind,
                          const QString& copy_text, bool retry,
                          std::optional<int> timeout_ms) {
    _kind = kind;
    label->setText(text);
    label->setToolTip(text);
    _copy_text = copy_text;
    copy_button->setVisible(!copy_text.isEmpty());
    copy_button->setText("Copy command");
    retry_button->setVisible(retry);
    dismiss_button->setVisible(true);
    _timer.stop();
    if (timeout_ms) {
        _timer.start(*timeout_ms);
    }
    show();
}

void Banner::hide_message() {
    _timer.stop();
    _kind = "";
    hide();
}

void Banner::_copy() {
    if (_copy_text.isEmpty()) return;
    QApplication::clipboard()->setText(_copy_text);
    copy_button->setText("Copied");
    _copy_reset_timer.start(1500);
}

void Banner::_retry() {
    if (retry_callback) retry_callback();
}

// ---------- MainWindow ----------

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent),
    _settings_store{"KeyboardInspector", "KeyboardInspectorLinux"} {

    _capture_timer.setInterval(20);
    connect(&_capture_timer, &QTimer::timeout, this, &MainWindow::_on_capture_tick);

    _tentative_timer.setInterval(750);
    connect(&_tentative_timer, &QTimer::timeout, this, &MainWindow::_run_tentative_analysis);

    _bin_debounce.setSingleShot(true);
    _bin_debounce.setInterval(150);
    connect(&_bin_debounce, &QTimer::timeout, this, &MainWindow::_apply_bin_rate);

    _hotplug_poll.setInterval(3000);
    connect(&_hotplug_poll, &QTimer::timeout, this, &MainWindow::_poll_hotplug);
    _hotplug_poll.start();

    _hotplug_debounce.setSingleShot(true);
    _hotplug_debounce.setInterval(350);
    connect(&_hotplug_debounce, &QTimer::timeout, this, &MainWindow::_refresh_from_hotplug);

    _hotplug_settle.setSingleShot(true);
    _hotplug_settle.setInterval(1600);
    connect(&_hotplug_settle, &QTimer::timeout, this, &MainWindow::_refresh_from_hotplug);

    _analysis_watcher = new QFutureWatcher<AnalysisResult>(this);
    connect(_analysis_watcher, &QFutureWatcher<AnalysisResult>::finished,
            this, &MainWindow::_on_analysis_finished);

    _settings = AnalysisSettings().validated();

    _build_ui();
    resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);
    setMinimumSize(MIN_WIDTH, MIN_HEIGHT);
    setAcceptDrops(true);
    // Restore the previous window layout (upstream remembers it through the
    // .NET Application settings). Skipped in screenshot/test modes so those
    // stay deterministic.
    if (!_screenshot_mode && QGuiApplication::platformName() != "offscreen") {
        const QByteArray geometry = _settings_store.value("window/geometry").toByteArray();
        if (!geometry.isEmpty()) restoreGeometry(geometry);
        const QByteArray splitter_state = _settings_store.value("window/splitter").toByteArray();
        if (!splitter_state.isEmpty() && splitter) splitter->restoreState(splitter_state);
    }
    _refresh_devices();

    _show_empty();
    _update_title();
    _update_controls();

    QTimer::singleShot(0, this, &MainWindow::_clamp_to_screen);
    // Upstream performs a one-shot update check on startup.
    QTimer::singleShot(2500, this, [this]() {
        if (!_screenshot_mode) _check_for_updates();
    });
}

MainWindow::~MainWindow() {
    if (_download_reply) {
        _download_reply->abort();
        _download_reply = nullptr;
    }
    if (_analysis_watcher) {
        _analysis_watcher->disconnect(this);
        if (_analysis_watcher->isRunning()) {
            _analysis_watcher->cancel();
            _analysis_watcher->waitForFinished();
        }
        delete _analysis_watcher;
        _analysis_watcher = nullptr;
    }
    _stop_if_running();
    _capture_timer.stop();
    _tentative_timer.stop();
    _hotplug_poll.stop();
    _hotplug_debounce.stop();
    _hotplug_settle.stop();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (_state == "recording") {
        event->ignore();
        _show_banner("Stop the recording before closing the window.", "warning", QString(), false, 4000);
        return;
    }
    if (!_screenshot_mode && QGuiApplication::platformName() != "offscreen") {
        _settings_store.setValue("window/geometry", saveGeometry());
        if (splitter) _settings_store.setValue("window/splitter", splitter->saveState());
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (partials_label) {
        bool wide = width() >= 860;
        partials_label->setVisible(wide);
        hps_label->setText(wide ? "HPS Partial Elimination:" : "HPS:");
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (_state == "recording") return;
    auto* mime = event->mimeData();
    if (!mime->hasUrls()) return;
    auto urls = mime->urls();
    if (urls.size() != 1 || !urls[0].isLocalFile()) return;
    if (urls[0].toLocalFile().toLower().endsWith(".kbi") ||
        urls[0].toLocalFile().toLower().endsWith(".ttr") ||
        urls[0].toLocalFile().toLower().endsWith(".ttrm")) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls[0].isLocalFile()) {
        _load_path(urls[0].toLocalFile());
        event->acceptProposedAction();
    }
}

void MainWindow::_build_ui() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    setCentralWidget(central);

    top_bar = _build_top_bar();
    root->addWidget(top_bar);

    banner = new Banner(central);
    banner->retry_callback = [this](){ _retry_devices(); };
    root->addWidget(banner);

    splitter = new QSplitter(Qt::Vertical, central);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(5);

    QWidget* charts_container = new QWidget(splitter);
    charts_layout = new QVBoxLayout(charts_container);
    charts_layout->setContentsMargins(12, 4, 12, 0);
    charts_layout->setSpacing(3);

    chart_grid = new QGridLayout();
    chart_grid->setContentsMargins(0, 0, 0, 0);
    chart_grid->setSpacing(3);
    chart_grid->setColumnStretch(0, 1);
    chart_grid->setColumnStretch(1, 1);
    chart_grid->setColumnStretch(2, 1);
    chart_grid->setRowStretch(0, 1);
    chart_grid->setRowStretch(1, 1);

    QStringList titles = {
        "Differences between consecutive events",
        "Differences between all events",
        "Events wrapped around a second",
    };

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            QString title = titles[col] + QString(" (%1)").arg(row == 0 ? "time domain" : "frequency domain");
            auto* chart = new ChartWidget(
                title,
                row == 0 ? Theme::TIME_LINE : Theme::FREQ_LINE,
                row == 0 ? 32.0 : 0.0
            );
            // Upstream MainForm.cs:189-205: time charts use 32 ms default
            // scope, 3 micro decimals and 6-decimal hover coordinates;
            // frequency charts use "0.###" hover formatting.
            if (row == 0) {
                chart->x_axis_micro_decimals = 3;
                chart->highlight_x_decimals = 6;
            } else {
                chart->highlight_x_decimals = -1;
            }
            chart->spotlight = [this](ChartWidget* c){ _handle_spotlight(c); };
            chart_grid->addWidget(chart, row, col);
            charts.push_back(chart);
        }
    }

    charts_layout->addLayout(chart_grid, 1);

    analysis_strip = _build_analysis_strip(charts_container);
    charts_layout->addWidget(analysis_strip);

    splitter->addWidget(charts_container);

    auto* timeline_container = new QWidget(splitter);
    auto* timeline_layout = new QVBoxLayout(timeline_container);
    timeline_layout->setContentsMargins(12, 0, 12, 8);
    timeline_layout->setSpacing(0);
    timeline = new TimelineWidget(timeline_container);
    connect(timeline, &TimelineWidget::analysis_requested, this, &MainWindow::_on_visibility_changed);
    connect(timeline, &TimelineWidget::changed, this, &MainWindow::_on_timeline_changed);
    connect(timeline, &TimelineWidget::message, this, [this](const QString& text){
        _show_banner(text, "info", QString(), false, 4000);
    });
    timeline_layout->addWidget(timeline);
    splitter->addWidget(timeline_container);

    splitter->setStretchFactor(0, 68);
    splitter->setStretchFactor(1, 32);
    splitter->setSizes({553, 259});

    root->addWidget(splitter, 1);

    setWindowTitle(QString(APP_NAME) + " " + QString(APP_VERSION));

    Theme::apply_theme(qApp);
}

QFrame* MainWindow::_build_top_bar() {
    auto* bar = new QFrame(this);
    bar->setObjectName("topBar");
    bar->setFixedHeight(30);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 1, 6, 1);
    layout->setSpacing(4);

    file_button = new QToolButton(bar);
    file_button->setText("File");
    file_button->setPopupMode(QToolButton::InstantPopup);
    auto* file_menu = new QMenu(file_button);
    file_button->setMenu(file_menu);
    connect(file_menu, &QMenu::aboutToShow, this,
            [this, file_menu]() { _fit_menu_to_screen(file_menu); });

    auto* open_action = file_menu->addAction("Open…");
    open_action->setShortcut(QKeySequence(QString("Ctrl+O")));
    connect(open_action, &QAction::triggered, this, &MainWindow::_open_dialog);

    auto* save_action = file_menu->addAction("Save As…");
    save_action->setShortcut(QKeySequence(QString("Ctrl+S")));
    connect(save_action, &QAction::triggered, this, &MainWindow::_save_dialog);
    this->save_action = save_action;

    auto* import_action = file_menu->addAction("Import from URL…");
    import_action->setShortcut(QKeySequence(QString("Ctrl+Shift+O")));
    connect(import_action, &QAction::triggered, this, &MainWindow::_import_url_dialog);

    file_menu->addSeparator();

    auto* quit_action = file_menu->addAction("Quit");
    quit_action->setShortcut(QKeySequence(QString("Ctrl+Q")));
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    layout->addWidget(file_button);

    help_button = new QToolButton(bar);
    help_button->setText("Help");
    help_button->setPopupMode(QToolButton::InstantPopup);
    auto* help_menu = new QMenu(help_button);
    help_button->setMenu(help_menu);
    connect(help_menu, &QMenu::aboutToShow, this,
            [this, help_menu]() { _fit_menu_to_screen(help_menu); });

    auto* docs_action = help_menu->addAction("Official Documentation");
    connect(docs_action, &QAction::triggered, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/mat1jaczyyy/Keyboard-Inspector/blob/master/DOCUMENTATION.md"));
    });

    auto* updates_action = help_menu->addAction("Check for Updates");
    connect(updates_action, &QAction::triggered, [this]() {
        _check_for_updates();
        QDesktopServices::openUrl(QUrl("https://github.com/mat1jaczyyy/Keyboard-Inspector/releases"));
    });

    auto* discord_action = help_menu->addAction("Discord");
    connect(discord_action, &QAction::triggered, []() {
        QDesktopServices::openUrl(QUrl("https://discord.gg/kX4cJQH5Zn"));
    });

    auto* donate_action = help_menu->addAction("Donate");
    connect(donate_action, &QAction::triggered, []() {
        QDesktopServices::openUrl(QUrl("https://ko-fi.com/mat1jaczyyy"));
    });

    help_menu->addSeparator();
    help_menu->addAction("About", [this](){
        AboutDialog dialog(this);
        dialog.exec();
    });
    layout->addWidget(help_button);

    // Hidden until an update is detected; then it moves out of the Help menu
    // to the top bar like the upstream releases notification.
    update_button = new QToolButton(bar);
    update_button->setText("Update available!");
    update_button->setObjectName("updateButton");
    update_button->hide();
    connect(update_button, &QToolButton::clicked, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/mat1jaczyyy/Keyboard-Inspector/releases/latest"));
    });
    layout->addWidget(update_button);

    layout->addStretch(1);

    rate_readout = new RateReadout(bar);
    layout->addWidget(rate_readout, 3);
    layout->addStretch(1);

    capture_button = new QToolButton(bar);
    capture_button->setText("Capture");
    capture_button->setPopupMode(QToolButton::InstantPopup);
    capture_menu = new QMenu(capture_button);
    capture_button->setMenu(capture_menu);

    devices_menu = capture_menu->addMenu("Devices");

    rescan_action = capture_menu->addAction("Rescan devices");
    connect(rescan_action, &QAction::triggered, this, &MainWindow::_retry_devices);

    capture_menu->addSeparator();

    // Upstream device-kind capture toggles (MainForm menu, CheckOnClick).
    capture_keyboard_action = capture_menu->addAction("Keyboard");
    capture_keyboard_action->setCheckable(true);
    capture_keyboard_action->setChecked(true);
    capture_gamepad_action = capture_menu->addAction("Gamepad/Joystick");
    capture_gamepad_action->setCheckable(true);
    capture_gamepad_action->setChecked(true);
    capture_mouse_action = capture_menu->addAction("Mouse");
    capture_mouse_action->setCheckable(true);
    capture_mouse_action->setChecked(false);
    connect(capture_keyboard_action, &QAction::toggled, this, [this](bool) { _on_capture_kind_toggled(); });
    connect(capture_gamepad_action, &QAction::toggled, this, [this](bool) { _on_capture_kind_toggled(); });
    connect(capture_mouse_action, &QAction::toggled, this, [this](bool) { _on_capture_kind_toggled(); });

    capture_menu->addSeparator();

    // Kernel auto-repeat diagnostics (EV_KEY value 2 is dropped, never
    // recorded). Informational only.
    capture_diagnostics_action = capture_menu->addAction("Dropped auto-repeats: 0");
    capture_diagnostics_action->setEnabled(false);

    capture_menu->addSeparator();

    usb_summary_action = capture_menu->addAction("USB descriptor: unknown");
    usb_summary_action->setEnabled(false);

    connect(capture_menu, &QMenu::aboutToShow, this, &MainWindow::_update_usb_summary);

    // Adaptive dropdown sizing: keep every menu inside the current screen.
    // Long labels (device names, USB descriptor summary) are elided to a
    // screen-proportional width with the full text kept in the tooltip;
    // height is bounded by the scrollable-menu style hint (Theme), so an
    // oversized menu scrolls instead of overflowing off the screen.
    connect(capture_menu, &QMenu::aboutToShow, this,
            [this]() { _fit_menu_to_screen(capture_menu); });
    connect(devices_menu, &QMenu::aboutToShow, this,
            [this]() { _fit_menu_to_screen(devices_menu); });

    layout->addWidget(capture_button);

    freeze_check = new QCheckBox("Freeze", bar);
    freeze_check->setToolTip("Preserve visible inputs and colors for the next recording.");
    layout->addWidget(freeze_check);

    record_button = new QPushButton("Start Recording", bar);
    record_button->setObjectName("recordButton");
    record_button->setFixedSize(150, 26);
    connect(record_button, &QPushButton::clicked, this, &MainWindow::_record_clicked);
    layout->addWidget(record_button);

    return bar;
}

QFrame* MainWindow::_build_analysis_strip(QWidget* parent) {
    auto* strip = new QFrame(parent);
    strip->setObjectName("analysisStrip");
    strip->setFixedHeight(25);
    auto* layout = new QHBoxLayout(strip);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(6);

    auto* bin_label = new QLabel("Binning Rate:", strip);
    layout->addWidget(bin_label);

    bin_edit = new QLineEdit(strip);
    bin_edit->setFixedWidth(56);
    bin_edit->setAlignment(Qt::AlignRight);
    connect(bin_edit, &QLineEdit::textChanged, this, &MainWindow::_bin_text_changed);
    layout->addWidget(bin_edit);

    auto* hz_label = new QLabel("Hz", strip);
    layout->addWidget(hz_label);

    half_button = new QPushButton("/ 2", strip);
    half_button->setObjectName("smallButton");
    half_button->setFixedWidth(29);
    connect(half_button, &QPushButton::clicked, [this](){ _scale_bin_rate(0.5); });
    layout->addWidget(half_button);

    double_button = new QPushButton("* 2", strip);
    double_button->setObjectName("smallButton");
    double_button->setFixedWidth(29);
    connect(double_button, &QPushButton::clicked, [this](){ _scale_bin_rate(2.0); });
    layout->addWidget(double_button);

    low_cut = new QCheckBox("Low Cut Filter", strip);
    connect(low_cut, &QCheckBox::toggled, this, &MainWindow::_low_cut_toggled);
    layout->addWidget(low_cut);

    hps_label = new QLabel("HPS Partial Elimination:", strip);
    layout->addWidget(hps_label);

    hps_spin = new QSpinBox(strip);
    hps_spin->setRange(0, 9);
    hps_spin->setFixedWidth(71);
    hps_spin->setAlignment(Qt::AlignRight);
    hps_spin->setKeyboardTracking(false);
    connect(hps_spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::_hps_changed);
    layout->addWidget(hps_spin);

    partials_label = new QLabel("partials", strip);
    layout->addWidget(partials_label);

    layout->addStretch(1);

    event_count_label = new QLabel("", strip);
    event_count_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(event_count_label);

    _silent = true;
    bin_edit->setText(QString::number(_settings.bin_rate));
    low_cut->setChecked(_settings.low_cut);
    hps_spin->setValue(_settings.hps);
    _silent = false;

    return strip;
}

void MainWindow::_refresh_devices(bool from_hotplug) {
    auto backend = std::make_unique<EvdevBackend>();

    bool initialized = _devices_initialized;
    std::set<std::string> previous_accessible;
    for (const auto& device : _devices) {
        if (device.accessible && device.capture_supported()) {
            previous_accessible.insert(device.id);
        }
    }

    auto devices = backend->list_devices();

    std::set<std::string> accessible;
    for (const auto& device : devices) {
        if (device.accessible && device.capture_supported()) {
            accessible.insert(device.id);
        }
    }

    std::set<std::string> candidate;
    auto it = _selected_ids.find(_backend_kind);
    if (it != _selected_ids.end()) {
        for (const auto& id : it->second) {
            if (accessible.count(id)) candidate.insert(id);
        }
    }
    if (candidate.empty()) {
        candidate = default_device_selection(devices);
    }

    std::vector<DeviceInfo> newly_selected;
    if (initialized && _auto_select_new) {
        for (const auto& device : devices) {
            if (!accessible.count(device.id) || previous_accessible.count(device.id)) continue;
            if (device.kind != "keyboard" && device.kind != "gamepad") continue;
            if (candidate.insert(device.id).second) {
                newly_selected.push_back(device);
            }
        }
    }

    _devices = std::move(devices);
    _selected_ids[_backend_kind] = candidate;
    _devices_initialized = true;

    _populate_device_menu();
    _update_usb_summary();
    _update_record_enabled();
    _check_permission_banner();

    if (from_hotplug && initialized) {
        _announce_hotplug(_devices, newly_selected);
    }
}

void MainWindow::_populate_device_menu() {
    devices_menu->clear();
    auto selected = _selected_ids[_backend_kind];

    if (_devices.empty()) {
        auto* empty = devices_menu->addAction("No devices found");
        empty->setEnabled(false);
    }

    struct Group { std::string kind; QString title; };
    std::vector<Group> groups = {
        {"keyboard", "Keyboards"},
        {"gamepad", "Gamepads / joysticks"},
        {"mouse", "Mice"},
        {"other", "Other devices"},
    };

    for (const auto& g : groups) {
        std::vector<DeviceInfo> devs;
        for (const auto& d : _devices) {
            if (d.kind == g.kind) devs.push_back(d);
        }
        if (devs.empty()) continue;
        devices_menu->addSection(g.title);
        for (const auto& d : devs) {
            QString label = QString::fromStdString(d.display_name());
            if (!d.accessible) label += "  [locked]";

            // Elided to the screen width when the menu opens
            // (_fit_menu_to_screen); the full label stays in the tooltip.
            auto* action = new QAction(label, this);
            action->setProperty("fullText", label);
            action->setCheckable(true);
            action->setChecked(selected.count(d.id) > 0);
            action->setEnabled(d.accessible && d.capture_supported());

            QString tooltip = QString::fromStdString(d.display_name());
            if (!d.accessible) tooltip += "  [locked]";
            if (!d.error.empty()) tooltip += "\n" + QString::fromStdString(d.error);
            if (d.usb) {
                tooltip += "\n" + QString::fromStdString(d.usb->summary());
            }
            action->setToolTip(tooltip);

            QString device_id = QString::fromStdString(d.id);
            connect(action, &QAction::toggled,
                    [this, device_id](bool checked){ _device_toggled(device_id, checked); });
            devices_menu->addAction(action);
        }
    }

    devices_menu->addSeparator();
    auto* rescan = devices_menu->addAction("Rescan");
    connect(rescan, &QAction::triggered, this, &MainWindow::_retry_devices);

    auto* auto_select = devices_menu->addAction("Auto-select new keyboards/gamepads");
    auto_select->setCheckable(true);
    auto_select->setChecked(_auto_select_new);
    connect(auto_select, &QAction::toggled, this, &MainWindow::_set_auto_select_new);
}

void MainWindow::_update_usb_summary() {
    auto selected = _selected_ids[_backend_kind];
    std::optional<UsbInfo> info;
    for (const auto& d : _devices) {
        if (selected.count(d.id) && d.usb) {
            info = *d.usb;
            break;
        }
    }
    QString summary;
    if (info) {
        summary = QString("USB descriptor (configured, not measured): ") +
                  QString::fromStdString(info->summary());
    } else {
        summary = "USB descriptor: not available for the selected device";
    }
    usb_summary_action->setText(summary);
    usb_summary_action->setProperty("fullText", summary);
    usb_summary_action->setToolTip(summary);
}

// Keep a dropdown inside the current screen: elide long action texts to a
// screen-proportional width (full text stays in the tooltip), cap the menu
// width, and cap the height to the space below the anchor the menu opens
// from (the top-bar button, or the triggering item for a submenu). When the
// content is taller than that space the scrollable-menu style hint makes
// QMenu show scroll arrows instead of overflowing off the screen.
void MainWindow::_fit_menu_to_screen(QMenu* menu) {
    const QRect available = menu->screen()->availableGeometry();
    const QFontMetrics metrics(menu->font());
    const int max_text = qBound(180, available.width() * 2 / 5, 480);
    for (QAction* action : menu->actions()) {
        const QVariant full = action->property("fullText");
        if (full.isValid()) {
            action->setText(metrics.elidedText(full.toString(), Qt::ElideMiddle, max_text));
        }
    }
    menu->setMaximumWidth(std::max(240, available.width() - 16));

    QPoint anchor_bottom;
    if (QMenu* parent_menu = qobject_cast<QMenu*>(menu->parent())) {
        for (QAction* action : parent_menu->actions()) {
            if (action->menu() == menu) {
                anchor_bottom = parent_menu->mapToGlobal(
                    parent_menu->actionGeometry(action).bottomLeft());
                break;
            }
        }
    } else if (QWidget* button = qobject_cast<QWidget*>(menu->parent())) {
        anchor_bottom = button->mapToGlobal(QPoint(0, button->height()));
    }

    const int space_below = available.bottom() - anchor_bottom.y() - 4;
    // If the anchor sits near the bottom of the screen there is no usable
    // space below it; leave the height at screen size and let the
    // compositor flip the popup above the anchor.
    const int height_cap =
        space_below >= 160 ? space_below : available.height() - 24;
    menu->setMaximumHeight(height_cap);
}

void MainWindow::_retry_devices() {
    _permission_banner_shown = false;
    _refresh_devices();
    if (banner->kind() == "permission" || banner->kind() == "backend") {
        banner->hide_message();
    }
}

void MainWindow::_check_permission_banner() {
    auto selected = _selected_ids[_backend_kind];
    bool any_readable = false;
    bool selected_readable = false;
    for (const auto& d : _devices) {
        if (d.accessible && d.capture_supported()) {
            any_readable = true;
            if (selected.count(d.id)) selected_readable = true;
        }
    }

    if (selected_readable) {
        if (banner->kind() == "permission" || banner->kind() == "backend") {
            banner->hide_message();
        }
        _permission_banner_shown = false;
        return;
    }

    if (!any_readable && !_permission_banner_shown) {
        _permission_banner_shown = true;
        _show_banner(
            "No readable input devices. Install the generic udev rule "
            "or add your user to the input group, then retry. Do not run the GUI as root.",
            "permission", QString("sudo usermod -aG input $USER"), true);
    } else if (any_readable) {
        _show_banner(
            "The selected device is not readable. Choose another device or fix access.",
            "permission", QString("sudo usermod -aG input $USER"), true);
    }
}

void MainWindow::_update_record_enabled() {
    if (_state == "recording") {
        record_button->setEnabled(true);
    } else if (_state == "idle") {
        record_button->setEnabled(_has_accessible_selection());
    } else if (_state == "downloading") {
        record_button->setEnabled(true);  // acts as Cancel during the download
    } else {
        record_button->setEnabled(false);
    }
}

bool MainWindow::_has_accessible_selection() const {
    auto it = _selected_ids.find(_backend_kind);
    if (it == _selected_ids.end()) return false;
    const auto& selected = it->second;
    for (const auto& d : _devices) {
        if (selected.count(d.id) && d.accessible && d.capture_supported() && _kind_allowed(d.kind)) {
            return true;
        }
    }
    return false;
}

// Enabling a capture kind selects the accessible devices of that kind, one
// node per physical device — upstream registers *all* devices of an enabled
// kind (ListenerWindow.cs registers Gamepad/Mouse usages), so a checked
// toggle without any matching selected device would silently capture nothing.
void MainWindow::_on_capture_kind_toggled() {
    auto ensure_kind = [&](const std::string& kind, bool enabled) {
        if (!enabled) return;
        std::map<std::string, DeviceInfo> best;
        for (const auto& d : _devices) {
            if (d.kind != kind || !d.accessible || !d.capture_supported()) continue;
            auto identity = device_identity(d);
            auto it = best.find(identity);
            if (it == best.end() || prefers_as_default(d, it->second)) {
                best[identity] = d;
            }
        }
        bool changed = false;
        for (const auto& [_, d] : best) {
            if (_selected_ids[_backend_kind].insert(d.id).second) changed = true;
        }
        if (changed) {
            _populate_device_menu();
            _update_usb_summary();
        }
    };
    ensure_kind("keyboard", !capture_keyboard_action || capture_keyboard_action->isChecked());
    ensure_kind("gamepad", !capture_gamepad_action || capture_gamepad_action->isChecked());
    ensure_kind("mouse", capture_mouse_action && capture_mouse_action->isChecked());
    _update_record_enabled();
}

// Upstream capture-kind toggles (Keyboard / Gamepad / Mouse) filter what is
// recorded; mice are unchecked by default like the Windows original.
bool MainWindow::_kind_allowed(const std::string& kind) const {
    if (kind == "keyboard") return !capture_keyboard_action || capture_keyboard_action->isChecked();
    if (kind == "gamepad") return !capture_gamepad_action || capture_gamepad_action->isChecked();
    if (kind == "mouse") return capture_mouse_action && capture_mouse_action->isChecked();
    return true;
}

void MainWindow::_update_diagnostics_action() {
    if (!capture_diagnostics_action) return;
    capture_diagnostics_action->setText(
        QString("Dropped auto-repeats: %1").arg(_last_repeat_dropped));
}

void MainWindow::_record_clicked() {
    if (_state == "recording") {
        _stop_recording();
    } else if (_state == "idle") {
        _start_recording();
    } else if (_state == "downloading") {
        _cancel_download();
    }
}

void MainWindow::_start_recording() {
    if (!_has_accessible_selection()) {
        _check_permission_banner();
        _show_banner("No readable device is selected. Choose a device under Capture → Devices.",
                     "permission", QString("sudo usermod -aG input $USER"), true);
        return;
    }

    // Upstream only freezes when a previous result exists (Program.SetFreeze).
    bool freeze_active = freeze_check->isChecked() && _result.has_value();
    _freeze_source = freeze_active ? std::optional<Recording>(*_result) : std::nullopt;

    auto selected_devices = std::vector<DeviceInfo>();
    auto selected = _selected_ids[_backend_kind];
    for (const auto& d : _devices) {
        if (selected.count(d.id) && d.accessible && d.capture_supported() && _kind_allowed(d.kind)) {
            selected_devices.push_back(d);
        }
    }
    if (selected_devices.empty()) {
        _show_banner("No selected device matches the capture-kind toggles "
                     "(Keyboard, Gamepad/Joystick, Mouse) under Capture.",
                     "warning", QString(), false, 6000);
        return;
    }

    auto backend = std::make_unique<EvdevBackend>();
    backend->start(selected_devices);

    if (!backend->is_running()) {
        for (const auto& error : backend->drain_errors()) {
            _handle_capture_error(error);
        }
        _show_banner("No active capture readers — all selected devices failed to open. "
                     "Check permissions and try again.", "warning", QString(), false, 6000);
        _update_controls();
        return;
    }

    _session = std::make_unique<RecordingSession>(
        backend->measurement_kind(), &_settings,
        _freeze_source.has_value() ? &(*_freeze_source) : nullptr,
        freeze_active
    );

    _last_estimate = std::nullopt;
    _lost_sources.clear();
    _result = std::nullopt;
    _analysis = std::nullopt;
    _recording_started = 0.0;
    _last_repeat_dropped = 0;
    _update_diagnostics_action();

    _set_state("recording");
    timeline->set_recording(nullptr);
    _show_empty_charts("Recording… type normally.");
    _set_event_count(0);
    rate_readout->set_state(
        "Recording 0.0 s · collecting",
        QString("tentative · ") + QString::fromStdString(measurement_kind_short(backend->measurement_kind())),
        "The rate appears only after the minimum sample and prominence requirements.",
        &Theme::RECORDING_ACCENT
    );
    _update_title();

    _capture_timer.start();
    _tentative_timer.start();

    // Store backend for capture tick
    _backend = std::move(backend);
}

void MainWindow::_on_capture_tick() {
    if (!_session || !_backend) return;
    auto events = _backend->drain_events();
    for (const auto& event : events) {
        _session->add(event);
    }
    if (!events.empty()) {
        _update_live_status();
    }
    _drain_capture_errors();
}

bool MainWindow::_drain_capture_errors() {
    if (!_backend) return false;
    bool finished = false;
    for (const auto& error : _backend->drain_errors()) {
        _handle_capture_error(error);
    }
    return finished;
}

void MainWindow::_handle_capture_error(const CaptureError& error) {
    if (error.code == "disconnected") {
        if (_session && error.source_id) {
            _session->synthesize_releases(*error.source_id, _session->elapsed());
            _lost_sources.insert(*error.source_id);
        }
        _show_banner(
            QString::fromStdString(error.message) + " Releases were synthesized at the last captured timestamp.",
            "disconnect", QString(), false);
    } else if (error.code == "permission") {
        _show_banner(QString::fromStdString(error.message), "permission",
                     QString("sudo usermod -aG input $USER"), true);
    } else {
        _show_banner(QString::fromStdString(error.message), "warning", QString(), false, 6000);
    }
}

double MainWindow::_session_duration() const {
    if (!_session) return 0.0;
    return std::max(_session->elapsed(), 0.0);
}

void MainWindow::_update_live_status() {
    if (_state != "recording" || !_session) return;
    double elapsed = _session_duration();
    if (_last_estimate && _last_estimate->available()) {
        rate_readout->set_state(
            QString("Recording %1 s · %2 Hz (tentative)").arg(elapsed, 0, 'f', 1).arg(_format_hz(*_last_estimate->hz)),
            QString("tentative · ") + QString::fromStdString(measurement_kind_short(_session->measurement_kind)),
            "", &Theme::RECORDING_ACCENT);
    } else {
        rate_readout->set_state(
            QString("Recording %1 s · insufficient data").arg(elapsed, 0, 'f', 1),
            QString("tentative · ") + QString::fromStdString(measurement_kind_short(_session->measurement_kind)),
            "", &Theme::RECORDING_ACCENT);
    }
}

void MainWindow::_run_tentative_analysis() {
    if (_state != "recording" || !_session) return;
    if (!_session->is_dirty()) return;

    auto snapshot = _session->build(_session_duration());
    if (snapshot) {
        _schedule_analysis(*snapshot, true, true, false);
    }
}

void MainWindow::_stop_recording(bool auto_stop) {
    if (_state != "recording" || !_session || _stopping) return;
    _stopping = true;
    _capture_timer.stop();
    _tentative_timer.stop();

    if (_backend) {
        _backend->stop();
        auto events = _backend->drain_events();
        for (const auto& event : events) {
            _session->add(event);
        }
        _drain_capture_errors();
        _last_repeat_dropped = _backend->repeat_dropped();
    }
    _update_diagnostics_action();

    double duration = std::max(_session_duration(), _session->elapsed());
    auto recording = _session->build(duration);
    _session = nullptr;
    _stopping = false;
    _backend = nullptr;

    if (!recording) {
        _result = std::nullopt;
        _analysis = std::nullopt;
        _set_state("idle");
        _show_empty();
        _update_title();
        _show_banner("No key events were captured. Check the selected device and permissions.",
                     "info", QString(), false, 6000);
        _run_pending_hotplug_refresh();
        return;
    }

    _result = *recording;
    _settings = _result->settings.validated();
    _result->settings = _settings;
    timeline->set_recording(_result ? &(*_result) : nullptr, true);
    _set_event_count(_result->event_count());
    _sync_settings_controls();
    _set_state("analyzing");
    rate_readout->set_state(
        "Analyzing…",
        QString::fromStdString(measurement_kind_short(_result->measurement_kind)),
        "Final analysis runs off the GUI thread."
    );
    _update_title();
    _schedule_analysis(*_result, false, false, true);
    _run_pending_hotplug_refresh();
}

void MainWindow::_schedule_analysis(const Recording& recording, bool tentative, bool rate_only, bool reset_view) {
    _analysis_token++;
    _pending_token = _analysis_token;
    _pending_rate_only = rate_only;
    _pending_reset_view = reset_view;
    _analysis_pending = true;

    // Run analysis on a background thread using QtConcurrent. A newer request
    // supersedes an in-flight one, matching the Python port's token semantics;
    // this keeps a final analysis from being dropped when a tentative one is
    // still running at stop time.
    auto future = QtConcurrent::run([recording, tentative]() {
        AnalysisSettings s = recording.settings.validated();
        return analyze(recording, &s, tentative);
    });

    _analysis_watcher->setFuture(future);
}

void MainWindow::_on_analysis_finished() {
    if (!_analysis_pending) return;
    // A superseded request can leave a stale notification queued; only consume
    // the result of the future the watcher is currently observing.
    if (!_analysis_watcher->isFinished()) return;
    _analysis_pending = false;

    AnalysisResult result;
    try {
        result = _analysis_watcher->result();
    } catch (const std::exception& exc) {
        LOG_ERROR("Analysis") << "Analysis failed: " << exc.what();
        if (_state == "analyzing") _set_state("idle");
        _show_banner("Analysis failed; see the log for details.", "warning", QString(), false, 8000);
        return;
    } catch (...) {
        LOG_ERROR("Analysis") << "Analysis failed with an unknown exception";
        if (_state == "analyzing") _set_state("idle");
        _show_banner("Analysis failed; see the log for details.", "warning", QString(), false, 8000);
        return;
    }

    if (_pending_rate_only) {
        // Tentative analyses only refresh the live rate readout; they must not
        // touch the charts or the recording state.
        if (_state == "recording") {
            _last_estimate = result.estimate;
            _update_live_status();
        }
        return;
    }

    _apply_analysis(result, _pending_reset_view);
    if (_state == "analyzing") {
        _set_state("idle");
    }
    _update_title();
    _update_controls();
}

void MainWindow::_apply_analysis(const AnalysisResult& result, bool reset_view) {
    _analysis = result;

    std::vector<ChartData> chart_data = {
        result.diffs, result.compound, result.circular,
        result.f_diffs, result.f_compound, result.f_circular
    };

    for (size_t i = 0; i < charts.size() && i < chart_data.size(); ++i) {
        charts[i]->set_data(chart_data[i], reset_view);
    }

    charts[0]->footer = QString("0 ms events: %1 (%2%)")
        .arg(result.zero_ms_count).arg(result.zero_ms_percent, 0, 'f', 1).toStdString();
    for (size_t i = 1; i < charts.size(); ++i) {
        charts[i]->footer = "";
    }

    if (_result) {
        timeline->set_recording(_result ? &(*_result) : nullptr, reset_view);
        _set_event_count(_result->event_count());
        _sync_settings_controls();
    }
    _set_rate_state(result);
    _check_lost_sources();
}

void MainWindow::_set_rate_state(const AnalysisResult& result) {
    auto estimate = result.estimate;
    auto kind = result.measurement_kind;
    QString main;
    if (estimate.available()) {
        main = QString("Estimated report rate: %1 Hz").arg(_format_hz(*estimate.hz));
    } else {
        main = "Estimated report rate: insufficient data";
    }
    QString sub = QString("estimated · ") + QString::fromStdString(measurement_kind_short(kind));
    if (!estimate.confidence.empty() && estimate.confidence != "insufficient") {
        sub += QString(" · ") + QString::fromStdString(estimate.confidence);
    }
    rate_readout->set_state(main, sub, _estimate_tooltip(estimate, kind));
}

QString MainWindow::_estimate_tooltip(const RateEstimate& estimate, MeasurementKind kind) const {
    QStringList lines;
    if (!estimate.available()) {
        lines << "No confident periodic peak was found.";
    } else {
        lines << QString("Confidence: %1 (peak prominence %2).")
                   .arg(QString::fromStdString(estimate.confidence))
                   .arg(estimate.prominence, 0, 'f', 1);
        if (estimate.phase_coherence) {
            if (*estimate.phase_coherence >= 0.35) {
                lines << QString("Timing grid verified from raw timestamps "
                                 "(phase coherence %1).").arg(*estimate.phase_coherence, 0, 'f', 2);
            } else {
                lines << QString("Timing grid not confirmed against raw timestamps "
                                 "(phase coherence %1).").arg(*estimate.phase_coherence, 0, 'f', 2);
            }
        }
        if (!estimate.chart.empty()) {
            lines << QString("Evidence: %1.").arg(QString::fromStdString(estimate.chart));
        }
    }
    for (const auto& w : estimate.warnings) {
        lines << QString::fromStdString(w);
    }
    if (estimate.tentative) {
        lines << "Tentative: recording is still in progress.";
    }
    lines << _measurement_sentence(kind);
    return lines.join("\n");
}

QString MainWindow::_measurement_sentence(MeasurementKind kind) const {
    switch (kind) {
        case MeasurementKind::EVDEV:
            return "Derived from evdev event timing; not the USB descriptor interval.";
        case MeasurementKind::USB_REPORT:
            return "Derived from USB HID report completion timing; not the configured descriptor interval.";
        case MeasurementKind::TETRIO:
            return "Loaded from a TETR.IO replay. Inputs are downsampled onto a 600 Hz grid.";
        case MeasurementKind::FILE:
            return "Loaded file: the capture method is unknown; treat the rate as unverified.";
    }
    return "";
}

void MainWindow::_load_recording(const Recording& recording) {
    if (_state == "recording" || _state == "downloading") return;
    // Upstream MainForm.cs:214-216: opening a new file silently unchecks
    // freeze so the fresh recording is not merged with the old inputs.
    if (freeze_check->isChecked()) freeze_check->setChecked(false);
    _result = recording;
    _analysis = std::nullopt;
    _settings = recording.settings.validated();
    const_cast<Recording&>(_result.value()).settings = _settings;
    _last_estimate = std::nullopt;
    timeline->set_recording(_result ? &(*_result) : nullptr, true);
    _show_empty_charts("Analyzing…");
    _set_event_count(_result->event_count());
    _sync_settings_controls();
    _set_state("analyzing");
    rate_readout->set_state(
        "Analyzing…",
        QString::fromStdString(measurement_kind_short(recording.measurement_kind)),
        "Final analysis runs off the GUI thread."
    );
    _update_title();
    _schedule_analysis(recording, false, false, true);
    _update_controls();
}

void MainWindow::_show_empty() {
    _show_empty_charts();
    timeline->set_recording(nullptr);
    _set_event_count(std::nullopt);
    _sync_settings_controls();
    _analysis = std::nullopt;
    rate_readout->set_state(
        "No recording",
        "Select devices under Capture, then press Start Recording.",
        "Capture starts unprivileged. See Measurement limits for the timing layers."
    );
}

void MainWindow::_show_empty_charts(const QString& hint) {
    charts[0]->footer = "";
    for (auto* chart : charts) {
        chart->empty_hint = hint.toStdString();
        chart->set_data(std::nullopt);
    }
}

void MainWindow::_set_event_count(std::optional<int> count) {
    if (!count) {
        event_count_label->setText("");
        event_count_label->setStyleSheet(QString("color: ") + Theme::TEXT_SECONDARY.name() + ";");
        return;
    }
    event_count_label->setText(QString::number(*count) + " input events");
    QColor color = Theme::event_count_color(*count);
    event_count_label->setStyleSheet(QString("color: ") + color.name() + ";");
}

void MainWindow::_on_visibility_changed() {
    if (!_result) return;
    // Carry the user's timeline edits (colors/visibility/order) into the
    // shared result before re-analyzing, so they survive re-analysis and
    // saving. Follows the upstream model where InputInfo lives on Result.
    timeline->sync_edits_into(*_result);
    if (_state != "idle") return;
    _result->settings = _settings;
    _set_state("analyzing");
    _schedule_analysis(*_result, false, false, false);
}

void MainWindow::_on_timeline_changed() {
    if (_result) {
        timeline->sync_edits_into(*_result);
        _update_title();
    }
}

AnalysisSettings MainWindow::_current_settings() const {
    bool ok = false;
    int bin_rate = bin_edit->text().toInt(&ok);
    if (!ok) bin_rate = _settings.bin_rate;
    return AnalysisSettings{
        bin_rate,
        hps_spin->value(),
        low_cut->isChecked()
    }.validated();
}

void MainWindow::_sync_settings_controls() {
    _silent = true;
    bin_edit->setText(QString::number(_settings.bin_rate));
    bin_edit->setProperty("invalid", false);
    _repolish(bin_edit);
    low_cut->setChecked(_settings.low_cut);
    hps_spin->setValue(_settings.hps);
    _silent = false;
}

void MainWindow::_bin_text_changed(const QString& text) {
    if (_silent) return;
    _bin_debounce.start();
}

void MainWindow::_apply_bin_rate() {
    if (_silent) return;
    QString text = bin_edit->text().trimmed();
    bool ok = false;
    int value = text.toInt(&ok);
    if (!ok) {
        _mark_bin_invalid(true);
        return;
    }
    auto [clamped, valid] = clamp_bin_rate(value);
    _mark_bin_invalid(!valid);
    if (!valid) return;
    if (text != QString::number(value)) {
        _silent = true;
        bin_edit->setText(QString::number(value));
        _silent = false;
    }
    _settings = AnalysisSettings(value, _settings.hps, _settings.low_cut);
    _settings_changed();
}

void MainWindow::_mark_bin_invalid(bool invalid) {
    bin_edit->setProperty("invalid", invalid);
    _repolish(bin_edit);
}

void MainWindow::_scale_bin_rate(double factor) {
    bool ok = false;
    int value = bin_edit->text().toInt(&ok);
    if (!ok) value = _settings.bin_rate;
    value = static_cast<int>(value * factor);
    value = std::max(20, std::min(value, 128000));
    _silent = true;
    bin_edit->setText(QString::number(value));
    _silent = false;
    _mark_bin_invalid(false);
    _settings = AnalysisSettings(value, _settings.hps, _settings.low_cut);
    _settings_changed();
}

void MainWindow::_low_cut_toggled(bool checked) {
    if (_silent) return;
    _settings = AnalysisSettings(_settings.bin_rate, _settings.hps, checked);
    _settings_changed();
}

void MainWindow::_hps_changed(int value) {
    if (_silent) return;
    _settings = AnalysisSettings(_settings.bin_rate, value, _settings.low_cut);
    _settings_changed();
}

void MainWindow::_settings_changed() {
    if (_result) {
        _result->settings = _settings.validated();
        if (_state == "idle") {
            _set_state("analyzing");
            _schedule_analysis(*_result, false, false, false);
        }
    }
    _update_controls();
}

void MainWindow::_open_dialog() {
    if (_state != "idle") return;
    QString filename = QFileDialog::getOpenFileName(
        this, "Open recording", "",
        "All supported recordings (*.kbi *.ttr *.ttrm);;Keyboard Inspector (*.kbi);;All files (*)"
    );
    if (!filename.isEmpty()) {
        _load_path(filename);
    }
}

bool MainWindow::open_path(const QString& filename) {
    if (_state == "recording" || _state == "downloading") return false;
    LOG_INFO("UI") << "Opening file: " << filename.toStdString();
    QString lowered = filename.toLower();
    if (lowered.endsWith(".ttr") || lowered.endsWith(".ttrm")) {
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly)) {
            _show_banner("Unable to open the file. It could be in use by another process, "
                         "or it is in an unsupported format.", "warning", QString(), false, 8000);
            return false;
        }
        _load_tetrio_document(file.readAll(), filename);
        return true;  // parse errors are surfaced via banner in _load_tetrio_document
    }
    if (!lowered.endsWith(".kbi")) {
        _show_banner("Unsupported file. Open a .kbi recording or a TETR.IO replay (.ttr/.ttrm).",
                     "warning", QString(), false, 8000);
        return false;
    }
    try {
        Recording rec = read_kbi_file(filename.toStdString());
        _load_recording(rec);
        LOG_INFO("UI") << "Successfully loaded recording: " << rec.title << " (" << rec.event_count() << " events)";
        return true;
    } catch (const KbiError& exc) {
        LOG_ERROR("UI") << "Failed to read .kbi file: " << exc.what();
        _show_banner(QString::fromStdString(std::string(exc.what())), "warning", QString(), false, 8000);
        return false;
    }
}

void MainWindow::_load_path(const QString& filename) {
    open_path(filename);
}

// FileSystem.cs:45-51 TETR.IO disclaimer text.
static const char* TETRIO_DISCLAIMER =
    "You are analyzing a TETR.IO replay file.\n\n"
    "TETR.IO downsamples input data to 600 Hz to fit it onto the subframe grid which "
    "you will notice as a peak at 600 Hz in the frequency domain. This adds another "
    "\"sampling rate\" (in addition to the usual USB poll rate and matrix scan rate) "
    "to the process.\n\n"
    "This is unlike a regular Keyboard Inspector recording which tries to get the "
    "most accurate real-time timestamp it can without any additional downsampling.\n\n"
    "You may still analyze the recording, but be vary of the limitations of the "
    "TETR.IO replay format.";

bool MainWindow::_show_tetrio_disclaimer() {
    if (_tetrio_disclaimer_shown) return true;
    TetrioDisclaimerDialog dialog(TETRIO_DISCLAIMER, this);
    _tetrio_disclaimer_shown = true;  // upstream shows it once per session even if dismissed
    dialog.exec();
    return true;
}

void MainWindow::_load_tetrio_document(const QByteArray& data, const QString& origin) {
    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_ERROR("UI") << "TETR.IO replay parse failed: " << parse_error.errorString().toStdString();
        _show_banner("Unable to parse the file. It is likely corrupt, or it is in an unsupported format.",
                     "warning", QString(), false, 8000);
        return;
    }

    try {
        if (tetrio_is_multiplayer(doc)) {
            TetrioMultiplayer info = tetrio_inspect_multiplayer(doc);
            TetrioPickerDialog picker(info, this);
            if (picker.exec() != QDialog::Accepted || !picker.selection()) {
                _show_empty();
                return;
            }
            const TetrioRoundEntry* choice = picker.selection();
            Recording rec = tetrio_build_recording(doc, choice->round_index, choice->player_id);
            _show_tetrio_disclaimer();
            _load_recording(rec);
        } else {
            Recording rec = tetrio_build_recording(doc);
            _show_tetrio_disclaimer();
            _load_recording(rec);
        }
        LOG_INFO("UI") << "Loaded TETR.IO replay: " << origin.toStdString();
    } catch (const TetrioError& exc) {
        LOG_ERROR("UI") << "TETR.IO replay error: " << exc.what();
        _show_banner(QString("Unable to parse the file. ") + exc.what(), "warning", QString(), false, 8000);
    } catch (const std::exception& exc) {
        LOG_ERROR("UI") << "TETR.IO replay error: " << exc.what();
        _show_banner("Unable to parse the file. It is likely corrupt, or it is in an unsupported format.",
                     "warning", QString(), false, 8000);
    }
}

void MainWindow::_import_url_dialog() {
    if (_state != "idle") return;
    ImportUrlDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || dialog.format() == ImportFormat::None) return;
    _import_url(dialog.download_url(), dialog.format());
}

void MainWindow::_import_url(const QString& download_url, ImportFormat format) {
    if (_state != "idle") return;

    if (!_network) {
        _network = new QNetworkAccessManager(this);
    }

    _download_format = format;
    QNetworkRequest request{QUrl(download_url)};
    request.setTransferTimeout(30000);
    // HTTP.cs: no auto-redirect; the status code is surfaced verbatim.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("User-Agent",
                         QString("%1/%2 (+https://github.com/mat1jaczyyy/Keyboard-Inspector)")
                             .arg(QString("KeyboardInspectorLinux"), QString(APP_VERSION))
                             .toUtf8());

    _download_reply = _network->get(request);
    connect(_download_reply, &QNetworkReply::finished, this, &MainWindow::_download_finished);

    _set_state("downloading");
}

void MainWindow::_cancel_download() {
    if (_download_reply) {
        _download_reply->abort();  // finished() fires and restores idle state
    }
}

void MainWindow::_download_finished() {
    QNetworkReply* reply = _download_reply;
    if (!reply) return;
    _download_reply = nullptr;
    reply->deleteLater();

    if (_state != "downloading") {
        reply->close();
        return;
    }

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        // Upstream returns an empty FileResult and restores idle silently.
        reply->close();
        _set_state("idle");
        rate_readout->set_state("No recording",
                                "Select devices under Capture, then press Start Recording.",
                                "Capture starts unprivileged. See Measurement limits for the timing layers.");
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        LOG_ERROR("UI") << "Import failed: " << reply->errorString().toStdString();
        reply->close();
        _set_state("idle");
        _show_banner("Unable to connect to the server. Check your network connection.",
                     "warning", QString(), false, 8000);
        return;
    }

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        reply->close();
        _set_state("idle");
        QString phrase = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        QString message = QString("Unable to download the file. Received status code %1 (%2).")
                              .arg(status).arg(phrase.isEmpty() ? QString::number(status) : phrase);
        _show_banner(message, "warning", QString(), false, 8000);
        return;
    }

    QByteArray data = reply->readAll();
    reply->close();

    if (_download_format == ImportFormat::Kbi) {
        try {
            Recording rec = kbi_loads(std::string(data.constData(), static_cast<size_t>(data.size())));
            _load_recording(rec);
        } catch (const KbiError& exc) {
            _set_state("idle");
            LOG_ERROR("UI") << "Imported .kbi parse failed: " << exc.what();
            _show_banner(QString("Unable to parse the file. ") + exc.what(), "warning", QString(), false, 8000);
        }
    } else {
        _load_tetrio_document(data, "URL import");
    }
}

void MainWindow::_check_for_updates() {
    if (!_network) {
        _network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request{QUrl("https://github.com/mat1jaczyyy/Keyboard-Inspector/releases/latest")};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply* reply = _network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        // GitHub answers the releases/latest request with a redirect to
        // /releases/tag/<tag>; the tag carries the newest version.
        QVariant target = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
        if (!target.isValid()) return;
        QString url = target.toString();
        static const QRegularExpression tag_rule("/releases/tag/(.+)$");
        auto match = tag_rule.match(url);
        if (!match.hasMatch()) return;
        QString tag = match.captured(1);
        if (tag != QString(APP_VERSION)) {
            _apply_update_available();
        }
    });
}

void MainWindow::_apply_update_available() {
    if (update_button && update_button->isHidden()) {
        update_button->show();
    }
}

void MainWindow::_save_dialog() {
    if (!_result || _state != "idle") return;
    QString suggested = QString::fromStdString(_result->title.empty() ? "recording" : _result->title) + ".kbi";
    QString filename = QFileDialog::getSaveFileName(this, "Save recording", suggested, "Keyboard Inspector (*.kbi)");
    if (filename.isEmpty()) return;
    if (!filename.toLower().endsWith(".kbi")) filename += ".kbi";
    try {
        write_kbi_file(*_result, filename.toStdString());
        _show_banner("Saved " + filename, "info", QString(), false, 4000);
    } catch (const KbiError& exc) {
        _show_banner(QString::fromStdString(std::string(exc.what())), "warning", QString(), false, 8000);
    }
}

void MainWindow::_set_state(const QString& state) {
    _state = state;
    bool recording = state == "recording";
    bool downloading = state == "downloading";
    record_button->setText(recording ? "Stop Recording" : (downloading ? "Cancel" : "Start Recording"));
    record_button->setProperty("recording", recording);
    _repolish(record_button);
    if (downloading) {
        record_button->setEnabled(true);
        rate_readout->set_state("Downloading file…", "import from URL",
                                "The file downloads over HTTPS with redirects disabled.");
    }
    _update_record_enabled();
    _update_controls();
}

void MainWindow::_update_controls() {
    bool idle = _state == "idle";
    file_button->setEnabled(idle);
    capture_button->setEnabled(idle);
    freeze_check->setEnabled(idle);
    splitter->setEnabled(idle);
    bool save_enabled = _result.has_value() && idle;
    if (save_action) save_action->setEnabled(save_enabled);
    bool settings_enabled = _result.has_value() && idle;
    QWidget* settings_widgets[] = {bin_edit, half_button, double_button, low_cut, hps_spin};
    for (auto* w : settings_widgets) {
        if (w) w->setEnabled(settings_enabled);
    }
    _update_record_enabled();
}

void MainWindow::_update_title() {
    QString title = QString(APP_NAME) + " " + QString(APP_VERSION);
    if (_state == "recording") {
        setWindowTitle("Recording – " + title);
    } else if (_result) {
        setWindowTitle(QString::fromStdString(_result->display_title()) + " – " + title);
    } else {
        setWindowTitle(title);
    }
}

void MainWindow::_clamp_to_screen() {
    if (_screenshot_mode || QGuiApplication::platformName() == "offscreen") {
        return;
    }
    QScreen* screen = this->screen() ?: QGuiApplication::primaryScreen();
    if (!screen) return;
    QRect available = screen->availableGeometry();
    if (width() > available.width() || height() > available.height()) {
        resize(
            std::max(MainWindow::MIN_WIDTH, available.width() - 20),
            std::max(MainWindow::MIN_HEIGHT, available.height() - 60)
        );
        showMaximized();
    }
}

QString MainWindow::_format_hz(double value) const {
    return format_hz(value);
}

void MainWindow::_show_banner(const QString& text, const QString& kind,
                               const QString& copy_text, bool retry,
                               std::optional<int> timeout_ms) {
    banner->show_message(text, kind, copy_text, retry, timeout_ms);
}

void MainWindow::_check_lost_sources() {
    if (_lost_sources.empty() || !_result) return;
    QStringList names;
    for (int64_t sid : _lost_sources) {
        auto it = _result->sources.find(sid);
        if (it != _result->sources.end()) {
            names << QString::fromStdString(it->second.name);
        }
    }
    if (!names.empty()) {
        _show_banner("Disconnected: " + names.join(", ") +
                     ". Releases were synthesized at the last captured timestamp.",
                     "disconnect");
    }
}

void MainWindow::_repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QString MainWindow::_device_signature(const std::vector<DeviceInfo>& devices) {
    QStringList parts;
    for (const auto& d : devices) {
        parts << QString("%1:%2:%3").arg(QString::fromStdString(d.id))
                         .arg(d.accessible ? "1" : "0")
                         .arg(QString::fromStdString(d.kind));
    }
    std::sort(parts.begin(), parts.end());
    return parts.join("|");
}

void MainWindow::_device_toggled(const QString& device_id, bool checked) {
    auto& selected = _selected_ids[_backend_kind];
    if (checked) selected.insert(device_id.toStdString());
    else selected.erase(device_id.toStdString());
    _update_usb_summary();
    _update_record_enabled();
}

void MainWindow::_set_auto_select_new(bool checked) {
    _auto_select_new = checked;
}

void MainWindow::_run_pending_hotplug_refresh() {
    // Triggered after recording stops or errors; refreshes device list
    _hotplug_debounce.start();
}

void MainWindow::_stop_if_running() {
    if (_state == "recording") {
        _stop_recording();
    }
}

void MainWindow::_refresh_from_hotplug() {
    if (_state == "recording") return;
    _refresh_devices(true);
}

void MainWindow::_poll_hotplug() {
    if (_state == "recording") return;
    auto backend = std::make_unique<EvdevBackend>();
    auto new_devices = backend->list_devices();

    QString old_sig = _device_signature(_devices);
    QString new_sig = _device_signature(new_devices);

    if (old_sig != new_sig) {
        _hotplug_debounce.start();
    }
}

void MainWindow::_announce_hotplug(const std::vector<DeviceInfo>& detected,
                                    const std::vector<DeviceInfo>& newly_selected) {
    if (newly_selected.empty()) return;

    QStringList names;
    for (const auto& d : newly_selected) {
        names << QString::fromStdString(d.display_name());
    }
    if (!names.isEmpty()) {
        _show_banner("Device(s) added: " + names.join(", "), "info", QString(), false, 4000);
    }
}

void MainWindow::_handle_spotlight(ChartWidget* chart) {
    if (!chart) return;

    // Upstream MainForm.HandleSpotlight (MainForm.cs:150-168): double-click
    // moves the chart to the top of the charts panel, enlarging it above the
    // remaining grid; double-click again restores its grid position.
    int index = -1;
    for (size_t i = 0; i < charts.size(); ++i) {
        if (charts[i] == chart) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) return;
    int row = index / 3;
    int col = index % 3;

    if (chart_grid->indexOf(chart) >= 0) {
        chart_grid->removeWidget(chart);
        charts_layout->insertWidget(0, chart, 6);
    } else {
        charts_layout->removeWidget(chart);
        chart_grid->addWidget(chart, row, col);
    }
    chart->setVisible(true);
    chart->update();
}

} // namespace keyboard_inspector
