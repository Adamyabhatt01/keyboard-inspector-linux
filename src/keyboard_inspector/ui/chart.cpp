#include "chart.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QLinearGradient>
#include <QFontMetrics>
#include <QCursor>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

#include "../ui/theme.h"

namespace keyboard_inspector {

namespace {

constexpr double kMinScrollbar = 20.0;
constexpr double kHoverRadius = 29.0;

double clamp_value(double value, double low, double high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Upstream Chart.cs Cramp (Chart.cs:602-628): nonlinear easing that slows the
// zoom change near the minimum scrollbar width so notch drags feel damped.
struct Cramp {
    static constexpr double s = 0.0004;
    static inline double a = 0.0, m = 0.0, n = 0.0, z = 0.0;

    static void refresh(double cramped_zoom, double max_zoom) {
        a = 1.0 / cramped_zoom;
        m = 1.0 / max_zoom;
        n = std::exp(-1.0 / (a - m));
        z = a - std::log(s) / std::log(n);
    }

    static double f(double x) {
        return std::pow(n, a - x) * (a - m) + m;
    }

    static double calc(double x) {
        if (x < z) return std::max(m, s * (x - z) + f(z));
        if (x < a) return f(x);
        return x;
    }

    static double inv(double y) {
        double fz = f(z);
        if (y < fz) return (y - fz) / s + z;
        if (y < a) return a - std::log((y - m) / (a - m)) / std::log(n);
        return y;
    }
};

// Utility.DrawShadowString (Utility.cs:131-140): text stamped twice with a
// shadow offset of (-1,-1) and (+2,+2) around the final pass.
void draw_shadow_string(QPainter* painter, const QString& text, const QRectF& rect,
                        int alignment, const QColor& text_color) {
    QColor shadow = Theme::blend(Theme::TOPBAR_BG, QColor(0, 0, 0), 0.75);
    shadow.setAlpha(224);

    painter->setPen(shadow);
    painter->drawText(rect.translated(-1, -1), alignment, text);
    painter->drawText(rect.translated(2, 2), alignment, text);
    painter->setPen(text_color);
    painter->drawText(rect, alignment, text);
}

int fractional_digits(double value) {
    QString text = QString::number(value, 'f', 6);
    // C# "{value:0.######}" trims trailing zeros.
    while (text.endsWith('0')) text.chop(1);
    int dot = text.indexOf('.');
    if (dot < 0 || dot + 1 >= text.size()) return 0;
    return text.size() - dot - 1;
}

// Format like C# "0.{zeros}{hashes}": fixed digits, then up to 6 total
// decimals with trailing zeros trimmed.
QString format_axis_number(double value, int digits) {
    QString text = QString::number(value, 'f', 6);
    text = text.left(text.size() - std::max(0, static_cast<int>(text.size()) - 6));
    while (text.endsWith('0') && static_cast<int>(text.size() - text.indexOf('.') - 1) > std::max(digits, 0)) {
        text.chop(1);
    }
    if (text.endsWith('.')) text.chop(1);
    return text;
}

// Time-domain charts label in milliseconds (upstream tCharts).
double time_interval_ms(int i) {
    return Theme::screen_interval_seconds(i) * 1000.0;
}

// Upstream frequency-domain interval series: 5, 10, 25, 75, 125, 250, ...
double frequency_interval_hz(int i) {
    if (i < 2) return (i + 1) * 5.0;
    if (i < 4) return (i - 1) * 25.0;
    return std::pow(2.0, i - 4) * 125.0;
}

} // namespace

ChartWidget::ChartWidget(const QString& title, const QColor& color, double initial_span, QWidget* parent)
    : QWidget(parent), title(title), color(color), initial_span(initial_span)
{
    setMouseTracking(true);
    setMinimumSize(140, 110);
    setAccessibleName(title);
    setToolTip("Wheel: zoom · Drag: pan · Double-click: spotlight · Values are derived from captured timestamps.");
}

double ChartWidget::full_span() const {
    if (!data || data->values.empty()) return 1.0;
    return std::max(static_cast<double>(data->values.size() - 1) * data->x_factor, data->x_factor);
}

double ChartWidget::max_zoom() const {
    if (!data) return 1.0;
    return std::max(1.0, static_cast<double>(data->values.size() - 1) * 0.66);
}

double ChartWidget::visible_span() const {
    return full_span() / std::max(zoom, 1e-9);
}

void ChartWidget::set_data(std::optional<ChartData> data, bool reset_view_param) {
    this->data = data;
    _hover_index = -1;
    if (!data) {
        zoom = 1.0;
        viewport = 0.0;
    } else if (reset_view_param) {
        reset_view();
    } else {
        zoom = clamp_value(zoom, 1.0, max_zoom());
        viewport = clamp_value(viewport, 0.0, std::max(0.0, 1.0 - 1.0 / zoom));
    }
    update();
}

void ChartWidget::set_title(const QString& title) {
    this->title = title;
    setAccessibleName(title);
    update();
}

void ChartWidget::reset_view() {
    if (!data || initial_span == 0.0) {
        zoom = 1.0;
    } else {
        zoom = clamp_value(full_span() / initial_span, 1.0, max_zoom());
    }
    viewport = 0.0;
}

ChartGeometry ChartWidget::geometry_for_paint() const {
    double width = static_cast<double>(this->width());
    double height = static_cast<double>(this->height());
    double title_height = 18.0;
    double left = 20.0;
    double right = 20.0;
    double xaxis_height = 21.0;
    double scroll_height = 17.0;
    double top = title_height + 1.0;
    double plot_bottom = height - scroll_height - xaxis_height;
    QRectF plot(
        left,
        top,
        std::max(10.0, width - left - right),
        std::max(10.0, plot_bottom - top)
    );
    QRectF xaxis(plot.left(), plot.bottom(), plot.width(), xaxis_height);
    double scroll_area_width = std::max(20.0, width - 23.0);
    QRectF scroll_area(11.5, height - scroll_height, scroll_area_width, scroll_height);
    double cramped_zoom = scroll_area.width() / kMinScrollbar;  // upstream Units.CrampedZoom
    double area_width = scroll_area.width();
    double thumb_width = area_width / std::max(zoom, 1e-9);
    if (thumb_width < kMinScrollbar) {
        area_width -= kMinScrollbar - thumb_width;
        thumb_width = kMinScrollbar;
    }
    QRectF thumb(
        scroll_area.left() + area_width * viewport,
        scroll_area.top() + 2.5,
        std::min(thumb_width, area_width),
        scroll_area.height() - 8.0
    );
    ChartGeometry geo{plot, xaxis, scroll_area, thumb};
    geo.notch_left = QRectF(thumb.left(), thumb.top(), 6.0, thumb.height());
    geo.notch_right = QRectF(thumb.right() - 6.0, thumb.top(), 6.0, thumb.height());
    geo.cramped_zoom = cramped_zoom;
    return geo;
}

std::tuple<double, double, int, int> ChartWidget::visible_range() const {
    if (!data || data->values.empty()) return {0.0, 1.0, 0, -1};
    double x_min = viewport * full_span();
    double x_max = std::min(full_span(), x_min + visible_span());
    double factor = data->x_factor;
    int first = static_cast<int>(std::max(0.0, std::floor(x_min / factor)));
    int last = static_cast<int>(std::min(static_cast<double>(data->values.size() - 1), std::ceil(x_max / factor)));
    return {x_min, x_max, first, last};
}

double ChartWidget::x_px(double value, double x_min, double x_unit, const QRectF& plot) const {
    return plot.left() + (value - x_min) * x_unit;
}

int ChartWidget::find_hover(const QPointF& position) const {
    if (!data || data->values.empty()) return -1;
    ChartGeometry geo = geometry_for_paint();
    if (!geo.plot.contains(position)) return -1;

    auto [x_min, x_max, first, last] = visible_range();
    double span = std::max(x_max - x_min, 1e-12);
    double x_unit = geo.plot.width() / span;

    double y_unit = geo.plot.height() / std::max(data->y_max, 1e-12);
    int best = -1;
    double best_distance = kHoverRadius * kHoverRadius;

    int minimum = std::max(0, first - 2);
    int maximum = std::min(static_cast<int>(data->values.size()) - 1, last + 2);
    for (int index = minimum; index <= maximum; ++index) {
        double px = x_px(index * data->x_factor, x_min, x_unit, geo.plot);
        double py = geo.plot.bottom() - static_cast<double>(data->values[index]) * y_unit;
        double dx = px - position.x();
        double dy = py - position.y();
        double distance = dx * dx + dy * dy;
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return best;
}

void ChartWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Theme::WINDOW_BG);

