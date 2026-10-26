#pragma once

#include <QColor>
#include <QFont>
#include <QApplication>
#include <cmath>
#include <string>

namespace keyboard_inspector {

struct Theme {
    static const QColor WINDOW_BG;
    static const QColor TOPBAR_BG;
    static const QColor TEXT;
    static const QColor TEXT_SECONDARY;
    static const QColor TEXT_DIM;
    static const QColor CHART_BG;
    static const QColor GRID_X;
    static const QColor GRID_Y;
    static const QColor TIME_LINE;
    static const QColor FREQ_LINE;
    static const QColor TEXTBOX_BG;
    static const QColor SCROLLBAR;
    static const QColor SCROLLBAR_HOVER;
    static const QColor SCROLLBAR_CAPTURED;
    static const QColor BANNER_BG;
    static const QColor BANNER_BORDER;
    static const QColor BANNER_TEXT;
    static const QColor RECORDING_ACCENT;
    static const QColor BORDER;
    static const QColor HOVER_TEXT;
    static const QColor DEFAULT_INPUT_COLOR;

    static const char* QSS;

    static QFont choose_font();
    static void apply_theme(QApplication* app);

    static QColor blend(const QColor& first, const QColor& second, double amount);
    static QColor color_with_alpha(const QColor& color, int alpha);

    static QColor event_count_color(int count);

    static int argb_to_qcolor(int value);
    static int qcolor_to_argb(const QColor& color);

    static double nice_step(double span, int target = 6);
    static double screen_interval_seconds(int index);
    static int fraction_digits(double step, int maximum = 6);
    static std::string format_axis_value(double value, double step, const std::string& unit);
    static std::string format_seconds(double seconds, double step = 1.0);
};

} // namespace keyboard_inspector
