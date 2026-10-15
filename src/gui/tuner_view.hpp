// tuner_view.hpp — Stimmanzeige aus dem USB-Audiostrom
//
// Wer:      Martin.Lonkwitz
// Was:      Widget mit Skala von -50 bis +50 Cent, Notenname und Frequenz, dazu
//           ein Faden, der im Hintergrund misst.
// Wann:     2026-09-11
// Warum:    Das Geraet meldet dem Host weder Tonhoehe noch Pegel
//           (docs/evidence/83, 86). Beides steht im Audiostrom, und die daraus
//           gerechnete Stimmung deckt sich mit der Anzeige des Geraets
//           (+3,2 Cent gemessen bei +3.2 angezeigt, docs/evidence/85).
// Projekt:  Ampersand
// Abhaengig: qccore (qc::audio, qc::pitch), Qt Widgets
// Nutzung:  view->start();  ... view->stop();
// Historie: git log
#pragma once

#include <QWidget>

#include <memory>
#include <thread>

class TunerView : public QWidget {
    Q_OBJECT
public:
    explicit TunerView(QWidget* parent = nullptr);
    ~TunerView() override;

    /// Oeffnet den Audiostrom und beginnt zu messen. Meldet per `failed`, wenn
    /// das nicht geht — etwa weil eine andere Anwendung die Karte haelt.
    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept { return worker_.joinable(); }

signals:
    void failed(const QString& reason);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    /// Wird aus dem Messfaden heraus in den GUI-Faden gereicht.
    Q_INVOKABLE void apply(const QString& note, double cents, double hz, double quality, double levelDb);

    QString note_;
    double cents_{0.0};
    double hz_{0.0};
    double quality_{1.0};
    double level_db_{-120.0};
    bool have_{false};

    std::jthread worker_;
};
