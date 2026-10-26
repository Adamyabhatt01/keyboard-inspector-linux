#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMenu>
#include <QColorDialog>
#include <QApplication>
#include <QClipboard>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>

namespace keyboard_inspector {

namespace {

constexpr double kMinScrollbar = 20.0;

double clamp_value(double value, double low, double high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Axis label helpers mirroring the chart implementation (upstream
// DrawShadowString and the XAxisMicroDecimals formatting).
void draw_shadow_text(QPainter* painter, const QString& text, const QRectF& rect,
                      const QColor& text_color) {
    QColor shadow = Theme::blend(Theme::TOPBAR_BG, QColor(0, 0, 0), 0.75);
    shadow.setAlpha(224);
    painter->setPen(shadow);
    painter->drawText(rect.translated(-1, -1), Qt::AlignCenter, text);
    painter->drawText(rect.translated(2, 2), Qt::AlignCenter, text);
    painter->setPen(text_color);
    painter->drawText(rect, Qt::AlignCenter, text);
}

int fractional_digits_of(double value) {
    QString text = QString::number(value, 'f', 6);
    while (text.endsWith('0')) text.chop(1);
    int dot = text.indexOf('.');
    if (dot < 0 || dot + 1 >= text.size()) return 0;
    return text.size() - dot - 1;
}

QString format_axis_number_fixed(double value, int digits) {
    QString text = QString::number(value, 'f', 6);
    while (text.endsWith('0') && (text.size() - text.indexOf('.') - 1) > std::max(digits, 0)) {
        text.chop(1);
    }
    if (text.endsWith('.')) text.chop(1);
    return text;
}

} // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(125);
    setAcceptDrops(true);
    setAccessibleName("Input timeline");
    setToolTip("Hold bars per input. Wheel: zoom · Drag: pan · Right-click: input options.");
}

double TimelineWidget::full_span() const {
    if (!recording) return 1.0;
    double last = 0.0;
    for (const auto& e : recording->events) {
        if (e.time > last) last = e.time;
    }
    return std::max({recording->duration, last, 0.001});
}

double TimelineWidget::max_zoom() const {
    return std::max(1.0, full_span() * 1000.0 * 0.66);
}

int TimelineWidget::visible_row_count() const {
    if (!recording) return 0;
    int count = 0;
    for (const auto& info : recording->inputs) {
        if (info.visible) ++count;
    }
    return count;
}

std::vector<InputInfo> TimelineWidget::visible_rows_impl() const {
    if (!recording) return {};
    std::vector<InputInfo> result;
    for (const auto& info : recording->inputs) {
        if (info.visible) result.push_back(info);
    }
    return result;
}

void TimelineWidget::set_recording(const Recording* recording, bool reset_view) {
    this->recording = recording ? std::optional<Recording>(*recording) : std::nullopt;
    _hover_row = -1;
    _hover_event = std::nullopt;
    if (!recording) {
        zoom = 1.0;
        viewport = 0.0;
    } else if (reset_view) {
        zoom = 1.0;
        viewport = 0.0;
    } else {
        zoom = clamp_value(zoom, 1.0, max_zoom());
        viewport = clamp_value(viewport, 0.0, std::max(0.0, 1.0 - 1.0 / zoom));
    }
    update();
}

void TimelineWidget::sync_edits_into(Recording& recording) const {
    if (!this->recording || recording.events.size() != this->recording->events.size()) return;

    // Only merge when the widget's copy describes the same inputs as the
    // target; otherwise the copy is stale (new recording/cleared view) and
    // must not overwrite fresh state.
    std::set<InputId> copy_ids;
    for (const auto& info : this->recording->inputs) {
        copy_ids.insert(info.input_id);
    }
    std::set<InputId> target_ids;
    for (const auto& info : recording.inputs) {
        target_ids.insert(info.input_id);
    }
    if (copy_ids != target_ids) return;

    std::vector<InputInfo> merged;
    merged.reserve(recording.inputs.size());
    for (const auto& held : this->recording->inputs) {
        const InputInfo* target = recording.get_info(held.input_id);
        if (!target) return;
        merged.push_back(InputInfo{held.input_id, held.color, held.visible, target->events});
    }
    if (merged.size() != recording.inputs.size()) return;
    recording.inputs = std::move(merged);
}

TimelineWidget::TimelineGeometry TimelineWidget::_geometry() const {
    double width = static_cast<double>(this->width());
    double height = static_cast<double>(this->height());
    double left_margin = 12.0;
    double right_margin = 20.0;
    double xaxis_height = 21.0;
    double scroll_height = 17.0;
    double top = 2.0;
    double plot_bottom = height - xaxis_height - scroll_height;

    auto rows = visible_rows_impl();

    QFont key_font = font();
    key_font.setPointSizeF(8.5);
    QFontMetrics key_metrics(key_font);
    double key_width = 0.0;
    for (const auto& info : rows) {
        double w = static_cast<double>(key_metrics.horizontalAdvance(QString::fromStdString(info.input_id.key)));
        if (w > key_width) key_width = w;
    }
    key_width = std::min(key_width + 8.0, std::max(24.0, width * 0.30));

    std::set<int64_t> sources;
    for (const auto& info : rows) {
        sources.insert(info.input_id.source_id);
    }
    double source_width = 0.0;
    if (sources.size() > 1 && recording) {
        for (int64_t sid : sources) {
            auto it = recording->sources.find(sid);
            if (it != recording->sources.end()) {
                double w = static_cast<double>(key_metrics.horizontalAdvance(QString::fromStdString(it->second.name)));
                if (w > source_width) source_width = w;
            }
        }
        source_width = std::min(source_width + 14.0, std::max(40.0, width * 0.32));
    }

    double plot_left = left_margin + source_width + key_width + 6.0;
    if (plot_left > width - right_margin - 40) {
        plot_left = left_margin + key_width + 6.0;
        source_width = 0.0;
    }

    QRectF plot(
        plot_left,
        top,
        std::max(20.0, width - plot_left - right_margin),
        std::max(20.0, plot_bottom - top)
    );
    QRectF xaxis(plot.left(), plot.bottom(), plot.width(), xaxis_height);
    QRectF scroll_area(11.5, height - scroll_height, std::max(20.0, width - 23.0), scroll_height);
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

    return {plot, xaxis, scroll_area, thumb, key_width, source_width, QFontMetrics(font())};
}

std::pair<double, double> TimelineWidget::_x_range() const {
    double span = full_span() / std::max(zoom, 1e-9);
    double x_min = viewport * full_span();
    return {x_min, std::min(full_span(), x_min + span)};
}

int TimelineWidget::_row_at(double y, const QRectF& plot) const {
    auto rows = visible_rows_impl();
    if (rows.empty() || !plot.contains(QPointF(plot.center().x(), y))) return -1;
    int index = static_cast<int>((y - plot.top()) / std::max(plot.height() / static_cast<double>(rows.size()), 1e-9));
    return (index >= 0 && index < static_cast<int>(rows.size())) ? index : -1;
}

std::vector<std::pair<double, double>> TimelineWidget::_hold_intervals(const InputInfo& info, double duration) const {
    std::vector<std::pair<double, double>> intervals;
    std::optional<double> start = std::nullopt;
    for (const auto& event : info.events) {
        if (event.pressed) {
            if (!start) start = event.time;
        } else if (start) {
            intervals.emplace_back(*start, event.time);
            start = std::nullopt;
        }
    }
    if (start) {
        intervals.emplace_back(*start, std::max(duration, *start));
    }
    return intervals;
}

void TimelineWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Theme::WINDOW_BG);

