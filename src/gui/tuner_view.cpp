// tuner_view.cpp — Stimmanzeige (siehe tuner_view.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      Messfaden ueber qc::audio und qc::pitch, dazu das Zeichnen der
//           Skala, des Zeigers und der Note.
// Wann:     2026-09-11
// Warum:    siehe Kopf von tuner_view.hpp
// Projekt:  Ampersand
// Abhaengig: qccore, Qt Widgets
// Nutzung:  ueber TunerView
// Historie: git log
#include "tuner_view.hpp"

#include "theme.hpp"

#include "qc/audio.hpp"
#include "qc/pitch.hpp"

#include <QFontMetrics>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

/// Halbe Sekunde. Kuerzer ging schief: Bei 55 Hz stecken in einer
/// Viertelsekunde nur rund 14 Perioden, und die Guete reicht dann meist nicht
/// ueber die Schwelle - die Anzeige sagte staendig "kein Ton", obwohl ein
/// sauberer anlag.
constexpr double kWindowSeconds = 0.5;

/// Ab hier ist eine Messung nicht mehr zu gebrauchen. Am Geraet lagen brauchbare
/// Fenster bei 0,001 bis 0,016, unbrauchbare bei 0,1 bis 0,5.
constexpr double kQualityLimit = 0.05;

constexpr double kCentsRange = 50.0;

}  // namespace

TunerView::TunerView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(120);
    setAutoFillBackground(false);
}

TunerView::~TunerView() { stop(); }

void TunerView::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](const std::stop_token& stop) {
        auto in = qc::audio::Capture::open_quad_cortex();
        if (!in) {
            const QString reason = QString::fromStdString(in.error());
            QMetaObject::invokeMethod(this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
            return;
        }
        std::vector<float> block(static_cast<std::size_t>(qc::audio::kSampleRate * kWindowSeconds));
        while (!stop.stop_requested()) {
            auto got = in->read_channel(qc::audio::kDryLeft, block);
            if (!got) {
                const QString reason = QString::fromStdString(got.error());
                QMetaObject::invokeMethod(this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
                return;
            }
            const auto reading = qc::pitch::detect(block, qc::audio::kSampleRate);
            const double rms = in->last_rms(qc::audio::kDryLeft);
            const double level = rms > 0.0 ? 20.0 * std::log10(rms) : -120.0;
            const auto note = reading.frequency_hz > 0.0 ? qc::pitch::nearest_note(reading.frequency_hz)
                                                         : qc::pitch::Note{};
            const QString name = QString::fromStdString(note.name);
            const double cents = note.cents;
            const double hz = reading.frequency_hz;
            const double quality = reading.aperiodicity;
            QMetaObject::invokeMethod(
                this, [this, name, cents, hz, quality, level] { apply(name, cents, hz, quality, level); },
                Qt::QueuedConnection);
        }
    });
}

void TunerView::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
    have_ = false;
    update();
}

void TunerView::apply(const QString& note, double cents, double hz, double quality, double levelDb) {
    note_ = note;
    cents_ = cents;
    hz_ = hz;
    quality_ = quality;
    level_db_ = levelDb;
    have_ = quality <= kQualityLimit && hz > 0.0;
    update();
}

QSize TunerView::sizeHint() const { return {520, 130}; }

void TunerView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect().adjusted(1, 1, -1, -1);
    p.fillRect(r, theme::kEmptyCell);

    const double mid = r.center().x();
    const double line_y = r.top() + r.height() * 0.62;
    const double half = r.width() * 0.42;

    // Skala: Striche alle 10 Cent, die Mitte betont.
    p.setPen(QPen(theme::kGridLine, 1.0));
    for (int c = -50; c <= 50; c += 10) {
        const double x = mid + half * (c / kCentsRange);
        const double h = (c == 0) ? 16.0 : (c % 50 == 0 ? 12.0 : 7.0);
        p.drawLine(QPointF(x, line_y - h), QPointF(x, line_y + h));
    }
    p.setPen(QPen(theme::kGridLine, 2.0));
    p.drawLine(QPointF(mid - half, line_y), QPointF(mid + half, line_y));

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    p.setPen(theme::kTextDim);
    p.drawText(QRectF(mid - half - 30, line_y + 14, 60, 20), Qt::AlignCenter, QStringLiteral("-50"));
    p.drawText(QRectF(mid + half - 30, line_y + 14, 60, 20), Qt::AlignCenter, QStringLiteral("+50"));

    if (!have_) {
        // Pegel und Guete auch im Wartezustand zeigen: Sonst sieht man nicht, ob
        // ueberhaupt Signal ankommt oder ob es nur nicht auswertbar ist - genau
        // diese Unterscheidung hat beim Einrichten am meisten Zeit gekostet.
        p.setPen(theme::kTextDim);
        const QString hint = level_db_ > -70.0 ? QStringLiteral("listening — no clear pitch")
                                               : QStringLiteral("listening — play a note");
        p.drawText(QRectF(r.left(), r.top() + 8, r.width(), 28), Qt::AlignCenter, hint);
        p.setFont(small);
        p.drawText(QRectF(r.left(), line_y + 30, r.width(), 20), Qt::AlignCenter,
                   quality_ < 1.0 ? QStringLiteral("%1 dBFS   quality %2")
                                        .arg(level_db_, 0, 'f', 1)
                                        .arg(quality_, 0, 'f', 3)
                                  : QStringLiteral("%1 dBFS").arg(level_db_, 0, 'f', 1));
        return;
    }

    // Innerhalb von +/-5 Cent gilt der Ton als sauber - das ist die Genauigkeit,
    // die auch das Geraet anzeigt.
    const bool in_tune = std::abs(cents_) <= 5.0;
    const QColor mark = in_tune ? theme::kOk : theme::kAccent;

    const double clamped = std::clamp(cents_, -kCentsRange, kCentsRange);
    const double x = mid + half * (clamped / kCentsRange);
    p.setBrush(mark);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(x, line_y), 9.0, 9.0);

    QFont big = font();
    big.setPointSizeF(big.pointSizeF() * 2.4);
    big.setBold(true);
    p.setFont(big);
    p.setPen(mark);
    p.drawText(QRectF(r.left(), r.top() + 4, r.width(), 44), Qt::AlignCenter, note_);

    p.setFont(small);
    p.setPen(theme::kTextDim);
    p.drawText(QRectF(r.left(), line_y + 30, r.width(), 20), Qt::AlignCenter,
               QStringLiteral("%1%2 cents   %3 Hz   %4 dBFS")
                   .arg(cents_ >= 0 ? QStringLiteral("+") : QString())
                   .arg(cents_, 0, 'f', 1)
                   .arg(hz_, 0, 'f', 2)
                   .arg(level_db_, 0, 'f', 1));
}
