#pragma once

#include <QWidget>
#include <QPointF>
#include <QRectF>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMenu>
#include <QColorDialog>
#include <QApplication>
#include <vector>
#include <optional>

#include "../core/model.h"
#include "../ui/theme.h"

namespace keyboard_inspector {

class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    std::optional<Recording> recording;
    double zoom = 1.0;
    double viewport = 0.0;
    std::string empty_hint = "Select a keyboard, press Start Recording, then type.";

signals:
    void changed();
    void analysis_requested();
    void message(const QString& text);

public:
    void set_recording(const Recording* recording, bool reset_view = true);
    int visible_row_count() const;
    // Push the user's color/visibility/row-order edits (held in this widget's
    // copy) back into the shared recording so they survive re-analysis and
    // are written when the recording is saved. No-op if the widget's copy is
    // stale (different inputs or no recording).
    void sync_edits_into(Recording& recording) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void _start_row_drag(int visible_row, QMouseEvent* event);
    void _reorder_row(int visible_from, int visible_to);
    int _label_hit_row(const QPointF& position, const QRectF& plot, double key_width) const;
    std::vector<InputInfo> visible_rows_impl() const;
    void _show_menu(const QPointF& position);
    double full_span() const;
    double max_zoom() const;

    struct TimelineGeometry {
        QRectF plot;
        QRectF xaxis;
        QRectF scroll_area;
        QRectF thumb;
        double key_width;
        double source_width;
        QFontMetrics metrics;
    };

    TimelineGeometry _geometry() const;
    std::pair<double, double> _x_range() const;
    int _row_at(double y, const QRectF& plot) const;

    std::vector<std::pair<double, double>> _hold_intervals(const InputInfo& info, double duration) const;

    void _draw_axes(QPainter* painter, const QRectF& plot, const QRectF& xaxis,
                    double x_min, double x_max, double span);
    void _draw_rows(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                    double row_height, double key_width, double source_width, const QFontMetrics& metrics);
    void _draw_holds(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                     double row_height, double x_min, double x_max, double x_unit);
    void _draw_hover(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                     double row_height, double x_min, double x_unit);
    void _draw_scrollbar(QPainter* painter, const QRectF& thumb);

    int _find_event(int row_index, double x_value) const;

    int _hover_row = -1;
    std::optional<Event> _hover_event;
    QPointF _pan_origin;
    double _pan_viewport = 0.0;
    bool _scroll_drag = false;
    double _scroll_origin = 0.0;
    double _scroll_viewport = 0.0;
    bool _scroll_hover = false;
    int _pending_drag_row = -1;
    QPointF _pending_drag_origin;
};

} // namespace keyboard_inspector