    TimelineGeometry geo = _geometry();
    painter.fillRect(geo.plot, Theme::CHART_BG);

    auto rows = visible_rows_impl();

    if (!recording || rows.empty()) {
        QFont font = this->font();
        font.setPointSizeF(8.5);
        painter.setFont(font);
        painter.setPen(Theme::TEXT_DIM);
        QString hint = recording ? QString("No visible inputs.") : QString::fromStdString(empty_hint);
        painter.drawText(
            geo.plot.adjusted(10, 10, -10, -10),
            Qt::AlignCenter | Qt::TextWordWrap,
            hint
        );
        _draw_scrollbar(&painter, geo.thumb);
        return;
    }

    double row_height = geo.plot.height() / static_cast<double>(rows.size());
    auto [x_min, x_max] = _x_range();
    double span = std::max(x_max - x_min, 1e-12);
    double x_unit = geo.plot.width() / span;

    _draw_axes(&painter, geo.plot, geo.xaxis, x_min, x_max, span);
    _draw_rows(&painter, geo.plot, rows, row_height, geo.key_width, geo.source_width, geo.metrics);
    _draw_holds(&painter, geo.plot, rows, row_height, x_min, x_max, x_unit);
    _draw_hover(&painter, geo.plot, rows, row_height, x_min, x_unit);
    _draw_scrollbar(&painter, geo.thumb);
}

