// io_meter_view.cpp — siehe io_meter_view.hpp
#include "io_meter_view.hpp"

#include "theme.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>

#include <algorithm>

namespace {
struct Row {
    const char* label;
    std::size_t left;   ///< Kanal in qc::iometer::Levels
    int right;          ///< zweiter Kanal eines Paars (angezeigt wird das Maximum), -1 = keiner
};
// Die *_limiter-Felder stehen gemessen dauerhaft auf 1,0 und hp_limiter_active
// auf wahr, auch ohne jede Uebersteuerung (Beleg 183). Ihre Bedeutung ist
// unbekannt; sie werden deshalb NICHT als Warnung angezeigt.
constexpr std::array<Row, IoMeterView::kRowCount> kRows{{
    {"Input 1", 0, -1}, {"Input 2", 1, -1}, {"Return 1", 2, -1}, {"Return 2", 3, -1},
    {"XLR 1", 4, -1}, {"XLR 2", 6, -1}, {"Out 3", 8, -1}, {"Out 4", 10, -1},
    {"Send 1", 12, -1}, {"Send 2", 13, -1}, {"Headphones", 14, 15},
    {"USB in 1", 24, 25}, {"USB in 2", 26, 27}, {"USB in 3", 28, 29}, {"USB in 4", 30, 31},
    {"USB out 1", 16, 17}, {"USB out 2", 18, 19}, {"USB out 3", 20, 21}, {"USB out 4", 22, 23},
}};
float pegel(const qc::iometer::Levels& l, const Row& r) {
    const float a = l.value(r.left);
    return r.right >= 0 ? std::max(a, l.value(static_cast<std::size_t>(r.right))) : a;
}
constexpr int kRowHeight = 14;
constexpr int kRowStep = 18;
constexpr int kGroupGap = 8;
}  // namespace

IoMeterView::IoMeterView(QWidget* parent) : QWidget(parent) {
    hold_db_.fill(kFloorDb);
    setMinimumHeight(static_cast<int>(kRows.size()) * kRowStep + 4 * kGroupGap + 24);
    setToolTip(QStringLiteral("Levels the unit reports itself (IOMeter). It sends them only while a signal "
                              "is present; silence shows as no level, not as an error."));
    // Nur fuer die Statuszeile: kommt nichts mehr, soll "silent" erscheinen,
    // auch wenn keine Meldung den Neuanstrich ausloest.
    tick_ = new QTimer(this);
    tick_->setInterval(500);
    connect(tick_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    tick_->start();
}

void IoMeterView::setLevels(const qc::iometer::Levels& levels) {
    levels_ = levels;
    for (std::size_t r = 0; r < kRows.size(); ++r) {
        const double db = std::max(qc::iometer::to_db(pegel(levels_, kRows[r])), kFloorDb);
        hold_db_[r] = std::max(db, hold_db_[r] - kHoldFallDb);
    }
    have_ = true;
    last_.restart();
    update();
}

void IoMeterView::clear() {
    levels_ = {};
    hold_db_.fill(kFloorDb);
    have_ = false;
    update();
}

QSize IoMeterView::sizeHint() const {
    return {320, static_cast<int>(kRows.size()) * kRowStep + 4 * kGroupGap + 24};
}

void IoMeterView::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.fillRect(event->rect(), theme::kBackground);
    const QFontMetrics fm(p.font());
    int labelBreite = 0;
    for (const auto& row : kRows) labelBreite = std::max(labelBreite, fm.horizontalAdvance(QString::fromLatin1(row.label)));
    labelBreite += 8;
    const int wertBreite = fm.horizontalAdvance(QStringLiteral("-00.0")) + 8;
    int y = 4;

    for (std::size_t r = 0; r < kRows.size(); ++r) {
        if (r == 4 || r == 8 || r == 11 || r == 15) y += kGroupGap;  // Eingaenge | Ausgaenge | Sends+KH | USB in | USB out
        const auto& row = kRows[r];
        p.setPen(theme::kTextDim);
        p.drawText(QRect(2, y, labelBreite, kRowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromLatin1(row.label));

        const int x0 = labelBreite + 4;
        const int breite = width() - x0 - wertBreite - 6;
        if (breite > 0) {
            p.fillRect(QRect(x0, y, breite, kRowHeight), theme::kEmptyCell);
            const float v = pegel(levels_, row);
            const double db = qc::iometer::to_db(v);
            const double anteil = std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0);
            const int voll = static_cast<int>(anteil * breite);
            const QColor farbe = db > -1.0 ? QColor(0xff, 0x5a, 0x3c) : db > -6.0 ? theme::kAccent : theme::kOk;
            if (voll > 0) p.fillRect(QRect(x0, y, voll, kRowHeight), farbe);
            const double halten = std::clamp((hold_db_[r] - kFloorDb) / -kFloorDb, 0.0, 1.0);
            if (halten > 0.0) {
                const int hx = x0 + static_cast<int>(halten * breite);
                p.setPen(theme::kSelected);
                p.drawLine(hx, y, hx, y + kRowHeight - 1);
            }
            p.setPen(theme::kText);
            p.drawText(QRect(x0 + breite + 4, y, wertBreite, kRowHeight), Qt::AlignVCenter | Qt::AlignRight,
                       v > 0.0f ? QString::number(db, 'f', 1) : QStringLiteral("-"));
        }
        y += kRowStep;
    }

    QString fuss;
    if (!have_) {
        fuss = QStringLiteral("waiting for signal");
    } else if (last_.elapsed() > kSilentAfterMs) {
        fuss = QStringLiteral("silent (no signal)");
    } else {
        fuss = QStringLiteral("live");
    }
    fuss += QStringLiteral("   (dB, scale presumed linear)");
    p.setPen(theme::kTextDim);
    p.drawText(QRect(2, y, width() - 4, 16), Qt::AlignVCenter | Qt::AlignLeft, fuss);
}
