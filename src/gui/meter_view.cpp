// meter_view.cpp — siehe meter_view.hpp
#include "meter_view.hpp"

#include "theme.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

/// 100 ms je Block - derselbe Takt, in dem auch das Geraet seine Pegel
/// schickt, wenn es sie schickt (Beleg 92: alle 100 ms).
constexpr std::size_t kBlockFrames = static_cast<std::size_t>(qc::audio::kSampleRate / 10);

double to_db(double linear) {
    return linear > 0.0 ? 20.0 * std::log10(linear) : -144.0;
}

}  // namespace

MeterView::MeterView(QWidget* parent) : QWidget(parent) {
    rms_db_.fill(kFloorDb);
    hold_db_.fill(kFloorDb);
    setMinimumHeight(8 * 18 + 24);
}

MeterView::~MeterView() { stop(); }

void MeterView::start() {
    if (worker_.joinable()) return;
    worker_ = std::jthread([this](const std::stop_token& stop) {
        auto in = qc::audio::Capture::open_quad_cortex();
        if (!in) {
            const QString reason = QString::fromStdString(in.error());
            QMetaObject::invokeMethod(this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
            return;
        }
        std::vector<float> block(kBlockFrames);
        while (!stop.stop_requested()) {
            // Einmal lesen genuegt: `read_channel` rechnet den Effektivwert
            // fuer ALLE Kanaele des Blocks, nicht nur fuer den geholten.
            auto got = in->read_channel(qc::audio::kDryLeft, block);
            if (!got) {
                const QString reason = QString::fromStdString(got.error());
                QMetaObject::invokeMethod(this, [this, reason] { emit failed(reason); }, Qt::QueuedConnection);
                return;
            }
            QList<double> werte;
            werte.reserve(qc::audio::kChannels);
            for (int c = 0; c < qc::audio::kChannels; ++c) werte.push_back(to_db(in->last_rms(c)));
            double spitze = 0.0;
            for (float v : block) spitze = std::max(spitze, static_cast<double>(std::abs(v)));
            const double peak_db = to_db(spitze);
            const auto aussetzer = static_cast<quint64>(in->dropouts());
            QMetaObject::invokeMethod(
                this, [this, werte, peak_db, aussetzer] { apply(werte, peak_db, aussetzer); },
                Qt::QueuedConnection);
        }
    });
}

void MeterView::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

void MeterView::apply(const QList<double>& rms_db, double peak_db, quint64 dropouts) {
    for (int c = 0; c < qc::audio::kChannels && c < rms_db.size(); ++c) {
        const auto i = static_cast<std::size_t>(c);
        rms_db_[i] = rms_db[c];
        // Die Spitze faellt langsam nach, sonst steht sie nach einem einzigen
        // lauten Block fuer immer am Anschlag und sagt nichts mehr.
        hold_db_[i] = std::max(rms_db_[i], hold_db_[i] - kPeakFallDbPerBlock);
    }
    peak_db_ = peak_db;
    dropouts_ = dropouts;
    have_ = true;
    update();
}

QSize MeterView::sizeHint() const { return {420, 8 * 18 + 24}; }

void MeterView::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.fillRect(event->rect(), theme::kBackground);
    p.setPen(theme::kTextDim);

    const QFontMetrics fm(p.font());
    const int labelBreite = fm.horizontalAdvance(QStringLiteral("usb out 4 (preset)")) + 8;
    const int wertBreite = fm.horizontalAdvance(QStringLiteral("-000.0")) + 8;
    const int hoehe = 14;
    const int abstand = 18;
    int y = 4;

    for (int c = 0; c < qc::audio::kChannels; ++c) {
        const auto i = static_cast<std::size_t>(c);
        const QString name = QString::fromUtf8(qc::audio::channel_name(c).data(),
                                               static_cast<qsizetype>(qc::audio::channel_name(c).size()));
        p.setPen(theme::kTextDim);
        p.drawText(QRect(2, y, labelBreite, hoehe), Qt::AlignVCenter | Qt::AlignLeft, name);

        const int x0 = labelBreite + 4;
        const int breite = width() - x0 - wertBreite - 6;
        if (breite <= 0) { y += abstand; continue; }
        p.fillRect(QRect(x0, y, breite, hoehe), theme::kEmptyCell);

        if (have_) {
            const double anteil = std::clamp((rms_db_[i] - kFloorDb) / -kFloorDb, 0.0, 1.0);
            const int voll = static_cast<int>(anteil * breite);
            // Farbe nach Aussteuerung: ueber -6 dBFS wird es eng, ueber -1 dB
            // ist es zu laut. Dieselbe Staffelung wie am Geraet.
            const QColor farbe = rms_db_[i] > -1.0   ? QColor(0xff, 0x5a, 0x3c)
                                 : rms_db_[i] > -6.0 ? theme::kAccent
                                                     : theme::kOk;
            if (voll > 0) p.fillRect(QRect(x0, y, voll, hoehe), farbe);

            const double halten = std::clamp((hold_db_[i] - kFloorDb) / -kFloorDb, 0.0, 1.0);
            const int hx = x0 + static_cast<int>(halten * breite);
            if (halten > 0.0) {
                p.setPen(theme::kSelected);
                p.drawLine(hx, y, hx, y + hoehe - 1);
            }
            p.setPen(theme::kText);
            p.drawText(QRect(x0 + breite + 4, y, wertBreite, hoehe), Qt::AlignVCenter | Qt::AlignRight,
                       QStringLiteral("%1").arg(rms_db_[i], 0, 'f', 1));
        }
        y += abstand;
    }

    p.setPen(theme::kTextDim);
    const QString fuss = have_ ? QStringLiteral("peak (dry) %1 dBFS   dropouts %2")
                                     .arg(peak_db_, 0, 'f', 1)
                                     .arg(dropouts_)
                               : QStringLiteral("not running");
    p.drawText(QRect(2, y, width() - 4, 16), Qt::AlignVCenter | Qt::AlignLeft, fuss);
}