void TimelineWidget::_draw_axes(QPainter* painter, const QRectF& plot, const QRectF& xaxis,
                                 double x_min, double x_max, double span) {
    double x_unit = plot.width() / std::max(span, 1e-12);
    QRectF unit_rect(12, xaxis.top() + 3, 80, 15);
    double lowest = 40.0 / std::max(x_unit, 1e-12);
    double step = 0.0;
    for (int i = 0; i < 1000; ++i) {
        step = Theme::screen_interval_seconds(i);
        if (step >= lowest) break;
    }
    if (!(step > 0.0)) return;

    QFont font = this->font();
    font.setPointSizeF(8.0);
    painter->setFont(font);
    QFontMetrics metrics(font);

    // Upstream KeyHistory uses XAxisMicroDecimals = 3 (MainForm.cs:204):
    // labels with more than 3 decimals collapse into a gray ".000" micro
    // remainder, replacing the full text.
    constexpr int kMicroDecimals = 3;
    int digits = std::min(fractional_digits_of(step), kMicroDecimals);
    double micro_scale = std::pow(10.0, kMicroDecimals);

    double value = std::floor(x_min / step) * step;

    while (value <= x_max + step * 0.5) {
        double px = plot.left() + (value - x_min) * x_unit;
        if (plot.left() <= px && px <= plot.right()) {
            painter->setPen(QPen(Theme::GRID_X, 1.0));
            painter->drawLine(QPointF(px, plot.top()), QPointF(px, plot.bottom()));
            QString text;
            QColor text_color = Theme::TEXT_SECONDARY;
            if (fractional_digits_of(value) > kMicroDecimals) {
                double scaled = value * micro_scale;
                double frac = scaled - std::floor(scaled);
                long long micro = static_cast<long long>(std::llround(frac * 1000.0)) % 1000;
                text = QString(".%1").arg(micro, 3, 10, QChar('0'));
                text_color = Theme::TEXT_DIM;
            } else {
                text = format_axis_number_fixed(value, digits);
            }
            double width = static_cast<double>(metrics.horizontalAdvance(text));
            QRectF text_rect(px - width / 2 - 2, xaxis.top() + 3, width + 4, 15);
            if (!text_rect.intersects(unit_rect)) {
                draw_shadow_text(painter, text, text_rect, text_color);
            }
        }
        value += step;
    }

    draw_shadow_text(painter, QString("Time (s)"), unit_rect, Theme::TEXT_DIM);
}

