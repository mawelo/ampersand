// grid_view.hpp — das 4x8-Grid als QGraphicsView
//
// Wer:      Martin.Lonkwitz
// Was:      Zeichnet Eingangs-/Ausgangsknoten, Bloecke (Kategorie-Farbe, Bypass
//           gedimmt), leere Zellen, Verbindungslinien und Split/Merge-Pfade;
//           meldet Klicks auf Zellen und Knoten.
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include "qc/preset_view.hpp"

#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QGraphicsView>

#include <optional>

class GridView : public QGraphicsView {
    Q_OBJECT
public:
    explicit GridView(QWidget* parent = nullptr);

    void setPreset(const qc::view::PresetView& view);
    void setSelected(int row, int column);
    [[nodiscard]] std::optional<std::pair<int, int>> selected() const { return selected_; }
    void clear();

signals:
    void cellClicked(int row, int column, bool occupied);
    void cellDoubleClicked(int row, int column, bool occupied);
    void inputClicked(int row);
    void outputClicked(int row);
    /// A block was dragged from one cell and dropped on another.
    void blockMoveRequested(int fromRow, int fromColumn, int toRow, int toColumn);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    struct Hit {
        enum class Kind { None, Cell, Input, Output } kind{Kind::None};
        int row{-1};
        int column{-1};
        bool occupied{false};
    };
    [[nodiscard]] Hit hitTest(const QPointF& scenePos) const;
    void rebuild();
    void fit();

    void cancelDrag();

    QGraphicsScene scene_;
    std::optional<qc::view::PresetView> view_;
    std::optional<std::pair<int, int>> selected_;
    // drag & drop state
    std::optional<std::pair<int, int>> dragSource_;
    QPointF dragStart_;
    bool dragging_{false};
    QGraphicsPathItem* dragGhost_{nullptr};
    QGraphicsPathItem* dropTarget_{nullptr};
    std::optional<std::pair<int, int>> dropCell_;
};
