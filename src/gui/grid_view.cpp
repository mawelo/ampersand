// grid_view.cpp — Grid-Zeichnung im Stil des Quad-Cortex-Displays (siehe grid_view.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
//
// Vorbild: Foto des Geraetedisplays (docs/evidence/23-unit-display-2D.jpg):
//   * schwarzer Grund, duenne helle Signallinie je Zeile
//   * Ein-/Ausgangsknoten als graue abgerundete Kacheln ("In 1", "Prev. Row",
//     "Row 4", "USB 3/4"); ohne Port nur ein "+"
//   * Bloecke als abgerundete Quadrate mit farbigem Rand und weissem Glyph,
//     leere Zellen werden NICHT gezeichnet (nur klickbar)
//   * Abzweig: die Lane beginnt VOR der Spalte `split` und faellt in die Zeile
//     darunter; Zusammenfuehrung steigt NACH der Spalte `mix` wieder auf
//   * Zeilen, die von einer anderen Zeile gespeist werden ("Row 3"/"Row 4"),
//     tragen den Eingang "Prev. Row"
#include "grid_view.hpp"

#include "theme.hpp"

#include <QFont>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPen>

namespace {

constexpr qreal kLeft = 16;
constexpr qreal kTop = 26;
constexpr qreal kNodeW = 74;
constexpr qreal kNodeH = 56;
constexpr qreal kCell = 62;
constexpr qreal kPitch = 100;
constexpr qreal kRowPitch = 124;
constexpr qreal kSlotsX = kLeft + kNodeW + 30;
constexpr qreal kOutX = kSlotsX + 8 * kPitch + 6;
constexpr qreal kForkGap = 16;

const QColor kLine{0xd8, 0xd8, 0xdc};
const QColor kNodeFill{0x3a, 0x3d, 0x42};
const QColor kBlockFill{0x0e, 0x0e, 0x10};

qreal rowY(int row) { return kTop + row * kRowPitch; }
qreal midY(int row) { return rowY(row) + kCell / 2; }
QRectF cellRect(int row, int col) { return {kSlotsX + col * kPitch, rowY(row), kCell, kCell}; }
QRectF inRect(int row) { return {kLeft, midY(row) - kNodeH / 2, kNodeW, kNodeH}; }
QRectF outRect(int row) { return {kOutX, midY(row) - kNodeH / 2, kNodeW, kNodeH}; }

QString elide(const QString& s, int max) { return s.size() > max ? s.left(max - 1) + QStringLiteral("…") : s; }

/// Simple line-art glyphs per category, drawn inside `r` in `color`.
void add_glyph(QGraphicsScene& scene, const QRectF& r, const QString& category, const QColor& color, bool capture) {
    const QString c = category.toLower();
    const QPen pen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const qreal cx = r.center().x(), cy = r.center().y(), s = r.width() * 0.26;
    QPainterPath p;
    if (capture) {  // box with a dot (the unit's capture marker)
        p.addRoundedRect(cx - s, cy - s * 0.6, 2 * s, 1.2 * s, 3, 3);
        p.moveTo(cx - s * 0.6, cy);
        p.lineTo(cx + s * 0.6, cy);
        scene.addPath(p, pen)->setZValue(4);
        scene.addEllipse(r.left() + 6, r.bottom() - 14, 8, 8, QPen(Qt::NoPen), QBrush(color))->setZValue(4);
        return;
    }
    if (c.contains("gate") || c.contains("utility")) {  // three slanted bars like the unit's gate icon
        for (int i = -1; i <= 1; ++i) {
            p.moveTo(cx + i * s * 0.7 - s * 0.3, cy + s);
            p.lineTo(cx + i * s * 0.7 + s * 0.3, cy - s);
        }
    } else if (c.contains("pitch")) {  // response curve
        p.moveTo(cx - s, cy + s * 0.8);
        p.cubicTo(cx - s * 0.2, cy + s * 0.8, cx + s * 0.2, cy - s * 0.8, cx + s, cy - s * 0.8);
        p.moveTo(cx - s, cy);
        p.lineTo(cx + s, cy);
    } else if (c.contains("equalizer") || c == "eq") {  // three faders
        for (int i = -1; i <= 1; ++i) {
            const qreal x = cx + i * s * 0.8;
            p.moveTo(x, cy - s);
            p.lineTo(x, cy + s);
            const qreal k = cy + (i == 0 ? -s * 0.4 : s * 0.3 * i);
            p.addRect(x - 3, k - 2.5, 6, 5);
        }
    } else if (c.contains("cab") || c.contains("irloader")) {  // speaker
        p.addRoundedRect(cx - s, cy - s, 2 * s, 2 * s, 3, 3);
        p.addEllipse(QPointF(cx, cy), s * 0.55, s * 0.55);
        p.addEllipse(QPointF(cx, cy), s * 0.15, s * 0.15);
    } else if (c.contains("amp")) {  // amp head: box with a knob row
        p.addRoundedRect(cx - s, cy - s * 0.7, 2 * s, 1.4 * s, 3, 3);
        for (int i = -1; i <= 1; ++i) p.addEllipse(QPointF(cx + i * s * 0.6, cy - s * 0.25), 2.5, 2.5);
        p.moveTo(cx - s * 0.7, cy + s * 0.3);
        p.lineTo(cx + s * 0.7, cy + s * 0.3);
    } else if (c.contains("overdrive") || c.contains("dist") || c.contains("fuzz")) {  // clipped wave
        p.moveTo(cx - s, cy);
        p.lineTo(cx - s * 0.6, cy - s * 0.8);
        p.lineTo(cx - s * 0.1, cy - s * 0.8);
        p.lineTo(cx + s * 0.1, cy + s * 0.8);
        p.lineTo(cx + s * 0.6, cy + s * 0.8);
        p.lineTo(cx + s, cy);
    } else if (c.contains("delay")) {  // echo: decreasing bars
        for (int i = 0; i < 4; ++i) {
            const qreal h = s * (1.0 - i * 0.25);
            p.moveTo(cx - s + i * s * 0.66, cy + s);
            p.lineTo(cx - s + i * s * 0.66, cy + s - 2 * h);
        }
    } else if (c.contains("reverb")) {  // waves
        for (int i = 0; i < 3; ++i) {
            const qreal y = cy - s * 0.6 + i * s * 0.6;
            p.moveTo(cx - s, y);
            p.cubicTo(cx - s * 0.5, y - s * 0.4, cx, y + s * 0.4, cx + s, y);
        }
    } else if (c.contains("mod") || c.contains("chorus") || c.contains("phaser") || c.contains("flanger")) {  // sine
        p.moveTo(cx - s, cy);
        p.cubicTo(cx - s * 0.5, cy - s * 1.4, cx, cy + s * 1.4, cx + s, cy);
    } else if (c.contains("comp") || c.contains("dyn")) {  // arrows in
        p.moveTo(cx - s, cy - s);
        p.lineTo(cx - s * 0.2, cy);
        p.lineTo(cx - s, cy + s);
        p.moveTo(cx + s, cy - s);
        p.lineTo(cx + s * 0.2, cy);
        p.lineTo(cx + s, cy + s);
    } else if (c.contains("wah") || c.contains("filter")) {  // pedal wedge
        p.moveTo(cx - s, cy + s * 0.6);
        p.lineTo(cx + s, cy - s * 0.6);
        p.lineTo(cx + s, cy + s * 0.6);
        p.closeSubpath();
    } else if (c.contains("loop") || c.contains("fx")) {  // loop arrows
        p.addEllipse(QPointF(cx, cy), s, s * 0.7);
        p.moveTo(cx + s, cy - s * 0.5);
        p.lineTo(cx + s, cy);
        p.lineTo(cx + s * 0.5, cy);
    } else if (c.contains("synth")) {  // keys
        p.addRect(cx - s, cy - s * 0.6, 2 * s, 1.2 * s);
        for (int i = -1; i <= 1; ++i) {
            p.moveTo(cx + i * s * 0.5, cy - s * 0.6);
            p.lineTo(cx + i * s * 0.5, cy + s * 0.2);
        }
    } else {  // generic: dot
        p.addEllipse(QPointF(cx, cy), s * 0.5, s * 0.5);
    }
    scene.addPath(p, pen)->setZValue(4);
}

}  // namespace