void TimelineWidget::_draw_rows(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                                 double row_height, double key_width, double source_width, const QFontMetrics& metrics) {
    QFont font = this->font();
    font.setPointSizeF(std::min(8.5, std::max(5.0, row_height * 0.72)));
    painter->setFont(font);

    for (size_t index = 0; index < rows.size(); ++index) {
        const auto& info = rows[index];
        double top = plot.top() + static_cast<double>(index) * row_height;
        double bottom = top + row_height;

        painter->setPen(QPen(Theme::GRID_Y, 1.0));
        painter->drawLine(QPointF(plot.left(), bottom), QPointF(plot.right(), bottom));

        if (static_cast<int>(index) == _hover_row) {
            painter->fillRect(
                QRectF(plot.left(), top, plot.width(), row_height),
                Theme::color_with_alpha(QColor("#3A3A3A"), 90)
            );
        }

        painter->save();
        painter->setClipRect(QRectF(0, top, plot.left() - 2, row_height));
        painter->setPen(static_cast<int>(index) == _hover_row ? Theme::HOVER_TEXT : Theme::TEXT_SECONDARY);
        painter->drawText(
            QRectF(plot.left() - key_width - 2, top, key_width, row_height),
            Qt::AlignRight | Qt::AlignVCenter,
            QString::fromStdString(info.input_id.key)
        );
        painter->restore();
    }

    if (source_width > 0 && recording) {
        std::vector<int64_t> visible_sources;
        for (const auto& info : rows) {
            if (std::find(visible_sources.begin(), visible_sources.end(), info.input_id.source_id) == visible_sources.end()) {
                visible_sources.push_back(info.input_id.source_id);
            }
        }

        size_t start = 0;
        while (start < rows.size()) {
            int64_t source_id = rows[start].input_id.source_id;
            size_t end = start;
            while (end < rows.size() && rows[end].input_id.source_id == source_id) {
                ++end;
            }
            double top = plot.top() + static_cast<double>(start) * row_height;
            double bottom = plot.top() + static_cast<double>(end) * row_height;
            double middle = (top + bottom) / 2.0;

            auto it = recording->sources.find(source_id);
            QString name = (it != recording->sources.end()) ? QString::fromStdString(it->second.name)
                                                           : QString("Device %1").arg(source_id);
            double group_height = std::max(1.0, bottom - top);
            QFont source_font = this->font();
            source_font.setPointSizeF(std::min(8.5, std::max(5.5, group_height * 0.55)));
            QFontMetrics source_metrics(source_font);
            double text_height = static_cast<double>(source_metrics.height());
            double text_width = std::max(0.0, source_width - 14);
            QRectF text_rect(12.0, middle - text_height / 2, text_width, text_height);

            painter->save();
            painter->setFont(source_font);
            painter->setClipRect(text_rect);
            painter->setPen(Theme::TEXT_SECONDARY);
            painter->drawText(
                text_rect,
                Qt::AlignRight | Qt::AlignVCenter,
                source_metrics.elidedText(name, Qt::ElideRight, static_cast<int>(text_width))
            );
            painter->restore();

            double separator_x = plot.left() - key_width - 4;
            painter->setPen(QPen(Theme::GRID_Y, 1.0));
            painter->drawLine(QPointF(separator_x, top + 2), QPointF(separator_x, bottom - 2));

            start = end;
        }
    }
}

void TimelineWidget::_draw_holds(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                                  double row_height, double x_min, double x_max, double x_unit) {
    double duration = full_span();
    for (size_t index = 0; index < rows.size(); ++index) {
        const auto& info = rows[index];
        QColor color = QColor::fromRgba(static_cast<uint>(info.color));
        double top = plot.top() + static_cast<double>(index) * row_height + 1;
        double height = std::max(1.0, row_height - 2);

        QLinearGradient gradient(plot.left(), top, plot.left(), top + height);
        gradient.setColorAt(0.0, Theme::color_with_alpha(Theme::blend(color, QColor("#000000"), 0.98), 240));
        gradient.setColorAt(0.1, Theme::color_with_alpha(color, 250));
        gradient.setColorAt(0.3, Theme::color_with_alpha(color, 250));
        gradient.setColorAt(1.0, Theme::color_with_alpha(Theme::blend(color, QColor("#000000"), 0.95), 215));
        painter->setPen(Qt::NoPen);
        painter->setBrush(gradient);

        auto intervals = _hold_intervals(info, duration);
        for (const auto& [start, end] : intervals) {
            if (end <= x_min || start >= x_max) continue;
            double s = std::max(start, x_min);
            double e = std::min(end, x_max);
            double x1 = plot.left() + (s - x_min) * x_unit;
            double x2 = plot.left() + (e - x_min) * x_unit;
            painter->drawRect(QRectF(x1, top, std::max(1.0, x2 - x1), height));
        }
    }
}

