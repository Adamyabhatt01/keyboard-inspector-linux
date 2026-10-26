#pragma once

#include <QMainWindow>
#include <QSettings>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QSplitter>
#include <QGridLayout>
#include <QToolButton>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QMenu>
#include <QActionGroup>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QtConcurrent>
#include <optional>
#include <set>
#include <map>
#include <vector>
#include <memory>
#include <string>

#include "../capture/base.h"
#include "../capture/evdev_backend.h"
#include "../core/model.h"
#include "../core/analysis.h"
#include "../core/recording.h"
#include "../core/kbi.h"
#include "../core/tetrio.h"
#include "../ui/theme.h"
#include "../ui/chart.h"
#include "../ui/timeline.h"
#include "../ui/dialogs.h"

namespace keyboard_inspector {

class RateReadout : public QWidget {
    Q_OBJECT
public:
    explicit RateReadout(QWidget* parent = nullptr);
    void set_state(const QString& main, const QString& sub = "",
                   const QString& tooltip = "", const QColor* color = nullptr);
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QString _main;
    QString _sub;
    QColor _main_color;
};

class Banner : public QFrame {
    Q_OBJECT
public:
    explicit Banner(QWidget* parent = nullptr);
    void show_message(const QString& text, const QString& kind = "warning",
                      const QString& copy_text = QString(), bool retry = false,
                      std::optional<int> timeout_ms = std::nullopt);
    void hide_message();
    QString kind() const { return _kind; }
    std::function<void()> retry_callback;
private:
    QString _kind;
    QString _copy_text;
    QLabel* label = nullptr;
    QPushButton* copy_button = nullptr;
    QPushButton* retry_button = nullptr;
    QPushButton* dismiss_button = nullptr;
    QTimer _timer;
    QTimer _copy_reset_timer;
    void _copy();
    void _retry();
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool open_path(const QString& filename);
    void set_screenshot_mode(bool enabled) { _screenshot_mode = enabled; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void _build_ui();
    QFrame* _build_top_bar();
    QFrame* _build_analysis_strip(QWidget* parent);

    void _refresh_devices(bool from_hotplug = false);
    void _populate_device_menu();
    void _update_usb_summary();
    void _fit_menu_to_screen(QMenu* menu);
    void _update_record_enabled();
    void _check_permission_banner();

    void _on_capture_kind_toggled();
    void _retry_devices();

    void _record_clicked();
    void _start_recording();
    void _stop_recording(bool auto_stop = false);
    void _on_capture_tick();
    void _handle_capture_error(const CaptureError& error);
    bool _drain_capture_errors();
    void _update_live_status();
    void _run_tentative_analysis();

    void _schedule_analysis(const Recording& recording, bool tentative, bool rate_only, bool reset_view);
    void _on_analysis_finished();
    void _apply_analysis(const AnalysisResult& result, bool reset_view);
    void _set_rate_state(const AnalysisResult& result);
    void _on_visibility_changed();
    void _on_timeline_changed();

    AnalysisSettings _current_settings() const;
    void _sync_settings_controls();
    void _bin_text_changed(const QString& text);
    void _apply_bin_rate();
    void _scale_bin_rate(double factor);
    void _low_cut_toggled(bool checked);
    void _hps_changed(int value);
    void _settings_changed();

    void _open_dialog();
    void _load_path(const QString& filename);
    void _save_dialog();
    void _load_recording(const Recording& recording);

    void _import_url_dialog();
    void _import_url(const QString& download_url, ImportFormat format);
    void _cancel_download();
    void _download_finished();
    void _load_tetrio_document(const QByteArray& data, const QString& origin);
    bool _show_tetrio_disclaimer();

    void _check_for_updates();
    void _apply_update_available();

    void _set_state(const QString& state);
    void _update_controls();
    void _update_title();
    void _show_empty();
    void _show_empty_charts(const QString& hint = QString());
    void _set_event_count(std::optional<int> count);
    void _show_banner(const QString& text, const QString& kind = "warning",
                      const QString& copy_text = QString(), bool retry = false,
                      std::optional<int> timeout_ms = std::nullopt);
    void _clamp_to_screen();

    QString _state = "idle";
    bool _silent = false;
    bool _stopping = false;
    std::optional<Recording> _result;
    std::optional<AnalysisResult> _analysis;
    std::unique_ptr<RecordingSession> _session;
    std::optional<Recording> _freeze_source;
    std::optional<RateEstimate> _last_estimate;
    std::vector<DeviceInfo> _devices;
    std::map<std::string, std::set<std::string>> _selected_ids;
    std::set<int64_t> _lost_sources;
    std::unique_ptr<EvdevBackend> _backend;

    QFrame* top_bar = nullptr;
    Banner* banner = nullptr;
    QSplitter* splitter = nullptr;
    QVBoxLayout* charts_layout = nullptr;
    QGridLayout* chart_grid = nullptr;
    std::vector<ChartWidget*> charts;
    QFrame* analysis_strip = nullptr;
    TimelineWidget* timeline = nullptr;

    QToolButton* file_button = nullptr;
    QToolButton* help_button = nullptr;
    QToolButton* capture_button = nullptr;
    QMenu* capture_menu = nullptr;
    QMenu* devices_menu = nullptr;
    QAction* rescan_action = nullptr;
    QAction* usb_summary_action = nullptr;
    QAction* save_action = nullptr;
    QAction* capture_keyboard_action = nullptr;
    QAction* capture_gamepad_action = nullptr;
    QAction* capture_mouse_action = nullptr;
    QAction* capture_diagnostics_action = nullptr;
    QToolButton* update_button = nullptr;
    QCheckBox* freeze_check = nullptr;
    QPushButton* record_button = nullptr;

    QLineEdit* bin_edit = nullptr;
    QPushButton* half_button = nullptr;
    QPushButton* double_button = nullptr;
    QCheckBox* low_cut = nullptr;
    QLabel* hps_label = nullptr;
    QSpinBox* hps_spin = nullptr;
    QLabel* partials_label = nullptr;
    QLabel* event_count_label = nullptr;

    RateReadout* rate_readout = nullptr;

    QTimer _capture_timer;
    QTimer _tentative_timer;
    QTimer _bin_debounce;
    QTimer _hotplug_poll;
    QTimer _hotplug_debounce;
    QTimer _hotplug_settle;
    QFutureWatcher<AnalysisResult>* _analysis_watcher;

    QSettings _settings_store{"KeyboardInspector", "KeyboardInspectorLinux"};
    AnalysisSettings _settings;
    std::string _backend_kind = "evdev";
    bool _restore_state = true;
    bool _devices_initialized = false;
    bool _auto_select_new = true;
    bool _permission_banner_shown = false;
    bool _tetrio_disclaimer_shown = false;
    double _recording_started = 0.0;

    QNetworkAccessManager* _network = nullptr;
    QNetworkReply* _download_reply = nullptr;
    ImportFormat _download_format = ImportFormat::None;

    int _analysis_token = 0;
    int _pending_token = -1;
    bool _pending_rate_only = false;
    bool _pending_reset_view = true;
    bool _analysis_pending = false;

    QString _format_hz(double value) const;
    QString _estimate_tooltip(const RateEstimate& estimate, MeasurementKind kind) const;
    QString _measurement_sentence(MeasurementKind kind) const;
    bool _has_accessible_selection() const;
    bool _kind_allowed(const std::string& kind) const;
    void _update_diagnostics_action();
    int _last_repeat_dropped = 0;
    double _session_duration() const;
    void _check_lost_sources();
    void _device_toggled(const QString& device_id, bool checked);
    void _set_auto_select_new(bool checked);
    void _run_pending_hotplug_refresh();
    void _stop_if_running();
    void _refresh_from_hotplug();
    void _poll_hotplug();
    void _announce_hotplug(const std::vector<DeviceInfo>& detected,
                           const std::vector<DeviceInfo>& newly_selected);
    void _mark_bin_invalid(bool invalid);
    void _repolish(QWidget* widget);
    QString _device_signature(const std::vector<DeviceInfo>& devices);
    void _handle_spotlight(ChartWidget* chart);

    bool _screenshot_mode{false};

    static constexpr int DEFAULT_WIDTH = 1294;
    static constexpr int DEFAULT_HEIGHT = 836;
    static constexpr int MIN_WIDTH = 720;
    static constexpr int MIN_HEIGHT = 500;
};

} // namespace keyboard_inspector