    ChartGeometry geo = geometry_for_paint();

    // Title with the upstream double-stamped shadow; the unit label sits on
    // the right edge of the title row.
    QFont title_font = font();
    title_font.setPointSizeF(9.0);
    painter.setFont(title_font);
    QFontMetrics title_metrics(title_font);
    std::string unit = data ? data->unit : "";
    int reserve = unit.empty() ? 0 : 26;
    QString elided = title_metrics.elidedText(
        title,
        Qt::ElideRight,
        std::max(0, static_cast<int>(width() - reserve * 2))
    );
    draw_shadow_string(&painter, elided,
                       QRectF(0, 0, static_cast<double>(width() - reserve), 18.0),
                       Qt::AlignHCenter | Qt::AlignVCenter, Theme::TEXT_SECONDARY);
    if (!unit.empty()) {
        painter.setFont(title_font);
        draw_shadow_string(&painter, QString::fromStdString(unit),
                           QRectF(width() - reserve, 0, reserve - 2, 18.0),
                           Qt::AlignRight | Qt::AlignVCenter, Theme::TEXT_DIM);
    }

    painter.fillRect(geo.plot, Theme::CHART_BG);

    if (!data) {
        draw_empty(&painter, geo);
        return;
    }

    draw_grid(&painter, geo);
    draw_axis_labels(&painter, geo);
    draw_line(&painter, geo);
    draw_footer(&painter, geo);
    draw_hover(&painter, geo);
    draw_scrollbar(&painter, geo);
}

void ChartWidget::draw_empty(QPainter* painter, const ChartGeometry& geo) {
    painter->setPen(Theme::GRID_Y);
    for (int i = 0; i < 6; ++i) {
        double y = geo.plot.top() + geo.plot.height() * i / 5.0;
        painter->drawLine(QPointF(geo.plot.left(), y), QPointF(geo.plot.right(), y));
    }

    QFont font = this->font();
    font.setPointSizeF(8.5);
    painter->setFont(font);
    painter->setPen(Theme::TEXT_DIM);
    painter->drawText(
        geo.plot.adjusted(8, 8, -8, -8),
        Qt::AlignCenter | Qt::TextWordWrap,
        QString::fromStdString(empty_hint)
    );
    draw_scrollbar(painter, geo);
}

void ChartWidget::draw_grid(QPainter* painter, const ChartGeometry& geo) {
    painter->setPen(QPen(Theme::GRID_Y, 1.0));
    for (int i = 0; i < 6; ++i) {
        double y = geo.plot.top() + geo.plot.height() * i / 5.0;
        painter->drawLine(QPointF(geo.plot.left(), y), QPointF(geo.plot.right(), y));
    }
}

void ChartWidget::draw_title(QPainter* painter) {
    if (title.isEmpty()) return;
    QFont font = this->font();
    font.setPointSizeF(8.5);
    painter->setFont(font);
    QRectF rect(0, 0, static_cast<double>(width()), 17.0);
    draw_shadow_string(painter, title, rect, Qt::AlignCenter | Qt::AlignVCenter, Theme::TEXT);
}

void ChartWidget::draw_axis_labels(QPainter* painter, const ChartGeometry& geo) {
    if (!data) return;
    auto [x_min, x_max, _, __] = visible_range();
    double span = std::max(x_max - x_min, 1e-12);
    double factor_to_px = geo.plot.width() / span;

    // Upstream picks the smallest interval that leaves at least 30 px (time) or
    // 40 px (frequency) between labels, using its own interval series rather
    // than a generic 1-2-5 step.
    double lowest = (data->is_frequency ? 40.0 : 30.0) / std::max(factor_to_px, 1e-12);
    double interval = 0.0;
    for (int i = 0; i < 1000; ++i) {
        interval = data->is_frequency ? frequency_interval_hz(i) : time_interval_ms(i);
        if (interval >= lowest) break;
    }
    if (!(interval > 0.0)) return;

    QFont font = this->font();
    font.setPointSizeF(8.0);
    painter->setFont(font);
    QFontMetrics metrics(font);

    // Upstream Chart.cs:1284: digits are capped at XAxisMicroDecimals; tick
    // values with more fractional digits collapse into a gray ".000" micro
    // remainder segment.
    int digits = std::min(fractional_digits(interval), x_axis_micro_decimals);
    double micro_scale = std::pow(10.0, x_axis_micro_decimals);

    double px = interval * factor_to_px;
    double offset = std::round(x_min * factor_to_px);
    int pos = static_cast<int>(std::ceil(x_min / interval)) - 1;
    double text_low = geo.xaxis.left() - px / 6.0;
    double text_high = geo.xaxis.right() + px / 6.0;

    for (int guard = 0; guard < 10000; ++guard, ++pos) {
        double point = interval * pos;
        double s = std::round(pos * px) - offset + geo.plot.left();

        QString text = format_axis_number(point, digits);
        QColor text_color = Theme::TEXT;
        QPen grid_pen(Theme::GRID_X, 1.0);

        if (fractional_digits(point) > x_axis_micro_decimals) {
            // Upstream: "{point * 10^micro % 1:.000}" — the fractional part of
            // the scaled value, always three digits (Chart.cs:1304).
            double scaled = point * micro_scale;
            double frac = scaled - std::floor(scaled);
            long long micro = static_cast<long long>(std::llround(frac * 1000.0)) % 1000;
            text = QString(".%1").arg(micro, 3, 10, QChar('0'));
            text_color = Theme::TEXT_DIM;
            grid_pen = QPen(Theme::GRID_Y, 1.0);
        }

        if (s >= geo.plot.left() - 0.5 && s <= geo.plot.right() + 0.5) {
            painter->setPen(grid_pen);
            painter->drawLine(
                QPointF(s, geo.plot.top()),
                QPointF(s, geo.xaxis.top() + 4.5)
            );
        }

        if (s >= text_low && s <= text_high) {
            double text_width = static_cast<double>(metrics.horizontalAdvance(text));
            double text_x = clamp_value(s, geo.xaxis.left(), geo.xaxis.right());
            painter->setPen(Qt::NoPen);
            draw_shadow_string(
                painter, text,
                QRectF(text_x - text_width / 2 - 2, geo.xaxis.top() + 3, text_width + 4, 15),
                Qt::AlignCenter,
                text_color
            );
        } else if (s >= geo.xaxis.right()) {
            break;
        }
    }
}

void ChartWidget::draw_line(QPainter* painter, const ChartGeometry& geo) {
    if (!data || data->values.empty()) return;
    auto [x_min, x_max, first, last] = visible_range();
    if (last < first) return;

    double span = std::max(x_max - x_min, 1e-12);
    double x_unit = geo.plot.width() / span;
    double x_unit_index = x_unit * data->x_factor;
    double y_unit = geo.plot.height() / std::max(data->y_max, 1e-12);

    auto to_point = [&](int index) {
        return QPointF(
            x_px(index * data->x_factor, x_min, x_unit, geo.plot),
            geo.plot.bottom() - static_cast<double>(data->values[index]) * y_unit
        );
    };

    const int count = last - first + 1;
    if (count <= 0) return;

    QLinearGradient gradient(geo.plot.left(), geo.plot.top(), geo.plot.left(), geo.plot.bottom());
    gradient.setColorAt(0.0, color);
    gradient.setColorAt(0.5, color);
    gradient.setColorAt(1.0, Theme::blend(color, Theme::CHART_BG, 0.5));
    QPen line_pen(QBrush(gradient), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QColor point_color = Theme::blend(color, QColor("#FFFFFF"), 0.6);

    painter->save();
    painter->setClipRect(geo.plot.adjusted(-0.5, -0.5, 0.5, 0.5));

    QPointF prev = to_point(first);
    if (count == 1) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(point_color);
        painter->drawEllipse(prev, 1.5, 1.5);
        painter->restore();
        return;
    }

    // Upstream collapses densely spaced samples into vertical min/max strokes
    // (Chart.OnPaint "useYLine") instead of sampling every Nth bin, so narrow
    // spikes remain visible when the whole histogram is shown.
    bool merge = x_unit_index < 2.0;
    double cutoff = 1.0;
    if (merge) {
        cutoff = (5.0 - 3.0 * std::sqrt(std::max(0.0, 1.0 - std::pow(x_unit_index - 1.0, 4)))) / 5.0;
        if (!std::isfinite(cutoff) || cutoff <= 0.0) merge = false;
    }
    double point_size = 1.8 / (1.0 + std::exp(9.0 - x_unit_index));

    bool use_y_line = false;
    double y_min = prev.y();
    double y_max = prev.y();
    double extra_diff = 0.0;

    for (int i = 1; i < count; ++i) {
        QPointF point = to_point(first + i);
        double diff = extra_diff + (point.x() - prev.x());

        y_min = std::min(y_min, point.y());
        y_max = std::max(y_max, point.y());

        if (merge && i < count - 1 && diff < cutoff) {
            use_y_line = true;
            continue;
        }
        if (merge) {
            extra_diff = std::fmod(diff, cutoff);
        }

        if (use_y_line) {
            double y_start = prev.y() > point.y() ? y_max : y_min;
            double y_end = prev.y() > point.y() ? y_min : y_max;
            painter->setPen(line_pen);
            painter->drawLine(QPointF(prev.x(), y_start), QPointF(point.x(), y_end));
            use_y_line = false;
        } else {
            painter->setPen(line_pen);
            painter->drawLine(prev, point);

            if (x_unit_index > 5.0) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(point_color);
                if (i == 1 && _hover_index != 0) {
                    painter->drawEllipse(prev, point_size / 2.0, point_size / 2.0);
                }
                if (first + i != _hover_index) {
                    painter->drawEllipse(point, point_size / 2.0, point_size / 2.0);
                }
            }
        }

        if (!use_y_line) {
            y_min = prev.y();
            y_max = prev.y();
        }
        prev = point;
    }