void TimelineWidget::_draw_hover(QPainter* painter, const QRectF& plot, const std::vector<InputInfo>& rows,
                                  double row_height, double x_min, double x_unit) {
    if (!_hover_event || _hover_row < 0 || _hover_row >= static_cast<int>(rows.size())) return;

    const auto& info = rows[_hover_row];
    double row_top = plot.top() + static_cast<double>(_hover_row) * row_height;
    double center_y = row_top + row_height / 2.0;
    double px = plot.left() + (_hover_event->time - x_min) * x_unit;
    if (!(plot.left() <= px && px <= plot.right())) return;

    painter->setPen(QPen(QColor("#FFFFFF"), 1.0));
    painter->setBrush(Theme::color_with_alpha(QColor::fromRgba(static_cast<uint>(info.color)), 200));
    painter->drawEllipse(QPointF(px, center_y), 3.0, 3.0);

    QString text = QString("%1  %2 s  %3")
        .arg(QString::fromStdString(info.input_id.key))
        .arg(_hover_event->time, 0, 'g', 6)
        .arg(_hover_event->pressed ? QString("down") : QString("up"));

    QFont font = this->font();
    font.setPointSizeF(8.0);
    painter->setFont(font);
    QFontMetrics metrics(font);
    double width = static_cast<double>(metrics.horizontalAdvance(text)) + 10;
    double height = static_cast<double>(metrics.height()) + 4;
    double x = px + 9;
    double y = center_y - height - 6;
    if (x + width > plot.right()) x = px - width - 9;
    if (y < plot.top()) y = center_y + 6;
    x = clamp_value(x, plot.left() + 1, plot.right() - width - 1);
    y = clamp_value(y, plot.top() + 1, plot.bottom() - height - 1);

    QRectF bubble(x, y, width, height);
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::color_with_alpha(Theme::WINDOW_BG, 220));
    painter->drawRect(bubble);
    painter->setPen(Theme::TEXT);
    painter->drawText(bubble, Qt::AlignCenter, text);
}

void TimelineWidget::_draw_scrollbar(QPainter* painter, const QRectF& thumb) {
    if (!recording) return;
    QColor color = _scroll_drag ? Theme::SCROLLBAR_CAPTURED :
                 (_scroll_hover ? Theme::SCROLLBAR_HOVER : Theme::SCROLLBAR);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRect(thumb);
}