GridView::GridView(QWidget* parent) : QGraphicsView(parent) {
    setScene(&scene_);
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::TextAntialiasing, true);
    setBackgroundBrush(QColor(0x08, 0x08, 0x09));
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    clear();
}

void GridView::clear() {
    view_.reset();
    selected_.reset();
    rebuild();
}

void GridView::setPreset(const qc::view::PresetView& view) {
    view_ = view;
    rebuild();
}

void GridView::setSelected(int row, int column) {
    selected_ = std::make_pair(row, column);
    rebuild();
}

void GridView::rebuild() {
    // scene_.clear() deletes every item, including a drag ghost in flight.
    dragGhost_ = nullptr;
    dropTarget_ = nullptr;
    dragging_ = false;
    dragSource_.reset();
    dropCell_.reset();
    scene_.clear();
    scene_.setSceneRect(0, 0, kOutX + kNodeW + 24, kTop + 4 * kRowPitch);

    QFont small = font();
    small.setPointSizeF(7.5);
    QFont nodeFont = font();
    nodeFont.setPointSizeF(8.5);
    nodeFont.setBold(true);
    QFont plusFont = font();
    plusFont.setPointSizeF(16);

    const QPen linePen(kLine, 2.2);
    const QPen dimLinePen(QColor(0x33, 0x33, 0x38), 2);

    auto addNode = [&](const QRectF& r, const QString& text) {
        QPainterPath path;
        path.addRoundedRect(r, 9, 9);
        scene_.addPath(path, QPen(Qt::NoPen), QBrush(kNodeFill))->setZValue(2);
        auto* t = scene_.addSimpleText(elide(text, 9), nodeFont);
        t->setBrush(QColor(0xf0, 0xf0, 0xf0));
        t->setPos(r.center().x() - t->boundingRect().width() / 2, r.center().y() - t->boundingRect().height() / 2);
        t->setZValue(3);
    };
    auto addPlus = [&](qreal x, qreal y) {
        auto* t = scene_.addSimpleText(QStringLiteral("+"), plusFont);
        t->setBrush(QColor(0xc0, 0xc0, 0xc8));
        t->setPos(x - t->boundingRect().width() / 2, y - t->boundingRect().height() / 2);
        t->setZValue(3);
    };

    for (int row = 0; row < qc::view::kRows; ++row) {
        const qc::view::RowView* rv = view_ ? &view_->rows[static_cast<std::size_t>(row)] : nullptr;
        const qreal y = midY(row);
        const bool hasOut = rv && rv->out_port > 0;
        const QString inLabel = rv ? QString::fromStdString(rv->input_label()) : QString();
        const qc::view::RowView* above = (rv && row % 2 == 1) ? &view_->rows[static_cast<std::size_t>(row - 1)] : nullptr;
        const bool forkedLane = above && above->split && above->split->split_column >= 0;
        const bool active = rv && (!inLabel.isEmpty() || forkedLane || !rv->blocks.empty());

        // input side
        qreal lineStart = inRect(row).right();
        if (!inLabel.isEmpty()) {
            addNode(inRect(row), inLabel);
        } else if (forkedLane) {
            const int sc = std::clamp(above->split->split_column, 0, 7);
            lineStart = cellRect(row, sc).left() - kForkGap - 20;
            addPlus(inRect(row).center().x(), y);
        } else {
            addPlus(inRect(row).center().x(), y);
        }
        // output side
        qreal lineEnd = outRect(row).left();
        const bool merges = rv && row % 2 == 1 && above && above->split && above->split->rejoins();
        if (hasOut) {
            addNode(outRect(row), QString::fromStdString(rv->out_name));
        } else if (merges) {
            const int mc = std::clamp(above->split->mix_column, 0, 7);
            lineEnd = cellRect(row, mc).right() + kForkGap + 20;
        } else {
            addPlus(outRect(row).center().x(), y);
        }
        scene_.addLine(lineStart, y, lineEnd, y, active ? linePen : dimLinePen)->setZValue(1);

        // row label (tiny, top-left of the row)
        auto* rl = scene_.addSimpleText(QStringLiteral("ROW %1").arg(row + 1), small);
        rl->setBrush(QColor(0x60, 0x60, 0x68));
        rl->setPos(kLeft, rowY(row) - 12);

        // blocks
        for (int col = 0; col < qc::view::kColumns; ++col) {
            const QRectF r = cellRect(row, col);
            const qc::view::BlockView* b = rv ? rv->block_at(col) : nullptr;
            const bool sel = selected_ && selected_->first == row && selected_->second == col;
            if (!b) {
                if (sel) {
                    QPainterPath path;
                    path.addRoundedRect(r, 12, 12);
                    scene_.addPath(path, QPen(theme::kSelected, 1.5, Qt::DashLine), QBrush(Qt::NoBrush))->setZValue(2);
                }
                continue;
            }
            const QColor color = theme::category_color(QString::fromStdString(b->category));
            QPainterPath path;
            path.addRoundedRect(r, 12, 12);
            auto* cell = scene_.addPath(path, QPen(color, 2.6), QBrush(kBlockFill));
            cell->setZValue(2);
            if (sel) {
                QPainterPath halo;
                halo.addRoundedRect(r.adjusted(-4, -4, 4, 4), 15, 15);
                scene_.addPath(halo, QPen(theme::kSelected, 1.5), QBrush(Qt::NoBrush))->setZValue(2);
            }
            add_glyph(scene_, r, QString::fromStdString(b->category), QColor(0xf4, 0xf4, 0xf6), b->is_capture);
            if (b->bypassed) {
                cell->setOpacity(0.35);
                for (auto* it : scene_.items(r)) {
                    if (it != cell && it->zValue() == 4) it->setOpacity(0.35);
                }
            }
            auto* name = scene_.addSimpleText(elide(QString::fromStdString(b->name), 13), small);
            name->setBrush(b->bypassed ? QColor(0x70, 0x70, 0x78) : QColor(0xb8, 0xb8, 0xc0));
            const QRectF nb = name->boundingRect();
            name->setPos(r.center().x() - nb.width() / 2, r.bottom() + 4);
            name->setZValue(4);
        }

        // fork / merge for even rows with a branch
        if (rv && rv->split && row % 2 == 0) {
            const qreal yLane = midY(row + 1);
            if (rv->split->split_column >= 0) {
                const int sc = std::clamp(rv->split->split_column, 0, 7);
                const qreal x0 = cellRect(row, sc).left() - kForkGap - 20;
                QPainterPath p;
                p.moveTo(x0, y);
                p.lineTo(x0 + 8, y);
                p.quadTo(x0 + 20, y, x0 + 20, y + 14);
                p.lineTo(x0 + 20, yLane - 14);
                p.quadTo(x0 + 20, yLane, x0 + 32, yLane);
                scene_.addPath(p, linePen)->setZValue(1);
            }
            if (rv->split->rejoins()) {
                const int mc = std::clamp(rv->split->mix_column, 0, 7);
                const qreal x1 = cellRect(row, mc).right() + kForkGap + 20;
                QPainterPath q;
                q.moveTo(x1 - 32, yLane);
                q.quadTo(x1 - 20, yLane, x1 - 20, yLane - 14);
                q.lineTo(x1 - 20, y + 14);
                q.quadTo(x1 - 20, y, x1 - 8, y);
                q.lineTo(x1, y);
                scene_.addPath(q, linePen)->setZValue(1);
            }
        }
    }

    if (!view_) {
        auto* t = scene_.addSimpleText(QStringLiteral("Not connected — File ▸ Connect (Ctrl+K)"), nodeFont);
        t->setBrush(theme::kTextDim);
        t->setPos(kSlotsX, kTop + 4 * kRowPitch - 20);
    }
    fit();
}