    painter->restore();
}

void ChartWidget::draw_footer(QPainter* painter, const ChartGeometry& geo) {
    if (footer.empty()) return;
    QFont font = this->font();
    font.setPointSizeF(8.0);
    painter->setFont(font);
    QFontMetrics metrics(font);
    QString text = metrics.elidedText(
        QString::fromStdString(footer),
        Qt::ElideRight,
        std::max(0, static_cast<int>(geo.plot.width() - 12))
    );
    draw_shadow_string(painter, text,
                       QRectF(geo.plot.left() + 3, geo.plot.bottom() - 18,
                              static_cast<double>(metrics.horizontalAdvance(text)) + 8, 15),
                       Qt::AlignCenter | Qt::AlignVCenter, Theme::TEXT_DIM);
}

void ChartWidget::draw_hover(QPainter* painter, const ChartGeometry& geo) {
    if (_hover_index < 0 || !data) return;
    int index = _hover_index;
    if (index >= static_cast<int>(data->values.size())) return;

    auto [x_min, x_max, _, __] = visible_range();
    double span = std::max(x_max - x_min, 1e-12);
    double x_unit = geo.plot.width() / span;
    double y_unit = geo.plot.height() / std::max(data->y_max, 1e-12);

    QPointF point(
        x_px(index * data->x_factor, x_min, x_unit, geo.plot),
        geo.plot.bottom() - static_cast<double>(data->values[index]) * y_unit
    );

    QColor marker_color = Theme::blend(color, QColor("#FFFFFF"), 0.16);
    marker_color.setAlpha(128);
    painter->setPen(QPen(Theme::blend(color, QColor("#FFFFFF"), 0.16), 1.0));
    painter->setBrush(marker_color);
    painter->drawEllipse(point, 3.0, 3.0);

    // Upstream Chart.cs:1566-1576: the x value is rounded to
    // HighlightXDecimals (or free-form "0.###" when negative). When the x
    // text carries more than 3 decimals, the remainder is split into a gray
    // micro segment: "(12.345" + ".678" + ", 42)".
    double x_value = index * data->x_factor;
    if (highlight_x_decimals >= 0) {
        x_value = std::round(x_value * std::pow(10.0, highlight_x_decimals)) /
                  std::pow(10.0, highlight_x_decimals);
    }
    QString x_text = highlight_x_decimals < 0
        ? QString::number(x_value, 'f', 3).remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"))
        : QString::number(x_value, 'f', highlight_x_decimals);
    QString y_text = QString::number(static_cast<double>(data->values[index]), 'f', 3)
                         .remove(QRegularExpression("0+$")).remove(QRegularExpression("\\.$"));

    QFont font = this->font();
    font.setPointSizeF(8.0);
    painter->setFont(font);
    QFontMetrics metrics(font);

    QStringList segments;
    if (x_text.section('.', 1).size() > 3) {
        QString head = QString::number(std::floor(x_value * 1000.0) / 1000.0, 'f', 3);
        // Upstream strips the dot from the micro remainder (Chart.cs:1575),
        // so the concatenated text reads "(12.345678, 42)".
        QString micro = QString::number(x_value * 1e3 - std::floor(x_value * 1e3) + 1e-9, 'f', 3)
                            .remove(QChar('.'));
        segments << QString("(%1").arg(head) << micro << QString(", %1)").arg(y_text);
    } else {
        segments << QString("(%1, %2)").arg(x_text).arg(y_text);
    }

    QVector<double> widths;
    double total_width = 0.0;
    double text_height = static_cast<double>(metrics.height());
    for (const QString& segment : segments) {
        double w = static_cast<double>(metrics.horizontalAdvance(segment));
        widths << w;
        total_width += w;
    }

    // Placement with the upstream mirroring fallback (Chart.cs:1592-1607).
    QRectF rect(
        std::round(point.x()) + 9,
        point.y() - 1 - text_height,
        total_width,
        text_height
    );
    if (!geo.plot.contains(rect)) {
        double dx = std::round((point.x() - rect.x()) * 2 - std::ceil(rect.width())) + 2;
        rect.moveLeft(rect.x() + dx);
        if (!geo.plot.contains(rect)) {
            double dy = std::round((point.y() - rect.y()) * 2 - std::ceil(rect.height())) + 3;
            rect.moveTop(rect.y() + dy);
            if (!geo.plot.contains(rect)) {
                rect.translate(-dx, 0);
            }
        }
    }

    // Shadow plate behind the bubble (upstream ShadowBrush).
    QRectF shadow_rect(rect.left() - 1.75, rect.top() - 3, rect.width() + 2, rect.height() + 2.5);
    QColor plate = Theme::blend(Theme::TOPBAR_BG, QColor(0, 0, 0), 0.75);
    plate.setAlpha(160);
    painter->setPen(Qt::NoPen);
    painter->setBrush(plate);
    painter->drawRect(shadow_rect);

    QColor subtext = Theme::blend(Theme::TEXT, Theme::CHART_BG, 0.4);
    double x = rect.left();
    for (int i = 0; i < segments.size(); ++i) {
        draw_shadow_string(painter, segments[i],
                           QRectF(x, rect.top(), widths[i], rect.height()),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           i % 2 == 0 ? Theme::TEXT : subtext);
        x += widths[i];
    }
}

