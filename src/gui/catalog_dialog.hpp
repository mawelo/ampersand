// catalog_dialog.hpp — Blockauswahl aus dem Geraetekatalog
//
// Wer:      Martin.Lonkwitz
// Was:      Dialog mit Kategorienliste, Suchfeld, Modell-Liste und Parameter-
//           Vorschau; liefert die gewaehlte Modell-ID. Versteckte/interne/
//           ersetzte Modelle sind per Haken zuschaltbar.
// Wann:     2026-09-04
// Warum:    Bloecke sollen ohne ID-Eingabe platziert werden koennen (todo).
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include "qc/catalog.hpp"

#include <QDialog>

#include <optional>

class QLineEdit;
class QListWidget;
class QCheckBox;
class QLabel;
class QListWidgetItem;

class CatalogDialog : public QDialog {
    Q_OBJECT
public:
    CatalogDialog(const qc::catalog::Catalog& catalog, int row, int column, QWidget* parent = nullptr);

    [[nodiscard]] std::optional<int> selectedModelId() const { return selected_; }

    /// Convenience: run modally, return the chosen model id.
    static std::optional<int> pick(const qc::catalog::Catalog& catalog, int row, int column, QWidget* parent);

private:
    void rebuildCategories();
    void rebuildModels();
    void showPreview(QListWidgetItem* item);

    const qc::catalog::Catalog& catalog_;
    QLineEdit* search_{nullptr};
    QListWidget* categories_{nullptr};
    QListWidget* models_{nullptr};
    QCheckBox* showAll_{nullptr};
    QLabel* preview_{nullptr};
    QLabel* count_{nullptr};
    std::optional<int> selected_;
};