int TimelineWidget::_find_event(int row_index, double x_value) const {
    auto rows = visible_rows_impl();
    if (row_index < 0 || row_index >= static_cast<int>(rows.size())) return -1;
    const auto& events = rows[row_index].events;
    if (events.empty()) return -1;

    // Binary search
    int lo = 0, hi = static_cast<int>(events.size()) - 1;
    int pos = static_cast<int>(events.size());
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (events[mid].time >= x_value) {
            pos = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    int best = -1;
    double best_dist = std::numeric_limits<double>::max();
    for (int idx : {pos - 1, pos}) {
        if (idx >= 0 && idx < static_cast<int>(events.size())) {
            double dist = std::abs(events[idx].time - x_value);
            if (dist < best_dist) {
                best_dist = dist;
                best = idx;
            }
        }
    }
    return best;
}

int TimelineWidget::_label_hit_row(const QPointF& position, const QRectF& plot, double key_width) const {
    (void)key_width;
    if (position.x() >= plot.left() - 4) return -1;
    auto rows = visible_rows_impl();
    if (rows.empty()) return -1;
    double row_height = plot.height() / static_cast<double>(rows.size());
    int index = static_cast<int>((position.y() - plot.top()) / std::max(row_height, 1e-9));
    return (index >= 0 && index < static_cast<int>(rows.size())) ? index : -1;
}

void TimelineWidget::_start_row_drag(int visible_row, QMouseEvent* event) {
    auto rows = visible_rows_impl();
    if (visible_row < 0 || visible_row >= static_cast<int>(rows.size()) || !recording) return;

    const InputInfo& info = rows[visible_row];
    QFont font = this->font();
    font.setPointSizeF(8.5);
    QString label = QString::fromStdString(info.input_id.key);

    QPixmap pixmap(label.size() * 9 + 12, 20);
    pixmap.fill(Theme::TEXTBOX_BG);
    {
        QPainter p(&pixmap);
        p.setFont(font);
        p.setPen(Theme::TEXT);
        p.drawText(pixmap.rect(), Qt::AlignCenter, label);
    }

    auto* mime = new QMimeData;
    mime->setData("application/x-kbi-row", QByteArray::number(visible_row));
    QDrag* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(pixmap);
    drag->setHotSpot(event->position().toPoint());
    // exec() is modal; the drop lands in dropEvent() on this widget.
    drag->exec(Qt::MoveAction);
}

void TimelineWidget::_reorder_row(int visible_from, int visible_to) {
    if (!recording || visible_from < 0 || visible_to < 0 || visible_from == visible_to) return;
    auto rows = visible_rows_impl();
    if (visible_from >= static_cast<int>(rows.size()) || visible_to >= static_cast<int>(rows.size())) return;

    InputId dragged = rows[visible_from].input_id;
    InputId target = rows[visible_to].input_id;
    if (dragged == target) return;

    auto& inputs = recording->inputs;
    auto it_from = std::find_if(inputs.begin(), inputs.end(), [&](const InputInfo& i) {
        return i.input_id == dragged;
    });
    if (it_from == inputs.end()) return;

    InputInfo item = *it_from;
    bool downward = visible_from < visible_to;
    inputs.erase(it_from);
    auto it_target = std::find_if(inputs.begin(), inputs.end(), [&](const InputInfo& i) {
        return i.input_id == target;
    });
    if (it_target == inputs.end()) {
        inputs.push_back(item);
    } else {
        // Moving down inserts after the target row, moving up inserts before.
        inputs.insert(it_target + (downward ? 1 : 0), item);
    }

    emit changed();
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() == this && event->mimeData()->hasFormat("application/x-kbi-row")) {
        event->acceptProposedAction();
    }
}

