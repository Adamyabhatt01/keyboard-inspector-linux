#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QUrl>

#include <string>

#include "../core/model.h"
#include "../core/tetrio.h"

namespace keyboard_inspector {

class AboutDialog : public QDialog {
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

// Port of FileSystem.FileFormat URL filters (Format.cs DefaultURLFilter and
// the TETR.IO CustomURLFilter).
enum class ImportFormat {
    None,
    Kbi,      // direct http(s) link to a .kbi file
    Tetrio,   // tetr.io replay link, rewritten to the replay API URL
};

ImportFormat classify_import_url(const QString& url, QString* download_url = nullptr);
const char* import_format_name(ImportFormat format);

// Port of FileSystem.ImportFileDialog: URL input with live format detection
// and clipboard auto-fill.
class ImportUrlDialog : public QDialog {
public:
    explicit ImportUrlDialog(QWidget* parent = nullptr);

    QString url() const { return _input->text().trimmed(); }
    ImportFormat format() const { return _format; }
    QString download_url() const { return _download_url; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    void _check(const QString& url);
    bool _clipboard_check();

    QLineEdit* _input;
    QLabel* _detected;
    QPushButton* _import;
    ImportFormat _format = ImportFormat::None;
    QString _download_url;
};

class TetrioDisclaimerDialog : public QDialog {
public:
    explicit TetrioDisclaimerDialog(const std::string& text, QWidget* parent = nullptr);
};

class TetrioPickerDialog : public QDialog {
public:
    explicit TetrioPickerDialog(const TetrioMultiplayer& info, QWidget* parent = nullptr);
    const TetrioRoundEntry* selection() const { return _selection; }

private:
    const TetrioRoundEntry* _selection = nullptr;
};

} // namespace keyboard_inspector
