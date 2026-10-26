#pragma once

#include <QWidget>
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <functional>
#include <optional>

#include "../core/analysis.h"
#include "../ui/theme.h"

namespace keyboard_inspector {

struct ChartGeometry {
    QRectF plot;
    QRectF xaxis;
    QRectF scroll_area;
    QRectF thumb;
    QRectF notch_left;
    QRectF notch_right;
    double cramped_zoom = 1.0;
};

class ChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit ChartWidget(const QString& title = "",
                         const QColor& color = Theme::TIME_LINE,
                         double initial_span = 0.0,
                         QWidget* parent = nullptr);

    QString title;
    QColor color;
    double initial_span = 0.0;

    std::optional<ChartData> data;
    std::string footer;
    std::string empty_hint = "Select a keyboard, press Start Recording, then type.";

    double zoom = 1.0;
    double viewport = 0.0;

    std::function<void(ChartWidget*)> spotlight;

    // Upstream MainForm.cs:204-205: the timeline uses 3 micro decimals on the
    // axis and 6 on the hover readout; frequency charts use plain "0.###".
    int x_axis_micro_decimals = 0;
    int highlight_x_decimals = 3;

    double full_span() const;
    double max_zoom() const;
    double visible_span() const;

    void set_data(std::optional<ChartData> data, bool reset_view = true);
    void set_title(const QString& title);
    void reset_view();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

public:
    int hover_index() const { return _hover_index; }

private:
    ChartGeometry geometry_for_paint() const;
    std::tuple<double, double, int, int> visible_range() const;
    double x_px(double value, double x_min, double x_unit, const QRectF& plot) const;
    int find_hover(const QPointF& position) const;
    bool zoom_change(double x_fraction, double scrolls);
    QPointF captured_tick(const QPointF& position);

    void draw_empty(QPainter* painter, const ChartGeometry& geo);
    void draw_grid(QPainter* painter, const ChartGeometry& geo);
    void draw_axis_labels(QPainter* painter, const ChartGeometry& geo);
    void draw_title(QPainter* painter);
    void draw_line(QPainter* painter, const ChartGeometry& geo);
    void draw_footer(QPainter* painter, const ChartGeometry& geo);
    void draw_hover(QPainter* painter, const ChartGeometry& geo);
    void draw_scrollbar(QPainter* painter, const ChartGeometry& geo);

    // Drag modes (upstream Chart.cs Captured* fields).
    static constexpr int kDragNone = 0;
    static constexpr int kDragPan = 1;
    static constexpr int kDragScrollbar = 2;
    static constexpr int kDragNotchLeft = 3;
    static constexpr int kDragNotchRight = 4;
    static constexpr int kPanUndecided = 0;
    static constexpr int kPanHorizontal = 1;
    static constexpr int kPanVertical = 2;

    int _drag_mode = kDragNone;
    int _pan_direction = kPanUndecided;
    int _hover_index = -1;
    QPointF _pan_origin;
    QPoint _captured_global;
    QPointF _captured_offset;
    double _pan_viewport = 0.0;
    double _captured_zoom = 1.0;
    double _captured_zoom_with_cramp = 1.0;
    int _scroll_hover = 0;  // 1 thumb · 2 left notch · 3 right notch
};

} // namespace keyboard_inspector