void ChartWidget::draw_scrollbar(QPainter* painter, const ChartGeometry& geo) {
    if (!data) return;

    auto component_color = [&](int component) {
        if (_drag_mode == kDragScrollbar || _drag_mode == kDragNotchLeft || _drag_mode == kDragNotchRight) {
            int captured = _drag_mode == kDragScrollbar ? 1 : (_drag_mode == kDragNotchLeft ? 2 : 3);
            if (component == captured) return Theme::SCROLLBAR_CAPTURED;
        }
        if (component == _scroll_hover) return Theme::SCROLLBAR_HOVER;
        return Theme::SCROLLBAR;
    };

    // Track jump zones get the plain chart background (upstream BackBrush).
    QRectF jump_left(geo.scroll_area.left(), geo.thumb.top(),
                     std::max(0.0, geo.thumb.left() - geo.scroll_area.left()), geo.thumb.height());
    QRectF jump_right(geo.thumb.right(), geo.thumb.top(),
                      std::max(0.0, geo.scroll_area.right() - geo.thumb.right()), geo.thumb.height());
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::CHART_BG);
    painter->drawRect(jump_left);
    painter->drawRect(jump_right);

    // Notches take hover priority, matching the upstream hover checks.
    painter->setBrush(component_color(2));
    painter->drawRect(geo.notch_left);
    painter->setBrush(component_color(3));
    painter->drawRect(geo.notch_right);
    painter->setBrush(component_color(1));
    painter->drawRect(geo.thumb);

    // Notch guide lines (upstream Chart.cs:1656-1668).
    painter->setBrush(Theme::CHART_BG);
    QRectF notch_line(0, geo.thumb.top() + 1.6, 1.4, geo.thumb.height() - 3.2);
    notch_line.moveLeft(geo.thumb.left() + 1.8);
    painter->drawRect(notch_line);
    notch_line.moveRight(geo.thumb.right() - 1.8);
    painter->drawRect(notch_line);
}

