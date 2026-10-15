// io_meter_view.hpp — Pegel, die das Geraet selbst meldet (IOMeter)
//
// Wer:      Martin.Lonkwitz
// Was:      Balken fuer Eingaenge, Returns, die analogen Ausgaenge samt
//           Kopfhoerer und die vier USB-Paare je Richtung (Maximum aus L/R),
//           dazu eine Statuszeile, ob gerade Meldungen kommen.
// Wann:     2026-10-01
// Warum:    Ergaenzt MeterView (Audiostrom, nur die acht USB-Kanaele) um das,
//           was nur das Geraet kennt: Returns, XLR, Out 3/4, Sends, Kopfhoerer.
//           Das Geraet meldet nur, solange Signal anliegt (Beleg 182); Stille
//           ist deshalb kein Fehler, sondern Pegel 0, und wird so angezeigt.
// Projekt:  Ampersand (GUI)
// Abhaengig: qccore (qc::iometer), Qt Widgets
// Nutzung:  connect(controller, &Controller::ioLevelsChanged, view, &IoMeterView::setLevels);
// GRENZE    Die Skala ist nicht bestaetigt; die Balken lesen die Werte als
//           lineare Amplitude (siehe include/qc/io_meter.hpp) und sagen das im
//           Fuss. Die "limiter"-Felder stehen gemessen dauerhaft auf 1,0
//           (Beleg 183); ihre Bedeutung ist unbekannt, sie werden nicht gezeigt.
// Historie: git log
#pragma once

#include "qc/io_meter.hpp"

#include <QElapsedTimer>
#include <QWidget>

#include <array>

class QTimer;

class IoMeterView : public QWidget {
    Q_OBJECT
public:
    explicit IoMeterView(QWidget* parent = nullptr);
    static constexpr std::size_t kRowCount = 19;  ///< Zeilen, siehe kRows in der .cpp

public slots:
    void setLevels(const qc::iometer::Levels& levels);
    /// Anzeige leeren, etwa beim Trennen.
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    static constexpr double kFloorDb = -60.0;
    static constexpr double kHoldFallDb = 0.6;  ///< je Meldung, ~5 dB/s bei 8,6 Meldungen/s
    static constexpr int kSilentAfterMs = 1500;

    qc::iometer::Levels levels_{};
    std::array<double, kRowCount> hold_db_{};
    bool have_{false};
    QElapsedTimer last_;
    QTimer* tick_{nullptr};
};
