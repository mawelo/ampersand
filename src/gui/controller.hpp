// controller.hpp — Bruecke zwischen GUI-Thread und QuadCortex (Worker-Thread)
//
// Wer:      Martin.Lonkwitz
// Was:      QObject, das in einem eigenen Thread lebt: alle Geraetezugriffe
//           laufen dort (blockierende Requests), Ergebnisse gehen als Signale
//           (queued) an die GUI. Geraete-Pushes werden hier zu View-Updates.
// Wann:     2026-09-04
// Warum:    Die GUI darf nie auf USB warten; der Zustand (Preset, Szene,
//           Katalog) hat genau einen Besitzer-Thread.
// Projekt:  Ampersand (GUI)
// Nutzung:  controller->moveToThread(&thread); connect(window, &MainWindow::x, controller, &Controller::y);
// Historie: git log
#pragma once

#include "qc/device.hpp"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <memory>
#include <optional>

Q_DECLARE_METATYPE(qc::view::PresetView)
Q_DECLARE_METATYPE(qc::catalog::Catalog)
Q_DECLARE_METATYPE(qc::Folder)
Q_DECLARE_METATYPE(qc::PresetEntry)
Q_DECLARE_METATYPE(QVector<qc::Folder>)
Q_DECLARE_METATYPE(QVector<qc::PresetEntry>)
Q_DECLARE_METATYPE(qc::iometer::Levels)

class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    static void registerTypes();

    /// Seriennummer in der Zusammenfassung unterdruecken. Gebraucht fuer
    /// Bildschirmfotos in der Anleitung: ein Bild wandert weiter, die Nummer
    /// soll es nicht. Vorgabe des Nutzers, 2026-09-22.
    void setRedactSerial(bool on) noexcept { redact_serial_ = on; }

public slots:
    void connectDevice();
    void disconnectDevice();
    void refreshCurrent();
    void recallPreset(const QString& folder, int position);
    void stepPreset(int delta);
    void switchScene(int scene);
    void setParam(int row, int column, int index, float value);
    void setParamText(int row, int column, int index, const QString& text);
    void setBypass(int row, int column, bool bypassed);
    void removeBlock(int row, int column);
    void setBlock(int row, int column, quint32 modelId);
    void moveBlock(int fromRow, int fromColumn, int toRow, int toColumn);
    void setChainInput(int row, int port);
    void setChainOutput(int row, int port);
    void savePreset(const QString& folder, int position, const QString& name, int instrument);
    void setMasterVolume(float volume);
    void setTuner(bool show);
    /// Pegelmeldungen des Geraets ein/aus. Der Wunsch gilt ueber Verbindungen
    /// hinweg: ist das Geraet noch nicht verbunden, wird nach dem Verbinden
    /// bestellt. Solange eingeschaltet, frischt ein Zeitgeber das Abo auf
    /// (qc::iometer::kResubscribeInterval).
    void setIoMeter(bool on);
    void listFolders();
    void listPresets(const QString& folder);
    void setSceneLabel(int scene, const QString& label);
    void setSceneColor(int scene, quint32 argb);
    /// LocalBackup vom Geraet holen und privat (0600) nach `path` sichern.
    void saveBackup(const QString& path);
    /// LocalBackup aus `path` zurueckspielen. DESTRUKTIV (ueberschreibt
    /// Presets/Captures/IRs) - die Sicherheitsabfrage liegt in der GUI.
    void restoreBackup(const QString& path);

signals:
    void connected(const QString& summary);
    void disconnected(const QString& reason);
    void presetViewChanged(const qc::view::PresetView& view);
    void sceneChanged(int scene);
    void positionChanged(const QString& folder, int position, bool factory);
    void dirtyChanged(bool dirty);
    void masterVolumeChanged(float volume);
    void ioLevelsChanged(const qc::iometer::Levels& levels);
    void tempoChanged(float bpm);
    void foldersListed(const QVector<qc::Folder>& folders);
    void presetsListed(const QString& folder, const QVector<qc::PresetEntry>& presets);
    void catalogReady(int models);
    void catalogLoaded(const qc::catalog::Catalog& catalog);
    void logMessage(const QString& line);
    void errorOccurred(const QString& message);
    void busy(bool on);
    /// Backup/Restore abgeschlossen; `message` ist menschenlesbar.
    void backupFinished(bool ok, const QString& message);

private:
    bool redact_serial_{false};
    void handleEvent(const qc::Event& e);
    void rebuildView();
    void scheduleRefresh();
    template <class F>
    void guarded(const char* what, F&& fn);

    std::unique_ptr<qc::QuadCortex> qc_;
    const qc::catalog::Catalog* catalog_{nullptr};
    std::optional<::BinaryPreset> preset_;
    int scene_{0};
    QString folder_;
    int position_{-1};
    bool factory_{false};
    QTimer* refreshTimer_{nullptr};
    QTimer* ioTimer_{nullptr};
    bool ioWanted_{false};
    void subscribeIo();
};
