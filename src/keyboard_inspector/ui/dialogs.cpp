#include "dialogs.h"
#include "theme.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QRegularExpression>

namespace keyboard_inspector {

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QString("About ") + QString(APP_NAME));
    setModal(true);
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto* title = new QLabel(QString("<b>") + QString(APP_NAME) + QString("</b> ") + QString(APP_VERSION));
    title->setStyleSheet(QString("color: ") + Theme::TEXT.name() + QString("; font-size: 12pt;"));
    layout->addWidget(title);

    auto* text = new QLabel(
        "A Linux port of Keyboard Inspector by mat1jaczyyy. It records Linux input "
        "events, builds the same three timing histograms and FFT charts, and reports "
        "a confidence-gated rate estimate."
    );
    text->setWordWrap(true);
    layout->addWidget(text);

    auto* measurement = new QLabel(
        "<b>Measurement note:</b> the estimated rate is derived from captured event "
        "timestamps. It is not the USB descriptor interval. Descriptor values are "
        "configured capabilities, shown separately and never substituted for a "
        "measurement."
    );
    measurement->setWordWrap(true);
    measurement->setStyleSheet(QString("color: ") + Theme::TEXT_SECONDARY.name());
    layout->addWidget(measurement);

    auto* license_text = new QLabel(
        "License: GPL-3.0-or-later for this port. Upstream Keyboard Inspector is "
        "BSD-3-Clause; its copyright and notice are preserved. The Qt 6 bindings "
        "are GPL-3.0 or commercial."
    );
    license_text->setWordWrap(true);
    license_text->setStyleSheet(QString("color: ") + Theme::TEXT_SECONDARY.name());
    layout->addWidget(license_text);

    auto* upstream = new QPushButton("Open upstream project");
    upstream->setFixedHeight(24);
    connect(upstream, &QPushButton::clicked, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/mat1jaczyyy/Keyboard-Inspector"));
    });
    layout->addWidget(upstream, 0, Qt::AlignmentFlag::AlignLeft);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::StandardButton::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

// Format.cs DefaultURLFilter: absolute http(s) URL whose path ends with the
// format's extension.
static bool default_url_filter(const QString& url, const QStringList& extensions, QString* result) {
    QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.isRelative()) return false;
    QString scheme = parsed.scheme().toLower();
    if (scheme != "http" && scheme != "https") return false;
    QString path = parsed.path();
    for (const QString& extension : extensions) {
        if (path.endsWith(extension, Qt::CaseInsensitive)) {
            if (result) *result = parsed.toString(QUrl::FullyEncoded);
            return true;
        }
    }
    return false;
}

// FileSystem.cs:38-43 TETR.IO URL filter.
static bool tetrio_url_filter(const QString& url, QString* result) {
    QString transformed = url;
    transformed.replace("https://tetr.io/#", "", Qt::CaseInsensitive);
    transformed.replace("tetrio://", "", Qt::CaseInsensitive);

    static const QRegularExpression matcher("^[Rr]:.+$");
    auto match = matcher.match(transformed);
    if (!match.hasMatch()) return false;

    transformed.replace("R:", "").replace("r:", "");
    QString id = transformed.split('@').first();

    if (result) {
        *result = "https://inoue.szy.lol/api/replay/" +
                  QString::fromUtf8(QUrl::toPercentEncoding(id));
    }
    return true;
}

ImportFormat classify_import_url(const QString& url, QString* download_url) {
    // Order matters: the kbi format is checked first (upstream Formats order).
    if (default_url_filter(url, {".kbi"}, download_url)) return ImportFormat::Kbi;
    if (tetrio_url_filter(url, download_url)) return ImportFormat::Tetrio;
    return ImportFormat::None;
}

const char* import_format_name(ImportFormat format) {
    switch (format) {
        case ImportFormat::Kbi:    return "Keyboard Inspector File";
        case ImportFormat::Tetrio: return "TETR.IO Replay File";
        default:                   return "";
    }
}

