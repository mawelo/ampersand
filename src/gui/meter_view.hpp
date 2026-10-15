// meter_view.hpp — Pegelanzeige aller acht USB-Kanaele
//
// Wer:      Martin.Lonkwitz
// Was:      Acht waagerechte Balken mit Effektivwert und mitlaufender Spitze,
//           dazu ein Faden, der im Hintergrund misst.
// Wann:     2026-09-21
// Warum:    Das Geraet meldet Pegel ueber das Protokoll nicht verlaesslich:
//           `IOMeter` fliesst nur kurz nach dem Start und danach nicht mehr
//           (Beleg 145). Im Audiostrom stehen sie dagegen immer (Beleg 83,
//           147). Der Weg ueber den Audiostrom ist ausserdem der allgemeinere -
//           er misst, was ueber die Schnittstelle kommt, gleich was das Geraet
//           intern routet.
// Projekt:  Ampersand (GUI)
// Abhaengig: qccore (qc::audio), Qt Widgets
// Nutzung:  view->start();  ... view->stop();
//
// ENTWURF   Derselbe Bau wie `TunerView`: ein `std::jthread` liest blockierend,
//           die Werte gehen per `QMetaObject::invokeMethod` in den GUI-Faden.
//           Die Karte laesst sich nur EINMAL oeffnen - laeuft `qcctl tune` oder
//           `qcctl meter` daneben, schlaegt `start()` fehl und meldet das ueber
//           `failed`, statt still nichts anzuzeigen.
// Historie: git log
#pragma once

#include "qc/audio.hpp"

#include <QWidget>

#include <array>
#include <thread>

class MeterView : public QWidget {
    Q_OBJECT
public:
    explicit MeterView(QWidget* parent = nullptr);
    ~MeterView() override;

    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return worker_.joinable(); }

signals:
    void failed(const QString& reason);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    Q_INVOKABLE void apply(const QList<double>& rms_db, double peak_db, quint64 dropouts);

    static constexpr double kFloorDb = -90.0;   ///< unteres Ende der Skala
    static constexpr double kPeakFallDbPerBlock = 1.5;  ///< Spitze sinkt sichtbar, statt zu kleben

    std::array<double, qc::audio::kChannels> rms_db_{};
    std::array<double, qc::audio::kChannels> hold_db_{};
    double peak_db_{kFloorDb};
    quint64 dropouts_{0};
    bool have_{false};

    std::jthread worker_;
};