void TimelineWidget::dropEvent(QDropEvent* event) {
    if (event->source() != this || !event->mimeData()->hasFormat("application/x-kbi-row")) return;
    int from = event->mimeData()->data("application/x-kbi-row").toInt();
    TimelineGeometry geo = _geometry();
    int to = _label_hit_row(event->position(), geo.plot, 0.0);
    if (to < 0) {
        // Dropping over the bars area still reorders to the row under the cursor.
        auto rows = visible_rows_impl();
        if (!rows.empty() && geo.plot.contains(event->position())) {
            double row_height = geo.plot.height() / static_cast<double>(rows.size());
            int index = static_cast<int>((event->position().y() - geo.plot.top()) / std::max(row_height, 1e-9));
            if (index >= 0 && index < static_cast<int>(rows.size())) to = index;
        }
    }
    event->acceptProposedAction();
    if (to >= 0) _reorder_row(from, to);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        _show_menu(event->position());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !recording) {
        QWidget::mousePressEvent(event);
        return;
    }
    TimelineGeometry geo = _geometry();
    QPointF position = event->position();
    if (geo.thumb.contains(position)) {
        _scroll_drag = true;
        _scroll_origin = position.x();
        _scroll_viewport = viewport;
        setCursor(Qt::SizeHorCursor);
    } else if (geo.scroll_area.contains(position)) {
        double page = 1.0 / std::max(zoom, 1e-9);
        if (position.x() < geo.thumb.left()) {
            viewport = clamp_value(viewport - page, 0.0, 1.0 - page);
        } else if (position.x() > geo.thumb.right()) {
            viewport = clamp_value(viewport + page, 0.0, 1.0 - page);
        }
    } else if (position.x() < geo.plot.left() - 4) {
        // Label area: press arms a row-drag (upstream YText drag, YTextIntent.Drag).
        int row = _label_hit_row(position, geo.plot, 0.0);
        if (row >= 0) {
            _pending_drag_row = row;
            _pending_drag_origin = position;
        }
    } else if (geo.plot.contains(position) || geo.xaxis.contains(position)) {
        _pan_origin = position;
        _pan_viewport = viewport;
        setCursor(Qt::ClosedHandCursor);
    }
    update();
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!recording) return;
    TimelineGeometry geo = _geometry();
    QPointF position = event->position();

    if (_pending_drag_row >= 0) {
        QPointF delta = position - _pending_drag_origin;
        if (QLineF(QPointF(0, 0), delta).length() > 8.0) {
            int row = _pending_drag_row;
            _pending_drag_row = -1;
            _start_row_drag(row, event);
        }
        event->accept();
        return;
    }

    if (_scroll_drag) {
        double delta = (position.x() - _scroll_origin) / std::max(geo.scroll_area.width(), 1.0);
        double limit = std::max(0.0, 1.0 - 1.0 / std::max(zoom, 1e-9));
        viewport = clamp_value(_scroll_viewport + delta, 0.0, limit);
        update();
        event->accept();
        return;
    }

    if (!_pan_origin.isNull()) {
        double delta = position.x() - _pan_origin.x();
        double limit = std::max(0.0, 1.0 - 1.0 / std::max(zoom, 1e-9));
        viewport = clamp_value(
            _pan_viewport - delta / std::max(geo.plot.width(), 1.0) / std::max(zoom, 1e-9),
            0.0, limit
        );
        update();
        event->accept();
        return;
    }

    int row = (geo.plot.left() <= position.x() && position.x() <= geo.plot.right())
        ? _row_at(position.y(), geo.plot) : -1;
    auto [x_min, x_max] = _x_range();
    double x_value = 0.0;
    bool has_x = false;
    if (row >= 0 && geo.plot.width() > 0) {
        x_value = x_min + (position.x() - geo.plot.left()) / geo.plot.width() * (x_max - x_min);
        has_x = true;
    }

    std::optional<Event> found = std::nullopt;
    if (has_x) {
        int idx = _find_event(row, x_value);
        if (idx >= 0) {
            double max_distance = std::max(6.0, geo.plot.width() / std::max(zoom, 1e-9) * 0.02);
            auto rows = visible_rows_impl();
            if (std::abs(rows[row].events[idx].time - x_value) > max_distance) {
                found = std::nullopt;
            } else {
                found = rows[row].events[idx];
            }
        }
    }

    bool found_changed = false;
    if (found && _hover_event) {
        found_changed = (found->time != _hover_event->time ||
                         found->pressed != _hover_event->pressed ||
                         found->input_id.source_id != _hover_event->input_id.source_id ||
                         found->input_id.key != _hover_event->input_id.key);
    } else {
        found_changed = (found.has_value() != _hover_event.has_value());
    }

    bool scroll_hover = geo.thumb.contains(position);
    if (row != _hover_row || found_changed || scroll_hover != _scroll_hover) {
        _hover_row = row;
        _hover_event = found;
        _scroll_hover = scroll_hover;
        update();
    }

    if (geo.thumb.contains(position)) {
        setCursor(Qt::SizeHorCursor);
    } else if (geo.plot.contains(position)) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        _pan_origin = QPointF();
        _scroll_drag = false;
        _pending_drag_row = -1;
        unsetCursor();
        update();
        event->accept();
    }
}