ImportUrlDialog::ImportUrlDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Import from URL");
    setModal(true);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto* row = new QHBoxLayout();
    auto* label = new QLabel("URL:");
    label->setStyleSheet(QString("color: ") + Theme::TEXT.name());
    _input = new QLineEdit(this);
    _input->setPlaceholderText("https://… or tetr.io replay link");
    row->addWidget(label);
    row->addWidget(_input, 1);
    layout->addLayout(row);

    _detected = new QLabel(this);
    _detected->setStyleSheet(QString("color: ") + Theme::TEXT_SECONDARY.name());
    layout->addWidget(_detected);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::StandardButton::Cancel |
        QDialogButtonBox::StandardButton::Ok
    );
    _import = buttons->button(QDialogButtonBox::StandardButton::Ok);
    _import->setText("Import");
    _import->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(_input, &QLineEdit::textChanged, this, &ImportUrlDialog::_check);
    connect(_input, &QLineEdit::returnPressed, this, [this]() {
        if (_format != ImportFormat::None) accept();
    });
}

void ImportUrlDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (_format != ImportFormat::None) return;
    if (!_clipboard_check()) {
        _input->setFocus();
        _input->setCursorPosition(_input->text().length());
    }
}

bool ImportUrlDialog::_clipboard_check() {
    QString url = QGuiApplication::clipboard()->text();
    if (url.isEmpty()) return false;
    QString download;
    if (classify_import_url(url, &download) == ImportFormat::None) return false;

    QSignalBlocker blocker(_input);
    _input->setText(url);
    _input->setFocus();
    _input->setCursorPosition(_input->text().length());
    _check(url);
    return true;
}

void ImportUrlDialog::_check(const QString& url) {
    _import->setEnabled(false);
    _detected->setText("");
    _format = ImportFormat::None;
    _download_url.clear();

    QString download;
    ImportFormat format = classify_import_url(url, &download);
    if (format == ImportFormat::None) return;

    _import->setEnabled(true);
    _detected->setText(import_format_name(format));
    _format = format;
    _download_url = download;
}

TetrioDisclaimerDialog::TetrioDisclaimerDialog(const std::string& text, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("TETR.IO replay disclaimer");
    setModal(true);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(10);

    auto* title = new QLabel("You are analyzing a TETR.IO replay file.");
    title->setStyleSheet(QString("color: ") + Theme::TEXT.name() + QString("; font-weight: bold;"));
    layout->addWidget(title);

    auto* body = new QLabel(QString::fromStdString(text));
    body->setWordWrap(true);
    body->setMinimumWidth(480);
    layout->addWidget(body);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::StandardButton::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

TetrioPickerDialog::TetrioPickerDialog(const TetrioMultiplayer& info, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Select a TETR.IO round");
    setModal(true);
    setMinimumSize(540, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(8);

    auto* matchup = new QLabel(QString::fromStdString(info.matchup));
    matchup->setStyleSheet(QString("color: ") + Theme::TEXT.name() + QString("; font-weight: bold;"));
    matchup->setWordWrap(true);
    layout->addWidget(matchup);

    auto* hint = new QLabel(
        "Each entry analyzes one player's inputs for that round. "
        "Both players are listed because the replay is stored per player."
    );
    hint->setWordWrap(true);
    hint->setStyleSheet(QString("color: ") + Theme::TEXT_SECONDARY.name());
    layout->addWidget(hint);

    auto* list = new QListWidget(this);
    for (const auto& entry : info.rounds) {
        auto* item = new QListWidgetItem(
            QString::fromStdString(entry.label) + "\n" + QString::fromStdString(entry.detail)
        );
        list->addItem(item);
    }
    layout->addWidget(list, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::StandardButton::Cancel |
        QDialogButtonBox::StandardButton::Ok
    );
    auto* analyze = buttons->button(QDialogButtonBox::StandardButton::Ok);
    analyze->setText("Analyze");
    analyze->setEnabled(list->count() > 0);
    if (list->count()) {
        list->setCurrentRow(0);
        _selection = &info.rounds[0];
    }
    connect(list, &QListWidget::currentRowChanged, this, [&info, this](int row) {
        if (row >= 0 && row < static_cast<int>(info.rounds.size())) {
            _selection = &info.rounds[row];
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, this, [this](){ accept(); });
    layout->addWidget(buttons);
}

} // namespace keyboard_inspector
