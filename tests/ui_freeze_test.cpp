#include <catch2/catch_all.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QColor>

#include <vector>
#include "../src/keyboard_inspector/ui/chart.h"
#include "../src/keyboard_inspector/ui/main_window.h"

using namespace keyboard_inspector;

namespace {

QApplication* ensure_app() {
    static int argc = 1;
    static char name[] = "keyboard-inspector-ui-tests";
    static char* argv[] = {name, nullptr};
    // Intentionally leaked: destroying the QApplication during static
    // destruction after exit() segfaults under some platform plugins
    // (observed with Qt 6.4.2's offscreen plugin).
    static QApplication* app = nullptr;
    if (!app) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        app = new QApplication(argc, argv);
    }
    return app;
}

QCheckBox* find_checkbox(QWidget& parent, const QString& text) {
    for (auto* box : parent.findChildren<QCheckBox*>()) {
        if (box->text() == text) return box;
    }
    return nullptr;
}

void pump_events(int rounds = 5) {
    for (int i = 0; i < rounds; ++i) QCoreApplication::processEvents();
}

} // namespace

TEST_CASE("Starting a recording with Freeze checked and no prior result is safe") {
    ensure_app();

    MainWindow window;
    auto* record = window.findChild<QPushButton*>("recordButton");
    REQUIRE(record != nullptr);

    if (!record->isEnabled()) {
        SKIP("no readable input device in this environment");
    }

    auto* freeze = find_checkbox(window, "Freeze");
    REQUIRE(freeze != nullptr);
    freeze->setChecked(true);

    record->click();
    REQUIRE(record->text() == "Stop Recording");

    record->click();
    REQUIRE(record->text() == "Start Recording");
}

TEST_CASE("Fully zoomed-out chart keeps a narrow spike visible") {
    ensure_app();

    std::vector<double> values(4000, 0.0);
    values[4] = 10.0;  // single 1 ms spike on a 4000-bin histogram

    ChartData data{values, 0.25, 10.0, "ms", false};
    ChartWidget chart("spike", QColor(65, 140, 240), 0.0);
    chart.set_data(data);  // initial_span 0 -> full view, 999.75 ms
    chart.resize(320, 220);
    QImage image = chart.grab().toImage();

    const int plot_left = 20;
    const int plot_top = 19;
    const int plot_bottom = 220 - 38;
    int hits = 0;
    for (int y = plot_top; y < plot_top + (plot_bottom - plot_top) / 3; ++y) {
        for (int x = plot_left - 2; x <= plot_left + 6; ++x) {
            QColor c = image.pixelColor(x, y);
            if (c.blue() > 120 && c.blue() > c.red() + 40) ++hits;
        }
    }
    REQUIRE(hits > 0);
}

TEST_CASE("Capture and device dropdowns stay within the screen") {
    ensure_app();

    MainWindow window;
    window.show();
    pump_events();

    QToolButton* capture_button = nullptr;
    for (QToolButton* button : window.findChildren<QToolButton*>()) {
        if (button->text() == "Capture") capture_button = button;
    }
    REQUIRE(capture_button != nullptr);
    REQUIRE(capture_button->menu() != nullptr);
    QMenu* capture_menu = capture_button->menu();

    // QToolButton::showMenu() blocks until the menu closes, so popup the
    // menu directly at the button like the button itself does.
    capture_menu->popup(capture_button->mapToGlobal(QPoint(0, capture_button->height())));
    pump_events();
    REQUIRE(capture_menu->isVisible());

    const QRect available = capture_menu->screen()->availableGeometry();
    CHECK(capture_menu->width() <= available.width());
    CHECK(capture_menu->height() <= available.height());
    CHECK(capture_menu->frameGeometry().right() <= available.right());
    CHECK(capture_menu->frameGeometry().bottom() <= available.bottom());

    // Long labels carry the full text in a property and the tooltip, and are
    // elided when the menu opens so they cannot widen it past the screen.
    QAction* usb_action = nullptr;
    for (QAction* action : capture_menu->actions()) {
        if (action->property("fullText").toString().startsWith("USB descriptor")) {
            usb_action = action;
        }
    }
    REQUIRE(usb_action != nullptr);
    const QString full_text = usb_action->property("fullText").toString();
    CHECK(usb_action->text().size() <= full_text.size());
    CHECK(usb_action->toolTip() == full_text);

    QMenu* devices_menu = nullptr;
    QAction* devices_action = nullptr;
    for (QObject* child : capture_menu->children()) {
        if (auto* menu = qobject_cast<QMenu*>(child)) devices_menu = menu;
    }
    for (QAction* action : capture_menu->actions()) {
        if (action->text() == "Devices") devices_action = action;
    }
    REQUIRE(devices_menu != nullptr);
    REQUIRE(devices_action != nullptr);

    capture_menu->hide();
    // Pop the submenu from its triggering item, like QMenu does on hover.
    devices_menu->popup(
        capture_menu->mapToGlobal(capture_menu->actionGeometry(devices_action).topRight()));
    pump_events();
    REQUIRE(devices_menu->isVisible());
    CHECK(devices_menu->width() <= available.width());
    CHECK(devices_menu->height() <= available.height());
    CHECK(devices_menu->frameGeometry().right() <= available.right());
    CHECK(devices_menu->frameGeometry().bottom() <= available.bottom());

    devices_menu->hide();
    pump_events();
}