void GridView::fit() {
    const QRectF sr = scene_.sceneRect();
    const qreal sx = viewport()->width() / sr.width();
    const qreal sy = viewport()->height() / sr.height();
    const qreal s = std::min({sx, sy, 1.35});
    resetTransform();
    scale(s, s);
}

void GridView::resizeEvent(QResizeEvent* e) {
    QGraphicsView::resizeEvent(e);
    fit();
}

GridView::Hit GridView::hitTest(const QPointF& p) const {
    Hit h;
    for (int row = 0; row < qc::view::kRows; ++row) {
        if (inRect(row).contains(p)) return {Hit::Kind::Input, row, -1, false};
        if (outRect(row).contains(p)) return {Hit::Kind::Output, row, -1, false};
        for (int col = 0; col < qc::view::kColumns; ++col) {
            QRectF r = cellRect(row, col);
            r.adjust(-6, -6, 6, 20);
            if (r.contains(p)) {
                const bool occ = view_ && view_->rows[static_cast<std::size_t>(row)].block_at(col) != nullptr;
                return {Hit::Kind::Cell, row, col, occ};
            }
        }
    }
    return h;
}

void GridView::cancelDrag() {
    if (dragGhost_) {
        scene_.removeItem(dragGhost_);
        delete dragGhost_;
        dragGhost_ = nullptr;
    }
    if (dropTarget_) {
        scene_.removeItem(dropTarget_);
        delete dropTarget_;
        dropTarget_ = nullptr;
    }
    dragging_ = false;
    dragSource_.reset();
    dropCell_.reset();
    setCursor(Qt::ArrowCursor);
}

