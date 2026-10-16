// library_panel.hpp — Ordner/Preset-Browser und "Save As"
//
// Wer:      Martin.Lonkwitz
// Was:      Ordnerauswahl (User/Factory + alle vom Geraet gemeldeten Ordner),
//           Preset-Liste (Doppelklick = Recall), Speichern-Formular.
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include "qc/device.hpp"

#include <QSet>
#include <QVector>
#include <QWidget>

class QComboBox;
class QListWidget;
class QLineEdit;
class QPushButton;
class QLabel;

class LibraryPanel : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPanel(QWidget* parent = nullptr);

    void setFolders(const QVector<qc::Folder>& folders);
    void setPresets(const QString& folder, const QVector<qc::PresetEntry>& presets);
    void setCurrent(const QString& folder, int position);
    void setEnabledAll(bool on);
    [[nodiscard]] QString currentFolder() const;

signals:
    void refreshRequested(const QString& folder);
    void foldersRequested();
    void recallRequested(const QString& folder, int position);
    void saveRequested(const QString& folder, int position, const QString& name, int instrument);
    void deletePresetRequested(const QString& folder, const QString& name);
    void movePresetRequested(const QString& folder, const QString& name, int toPosition);

private:
    /// true, wenn der gewaehlte Ordner werksseitig (schreibgeschuetzt) ist -
    /// dann kein Loeschen/Verschieben anbieten.
    [[nodiscard]] bool currentIsFactory() const;
    void showContextMenu(const QPoint& pos);

    QComboBox* folders_{nullptr};
    QSet<QString> factoryKeys_;
    QListWidget* list_{nullptr};
    QPushButton* refresh_{nullptr};
    QPushButton* scan_{nullptr};
    QLineEdit* saveSlot_{nullptr};
    QLineEdit* saveName_{nullptr};
    QComboBox* instrument_{nullptr};
    QPushButton* save_{nullptr};
    QLabel* status_{nullptr};
};
