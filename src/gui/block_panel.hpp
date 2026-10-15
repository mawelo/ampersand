// block_panel.hpp — Parameter-Editor fuer den ausgewaehlten Block
//
// Wer:      Martin.Lonkwitz
// Was:      Zeigt Name/Kategorie/Modell-ID, Bypass-Schalter, Entfernen-Knopf
//           und je Parameter einen Slider (bzw. Auswahlliste fuer Listen-
//           Parameter, Text fuer Strings). Aenderungen werden gedrosselt
//           (30 ms) als Signale gemeldet.
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include "qc/preset_view.hpp"

#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QVBoxLayout;
class QCheckBox;
class QPushButton;
class QSlider;
class QComboBox;
class QTimer;

class BlockPanel : public QWidget {
    Q_OBJECT
public:
    explicit BlockPanel(QWidget* parent = nullptr);

    void showBlock(const qc::view::BlockView& block);
    void showEmpty(int row, int column);
    void showNothing();
    /// Refresh values from a new view without rebuilding widgets (keeps sliders being dragged).
    void update(const qc::view::PresetView& view);

signals:
    void paramChanged(int row, int column, int index, float value);
    void paramTextChanged(int row, int column, int index, const QString& text);
    void bypassToggled(int row, int column, bool bypassed);
    void removeRequested(int row, int column);
    void addRequested(int row, int column);

private:
    struct ParamWidgets {
        int index{0};
        QSlider* slider{nullptr};
        QComboBox* combo{nullptr};
        QLabel* value{nullptr};
        qc::view::ParamView spec;
    };
    void clearParams();
    void emitParam(const ParamWidgets& w);
    QString valueText(const qc::view::ParamView& p, float normalized) const;

    QLabel* title_{nullptr};
    QLabel* subtitle_{nullptr};
    QCheckBox* bypass_{nullptr};
    QPushButton* remove_{nullptr};
    QPushButton* add_{nullptr};
    QWidget* paramsHost_{nullptr};
    QVBoxLayout* paramsLayout_{nullptr};
    std::vector<ParamWidgets> params_;
    std::optional<qc::view::BlockView> block_;
    int row_{-1};
    int column_{-1};
    bool suppress_{false};
    QTimer* throttle_{nullptr};
    std::optional<std::pair<int, float>> pending_;
};