void GridView::mouseMoveEvent(QMouseEvent* e) {
    if (dragSource_ && (e->buttons() & Qt::LeftButton)) {
        const QPointF pos = mapToScene(e->pos());
        if (!dragging_ && (pos - dragStart_).manhattanLength() > QApplication::startDragDistance()) {
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
            const auto* b = view_ ? view_->rows[static_cast<std::size_t>(dragSource_->first)].block_at(dragSource_->second) : nullptr;
            const QColor color = b ? theme::category_color(QString::fromStdString(b->category)) : theme::kSelected;
            QPainterPath path;
            path.addRoundedRect(QRectF(-kCell / 2, -kCell / 2, kCell, kCell), 12, 12);
            dragGhost_ = scene_.addPath(path, QPen(color, 2.6), QBrush(QColor(color.red(), color.green(), color.blue(), 60)));
            dragGhost_->setZValue(10);
            dragGhost_->setOpacity(0.8);
        }
        if (dragging_) {
            dragGhost_->setPos(pos);
            const Hit h = hitTest(pos);
            std::optional<std::pair<int, int>> target;
            if (h.kind == Hit::Kind::Cell && !(h.row == dragSource_->first && h.column == dragSource_->second)) {
                target = std::make_pair(h.row, h.column);
            }
            if (target != dropCell_) {
                dropCell_ = target;
                if (dropTarget_) {
                    scene_.removeItem(dropTarget_);
                    delete dropTarget_;
                    dropTarget_ = nullptr;
                }
                if (dropCell_) {
                    QPainterPath path;
                    path.addRoundedRect(cellRect(dropCell_->first, dropCell_->second).adjusted(-4, -4, 4, 4), 15, 15);
                    dropTarget_ = scene_.addPath(path, QPen(theme::kSelected, 2, Qt::DashLine), QBrush(Qt::NoBrush));
                    dropTarget_->setZValue(9);
                }
            }
        }
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
}

void GridView::mouseReleaseEvent(QMouseEvent* e) {
    if (dragging_ && dragSource_) {
        const auto from = *dragSource_;
        const auto to = dropCell_;
        cancelDrag();
        if (to) emit blockMoveRequested(from.first, from.second, to->first, to->second);
        return;
    }
    dragSource_.reset();
    QGraphicsView::mouseReleaseEvent(e);
}

void GridView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && dragging_) {
        cancelDrag();
        return;
    }
    QGraphicsView::keyPressEvent(e);
}

void GridView::mousePressEvent(QMouseEvent* e) {
    const Hit h = hitTest(mapToScene(e->pos()));
    switch (h.kind) {
        case Hit::Kind::Cell:
            setSelected(h.row, h.column);
            emit cellClicked(h.row, h.column, h.occupied);
            if (h.occupied && e->button() == Qt::LeftButton) {
                dragSource_ = std::make_pair(h.row, h.column);
                dragStart_ = mapToScene(e->pos());
            }
            break;
        case Hit::Kind::Input: emit inputClicked(h.row); break;
        case Hit::Kind::Output: emit outputClicked(h.row); break;
        case Hit::Kind::None: break;
    }
    QGraphicsView::mousePressEvent(e);
}

void GridView::mouseDoubleClickEvent(QMouseEvent* e) {
    const Hit h = hitTest(mapToScene(e->pos()));
    if (h.kind == Hit::Kind::Cell) emit cellDoubleClicked(h.row, h.column, h.occupied);
    QGraphicsView::mouseDoubleClickEvent(e);
}