void TimelineWidget::leaveEvent(QEvent* event) {
    _hover_row = -1;
    _hover_event = std::nullopt;
    _scroll_hover = false;
    unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
    if (!recording) return;
    TimelineGeometry geo = _geometry();
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
    viewport += fraction * (1.0 / old_zoom - 1.0 / new_zoom);
    zoom = new_zoom;
    double limit = std::max(0.0, 1.0 - 1.0 / zoom);
    viewport = clamp_value(viewport, 0.0, limit);
    update();
    event->accept();
}

void TimelineWidget::_show_menu(const QPointF& position) {
    if (!recording) return;

    TimelineGeometry geo = _geometry();
    int row = _row_at(position.y(), geo.plot);
    auto rows = visible_rows_impl();
    std::optional<InputInfo> info = std::nullopt;
    if (row >= 0 && row < static_cast<int>(rows.size())) {
        info = rows[row];
    }

    std::set<int64_t> visible_sources;
    for (const auto& item : rows) {
        visible_sources.insert(item.input_id.source_id);
    }

    auto it = recording->sources.find(info ? info->input_id.source_id : -1);
    std::optional<SourceInfo> source = std::nullopt;
    if (it != recording->sources.end()) source = it->second;

    QMenu menu(this);
    QAction* change = menu.addAction("Change Color…");
    QAction* reset = menu.addAction("Reset Color");
    menu.addSeparator();
    QAction* hide_input = menu.addAction("Hide Input");
    QAction* hide_device = menu.addAction("Hide Device");
    menu.addSeparator();
    QAction* sort_device = menu.addAction("Sort by Device");
    QAction* show_all = menu.addAction("Show All Inputs");
    menu.addSeparator();
    QAction* copy_interface = menu.addAction("Copy Interface");

    change->setEnabled(info.has_value());
    reset->setEnabled(info.has_value() && info->color != Theme::DEFAULT_INPUT_COLOR.rgba());
    hide_input->setEnabled(info.has_value() && rows.size() > 1);
    hide_device->setEnabled(info.has_value() && visible_sources.size() > 1);
    sort_device->setEnabled(visible_sources.size() > 1);
    bool any_hidden = false;
    for (const auto& item : recording->inputs) {
        if (!item.visible) { any_hidden = true; break; }
    }
    show_all->setEnabled(any_hidden);
    copy_interface->setEnabled(source.has_value() && !source->device_interface.empty());

    QAction* chosen = menu.exec(mapToGlobal(position.toPoint()));
    if (!chosen) return;

    if (chosen == change && info) {
        QColor color = QColorDialog::getColor(
            QColor::fromRgba(static_cast<uint>(info->color)), this, "Input Color"
        );
        if (color.isValid()) {
            for (auto& item : recording->inputs) {
                if (item.input_id == info->input_id) {
                    item.color = color.rgba();
                    break;
                }
            }
            emit changed();
            update();
        }
    } else if (chosen == reset && info) {
        for (auto& item : recording->inputs) {
            if (item.input_id == info->input_id) {
                item.color = Theme::DEFAULT_INPUT_COLOR.rgba();
                break;
            }
        }
        emit changed();
        update();
    } else if (chosen == hide_input && info) {
        for (auto& item : recording->inputs) {
            if (item.input_id == info->input_id) {
                item.visible = false;
                break;
            }
        }
        emit analysis_requested();
        update();
    } else if (chosen == hide_device && info) {
        for (auto& item : recording->inputs) {
            if (item.input_id.source_id == info->input_id.source_id) {
                item.visible = false;
            }
        }
        emit analysis_requested();
        update();
    } else if (chosen == sort_device) {
        recording->sort_inputs_by_device();
        emit changed();
        update();
    } else if (chosen == show_all) {
        for (auto& item : recording->inputs) {
            item.visible = true;
        }
        emit analysis_requested();
        update();
    } else if (chosen == copy_interface && source) {
        QApplication::clipboard()->setText(QString::fromStdString(source->device_interface));
        emit message("Copied device interface.");
    }
}

} // namespace keyboard_inspector
