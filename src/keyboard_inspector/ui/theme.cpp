#include "theme.h"

#include <QFontDatabase>
#include <QProxyStyle>
#include <QStyle>
#include <cmath>

namespace keyboard_inspector {

namespace {

// Fusion, but with QMenu forced into scroll mode: an oversized popup (the
// device list on a short screen, for example) is then bounded to the
// available screen area and shows scroll arrows instead of overflowing off
// the edge of the screen.
class MenuScrollStyle : public QProxyStyle {
public:
    MenuScrollStyle() : QProxyStyle("Fusion") {}

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* return_data = nullptr) const override {
        if (hint == QStyle::SH_Menu_Scrollable) return 1;
        return QProxyStyle::styleHint(hint, option, widget, return_data);
    }
};

} // namespace

const QColor Theme::WINDOW_BG("#2B2B2B");
const QColor Theme::TOPBAR_BG("#3C3F41");
const QColor Theme::TEXT("#DCDCDC");
const QColor Theme::TEXT_SECONDARY("#A0A0A0");
const QColor Theme::TEXT_DIM("#808080");
const QColor Theme::CHART_BG("#070707");
const QColor Theme::GRID_X("#141414");
const QColor Theme::GRID_Y("#2C2C2C");
const QColor Theme::TIME_LINE("#418CF0");
const QColor Theme::FREQ_LINE("#F07800");
const QColor Theme::TEXTBOX_BG("#45494A");
const QColor Theme::SCROLLBAR("#5C5C5C");
const QColor Theme::SCROLLBAR_HOVER("#7A8084");
const QColor Theme::SCROLLBAR_CAPTURED("#9EB1C3");
const QColor Theme::BANNER_BG("#3A2F1B");
const QColor Theme::BANNER_BORDER("#8A6D2F");
const QColor Theme::BANNER_TEXT("#E8D8A8");
const QColor Theme::RECORDING_ACCENT("#E05A4F");
const QColor Theme::BORDER("#2C2C2C");
const QColor Theme::HOVER_TEXT("#7FC8F8");
const QColor Theme::DEFAULT_INPUT_COLOR("#A9A9A9");