QPointF ChartWidget::captured_tick(const QPointF& position) {
    QPoint global = QCursor::pos();
    QPointF offset(_captured_global.x() - global.x(), _captured_global.y() - global.y());
    if (!offset.isNull()) {
        _captured_offset += offset;
        // Upstream pins the cursor to the capture point while dragging
        // (Chart.CapturedCursorTick), accumulating the true drag distance.
        QCursor::setPos(_captured_global);
    }
    (void)position;
    return _captured_offset;
}

bool ChartWidget::zoom_change(double x_fraction, double scrolls) {
    if (scrolls == 0) return false;
    double s = 1.0 / zoom;
    double change = std::pow(1.05, scrolls);
    zoom *= change;

    if (zoom <= 1.0) {
        zoom = 1.0;
        viewport = 0.0;
    } else {
        if (zoom > max_zoom()) {
            zoom = max_zoom();
            change = zoom * s;
        }
        viewport += x_fraction * (1.0 - 1.0 / change) * s;
    }
    return true;
}

void ChartWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !data) {
        QWidget::mousePressEvent(event);
        return;
    }
    ChartGeometry geo = geometry_for_paint();
    QPointF position = event->position();

    auto capture = [&](int mode) {
        _drag_mode = mode;
        _pan_origin = position;
        _captured_global = event->globalPosition().toPoint();
        _captured_offset = QPointF(0, 0);
        _pan_viewport = viewport;
        _captured_zoom = zoom;
        if (mode == kDragScrollbar || mode == kDragNotchLeft || mode == kDragNotchRight) {
            Cramp::refresh(geo.cramped_zoom, max_zoom());
            _captured_zoom_with_cramp = 1.0 / Cramp::inv(1.0 / zoom);
            _scroll_hover = 0;
            setCursor(Qt::SizeHorCursor);
        } else {
            _pan_direction = (zoom == 1.0) ? kPanVertical : kPanUndecided;
            setCursor(Qt::SizeHorCursor);
        }
    };

    if (geo.notch_left.contains(position)) {
        capture(kDragNotchLeft);
    } else if (geo.notch_right.contains(position)) {
        capture(kDragNotchRight);
    } else if (geo.thumb.contains(position)) {
        capture(kDragScrollbar);
    } else if (geo.scroll_area.contains(position)) {
        double page = 1.0 / std::max(zoom, 1e-9);
        if (position.x() < geo.thumb.left()) {
            viewport = clamp_value(viewport - page, 0.0, 1.0 - page);
        } else if (position.x() > geo.thumb.right()) {
            viewport = clamp_value(viewport + page, 0.0, 1.0 - page);
        }
        _hover_index = -1;
    } else if (geo.plot.contains(position) || geo.xaxis.contains(position)) {
        capture(kDragPan);
        _hover_index = -1;
    }
    update();
    event->accept();
}

void ChartWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!data) return;
    QPointF position = event->position();
    ChartGeometry geo = geometry_for_paint();

    if (_drag_mode == kDragPan) {
        QPointF offset = captured_tick(position);

        if (_pan_direction == kPanUndecided) {
            if (offset.manhattanLength() < 3) {
                event->accept();
                return;
            }
            double angle = std::atan2(std::abs(offset.y()), std::abs(offset.x()));
            if (angle <= M_PI / 6) _pan_direction = kPanHorizontal;
            else if (angle >= M_PI / 3) _pan_direction = kPanVertical;
            if (_pan_direction != kPanUndecided) _captured_offset = QPointF(0, 0);
        }

        if (_pan_direction == kPanHorizontal) {
            double limit = std::max(0.0, 1.0 - 1.0 / std::max(zoom, 1e-9));
            viewport = clamp_value(
                _pan_viewport + _captured_offset.x() / std::max(geo.plot.width(), 1.0) / std::max(zoom, 1e-9),
                0.0, limit
            );
        } else if (_pan_direction == kPanVertical) {
            // Vertical drag-zoom at the captured x (upstream PanAction).
            double fraction = clamp_value(
                (_pan_origin.x() - geo.plot.left()) / std::max(geo.plot.width(), 1.0), 0.0, 1.0);
            zoom_change(fraction, _captured_offset.y() / 10.0);
        }
        update();
        event->accept();
        return;
    }

    if (_drag_mode != kDragNone) {
        // Scrollbar drag family (upstream ScrollAction).
        QPointF offset = captured_tick(position);
        double area_width = std::max(geo.scroll_area.width(), 1.0);
        double distance = -offset.x() / area_width;

        if (_drag_mode == kDragScrollbar) {
            double limit = std::max(0.0, 1.0 - 1.0 / std::max(zoom, 1e-9));
            viewport = clamp_value(_pan_viewport - distance, 0.0, limit);
        } else if (_drag_mode == kDragNotchLeft) {
            double right_edge = _pan_viewport + 1.0 / _captured_zoom;
            Cramp::refresh(geo.cramped_zoom, max_zoom());
            double new_inv_zoom = clamp_value(
                Cramp::calc(1.0 / _captured_zoom_with_cramp + offset.x() / area_width),
                0.0, right_edge);
            zoom = 1.0 / new_inv_zoom;
            viewport = right_edge - new_inv_zoom;
        } else if (_drag_mode == kDragNotchRight) {
            Cramp::refresh(geo.cramped_zoom, max_zoom());
            double new_inv_zoom = clamp_value(
                Cramp::calc(1.0 / _captured_zoom_with_cramp + distance),
                0.0, 1.0 - viewport);
            zoom = 1.0 / new_inv_zoom;
        }
        update();
        event->accept();
        return;
    }

    int hover = find_hover(position);
    int scroll_hover = 0;
    if (geo.notch_left.contains(position)) scroll_hover = 2;
    else if (geo.notch_right.contains(position)) scroll_hover = 3;
    else if (geo.thumb.contains(position)) scroll_hover = 1;

    if (hover != _hover_index || scroll_hover != _scroll_hover) {
        _hover_index = hover;
        _scroll_hover = scroll_hover;
        update();
    }

    if (scroll_hover != 0) {
        setCursor(Qt::SizeHorCursor);
    } else if (geo.plot.contains(position) || geo.xaxis.contains(position)) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
}

void ChartWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _pan_origin = QPointF();
        _drag_mode = kDragNone;
        _pan_direction = kPanUndecided;
        _captured_offset = QPointF(0, 0);
        unsetCursor();
        update();
        event->accept();
    }
}

void ChartWidget::leaveEvent(QEvent* event) {
    _hover_index = -1;
    _scroll_hover = 0;
    unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

void ChartWidget::wheelEvent(QWheelEvent* event) {
    if (!data) return;
    ChartGeometry geo = geometry_for_paint();
    QPointF position = event->position();
    if (!(geo.plot.contains(position) || geo.xaxis.contains(position))) return;

    double steps = event->angleDelta().y() / 120.0;
    if (steps == 0) return;

    double old_zoom = zoom;
    double new_zoom = clamp_value(old_zoom * std::pow(1.05, steps), 1.0, max_zoom());
    if (std::abs(new_zoom - old_zoom) < 1e-9) {
        event->accept();
        return;
    }
    double fraction = clamp_value((position.x() - geo.plot.left()) / std::max(geo.plot.width(), 1.0), 0.0, 1.0);
    viewport = viewport + fraction * (1.0 / old_zoom - 1.0 / new_zoom);
    zoom = new_zoom;
    double limit = std::max(0.0, 1.0 - 1.0 / zoom);
    viewport = clamp_value(viewport, 0.0, limit);
    if (_drag_mode == kDragPan) {
        _captured_offset = QPointF(0, 0);
        _pan_viewport = viewport;
        _captured_zoom = zoom;
    }
    _hover_index = find_hover(position);
    update();
    event->accept();
}

void ChartWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && data) {
        ChartGeometry geo = geometry_for_paint();
        if (geo.plot.contains(event->position()) && spotlight) {
            spotlight(this);
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace keyboard_inspector