const char* Theme::QSS = R"(
QWidget {
    background-color: #2B2B2B;
    color: #DCDCDC;
    selection-background-color: #4B6EAF;
    selection-color: #FFFFFF;
}
QFrame#topBar {
    background-color: #3C3F41;
    border-bottom: 1px solid #1E1E1E;
}
QToolButton {
    background-color: transparent;
    border: 1px solid transparent;
    padding: 2px 7px;
    color: #DCDCDC;
}
QToolButton:hover, QToolButton:pressed {
    background-color: #4A4E50;
    border: 1px solid #5C5C5C;
}
QPushButton {
    background-color: #3C3F41;
    border: 1px solid #5C5C5C;
    padding: 2px 10px;
    color: #DCDCDC;
}
QPushButton:hover { background-color: #474B4D; }
QPushButton:pressed { background-color: #313436; }
QPushButton:disabled { color: #777777; border-color: #444444; }
QPushButton#recordButton {
    border: 1px solid #6A6A6A;
    min-width: 108px;
    min-height: 18px;
}
QPushButton#recordButton[recording="true"] {
    color: #E05A4F;
    border: 1px solid #E05A4F;
}
QPushButton#smallButton {
    min-width: 22px;
    max-width: 29px;
    padding: 1px 2px;
}
QLineEdit, QSpinBox {
    background-color: #45494A;
    border: 1px solid #5C5C5C;
    padding: 1px 4px;
    color: #DCDCDC;
    selection-background-color: #4B6EAF;
}
QCheckBox { spacing: 5px; background: transparent; }
QCheckBox::indicator {
    width: 12px; height: 12px;
    border: 1px solid #6A6A6A;
    background-color: #3C3F41;
}
QCheckBox::indicator:checked { background-color: #7A8084; }
QMenu {
    background-color: #3C3F41;
    border: 1px solid #2C2C2C;
    padding: 3px 0px;
}
QMenu::item { padding: 4px 26px 4px 20px; background: transparent; }
QMenu::item:selected { background-color: #365880; }
QMenu::item:disabled { color: #777777; }
QMenu::separator { height: 1px; background: #2C2C2C; margin: 4px 8px; }
QFrame#banner {
    background-color: #3A2F1B;
    border: 1px solid #8A6D2F;
}
QFrame#analysisStrip { background-color: #2B2B2B; }
QSplitter::handle { background-color: #2B2B2B; }
QSplitter::handle:hover { background-color: #3C3F41; }
QToolTip {
    background-color: #3C3F41;
    color: #DCDCDC;
    border: 1px solid #5C5C5C;
}
)";

QFont Theme::choose_font() {
    QStringList families = {"Noto Sans", "DejaVu Sans", "Liberation Sans", "Sans Serif"};
    QFont font;
    for (const QString& name : families) {
        QFont f(name);
        if (QFontDatabase().families().contains(name)) {
            font = f;
            break;
        }
    }
    font.setPointSizeF(9.0);
    return font;
}

void Theme::apply_theme(QApplication* app) {
    app->setStyle(new MenuScrollStyle);
    app->setFont(choose_font());
    app->setStyleSheet(QSS);
}

QColor Theme::blend(const QColor& first, const QColor& second, double amount) {
    amount = std::max(0.0, std::min(1.0, amount));
    return QColor(
        static_cast<int>(first.red() + (second.red() - first.red()) * amount),
        static_cast<int>(first.green() + (second.green() - first.green()) * amount),
        static_cast<int>(first.blue() + (second.blue() - first.blue()) * amount),
        static_cast<int>(first.alpha() + (second.alpha() - first.alpha()) * amount)
    );
}

QColor Theme::color_with_alpha(const QColor& color, int alpha) {
    QColor out(color);
    out.setAlpha(std::max(0, std::min(255, alpha)));
    return out;
}

QColor Theme::event_count_color(int count) {
    count = std::max(0, count);
    struct Stop { double n; QColor c; };
    static const Stop stops[] = {
        {50.0, QColor("#EE0000")},
        {200.0, QColor("#EA7700")},
        {600.0, QColor("#DDDD00")},
        {1500.0, QColor("#00EE00")},
    };
    if (count <= stops[0].n) return stops[0].c;
    for (size_t i = 0; i + 1 < sizeof(stops)/sizeof(stops[0]); ++i) {
        if (count <= stops[i+1].n) {
            double amount = (count - stops[i].n) / (stops[i+1].n - stops[i].n);
            return blend(stops[i].c, stops[i+1].c, amount);
        }
    }
    return stops[sizeof(stops)/sizeof(stops[0]) - 1].c;
}

int Theme::argb_to_qcolor(int value) {
    return qRgba(
        (value >> 16) & 0xFF,
        (value >> 8) & 0xFF,
        value & 0xFF,
        (value >> 24) & 0xFF
    );
}

int Theme::qcolor_to_argb(const QColor& color) {
    return (
        (color.alpha() << 24) |
        (color.red() << 16) |
        (color.green() << 8) |
        color.blue()
    );
}

double Theme::nice_step(double span, int target) {
    if (!std::isfinite(span) || span <= 0 || target <= 0) return 1.0;
    double raw = span / target;
    int exponent = static_cast<int>(std::floor(std::log10(raw)));
    double base = raw / std::pow(10.0, exponent);
    double nice;
    if (base <= 1.0) nice = 1.0;
    else if (base <= 2.0) nice = 2.0;
    else if (base <= 5.0) nice = 5.0;
    else nice = 10.0;
    return nice * std::pow(10.0, exponent);
}

double Theme::screen_interval_seconds(int index) {
    // Upstream MainForm.ScreenInterval.
    if (index <= 6) return std::pow(2.0, index - 3) / 1000.0;
    if (index <= 15) {
        double value = std::pow(10.0, (index / 3) - 4);
        if (index % 3 == 1) value *= 2;
        if (index % 3 == 2) value *= 5;
        return value;
    }
    if (index <= 18) return std::pow(2.0, index - 17) * 60.0;
    if (index <= 20) return std::pow(2.0, index - 19) * 300.0;
    return std::pow(2.0, index - 21) * 1800.0;
}

int Theme::fraction_digits(double step, int maximum) {
    if (step <= 0) return 0;
    int digits = static_cast<int>(std::ceil(-std::log10(step)));
    return std::max(0, std::min(digits, maximum));
}

std::string Theme::format_axis_value(double value, double step, const std::string& unit) {
    int digits = fraction_digits(step, unit == "s" ? 6 : 3);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", digits, value);
    std::string text(buf);
    if (text.find(".") != std::string::npos) {
        size_t end = text.find_last_not_of('0');
        if (text[end] == '.') end--;
        text = text.substr(0, end + 1);
    }
    if (text == "" || text == "-0") text = "0";
    return text;
}

std::string Theme::format_seconds(double seconds, double step) {
    return format_axis_value(seconds, step, "s");
}

} // namespace keyboard_inspector
