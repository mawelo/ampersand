// main_window.hpp — Hauptfenster von cortex-control
//
// Wer:      Martin.Lonkwitz
// Was:      Toolbar (Verbinden, Preset-Navigation, Szenen A-H, Master-Volume),
//           Grid in der Mitte, Docks: Block-Editor (rechts), Bibliothek (links),
//           Log (unten). Alle Geraeteaktionen laufen ueber Signale zum
//           Controller im Worker-Thread.
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include "qc/catalog.hpp"
#include "qc/io_meter.hpp"
#include "qc/preset_view.hpp"

#include <QMainWindow>
#include <QVector>

#include <array>
#include <optional>

class GridView;
class BlockPanel;
class LibraryPanel;
class QLabel;
class QToolButton;
class QSlider;
class QPlainTextEdit;
class QAction;
class QProgressBar;
class QDockWidget;

namespace qc {
struct Folder;
struct PresetEntry;
}  // namespace qc

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

signals:
    void connectRequested();
    void disconnectRequested();
    void refreshRequested();
    void recallRequested(const QString& folder, int position);
    void stepRequested(int delta);
    void sceneRequested(int scene);
    void paramRequested(int row, int column, int index, float value);
    void bypassRequested(int row, int column, bool bypassed);
    void removeBlockRequested(int row, int column);
    void setBlockRequested(int row, int column, quint32 modelId);
    void moveBlockRequested(int fromRow, int fromColumn, int toRow, int toColumn);
    void chainInputRequested(int row, int port);
    void chainOutputRequested(int row, int port);
    void saveRequested(const QString& folder, int position, const QString& name, int instrument);
    void masterVolumeRequested(float volume);
    void tunerRequested(bool show);
    /// Pegelmeldungen des Geraets ein/aus (folgt der Sichtbarkeit des Docks).
    void ioMeterRequested(bool on);

public:
    /// Stimmanzeige beim Start einschalten (Kommandozeile, Kopfzeilen-Knopf
    /// spiegelt den Zustand).
    void setTunerActive(bool on);

    /// Pegelanzeige beim Start nach vorn holen. Gebraucht fuer `--meters` und
    /// damit sich die Anzeige im Bildschirmfoto belegen laesst.
    void showMeters();
    /// Die vom Geraet gemeldeten Pegel nach vorn holen (`--device-meters`).
    void showDeviceMeters();

signals:
    void foldersRequested();
    void presetsRequested(const QString& folder);
    void sceneLabelRequested(int scene, const QString& label);
    void sceneColorRequested(int scene, quint32 argb);
    void saveBackupRequested(const QString& path);
    void restoreBackupRequested(const QString& path);

public slots:
    /// Zelle von aussen auswaehlen, 0-basiert. Gebraucht fuer `--select` auf
    /// der Kommandozeile: ohne das ist der Parameter-Editor nur per Mausklick
    /// zu erreichen und damit im Bildschirmfoto nicht belegbar.
    void selectCell(int row, int column);

    void onConnected(const QString& summary);
    void onDisconnected(const QString& reason);
    void onPresetView(const qc::view::PresetView& view);
    void onScene(int scene);
    void onPosition(const QString& folder, int position, bool factory);
    void onDirty(bool dirty);
    void onMasterVolume(float volume);
    void onIoLevels(const qc::iometer::Levels& levels);
    void onTempo(float bpm);
    void onFolders(const QVector<qc::Folder>& folders);
    void onPresets(const QString& folder, const QVector<qc::PresetEntry>& presets);
    void onCatalog(int models);
    void onCatalogLoaded(const qc::catalog::Catalog& catalog);
    void onLog(const QString& line);
    void onError(const QString& message);
    void onBusy(bool on);
    void onBackupFinished(bool ok, const QString& message);

    /// Grab the whole window into a PNG (for automated verification).
    bool saveScreenshot(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void renameScene(int scene);
    void recolorScene(int scene);
    void doSaveBackup();
    void doRestoreBackup();
    void chooseInputPort(int row);
    void chooseOutputPort(int row);
    void chooseBlock(int row, int column);
    void updateHeader();
    void pruefeUsb();

    GridView* grid_{nullptr};
    BlockPanel* blockPanel_{nullptr};
    LibraryPanel* library_{nullptr};
    QPlainTextEdit* log_{nullptr};
    QLabel* presetName_{nullptr};
    QLabel* presetSlot_{nullptr};
    QLabel* status_{nullptr};
    QLabel* tempo_{nullptr};
    QSlider* volume_{nullptr};
    QToolButton* tuner_{nullptr};
    class TunerView* tunerView_{nullptr};
    class MeterView* meterView_{nullptr};
    QDockWidget* meterDock_{nullptr};
    class IoMeterView* ioMeterView_{nullptr};
    QDockWidget* ioMeterDock_{nullptr};
    QProgressBar* busy_{nullptr};
    class QTimer* usbTimer_{nullptr};  ///< wartet ohne Verbindung auf ein angestecktes Geraet
    bool usbDa_{false};                ///< beim letzten Blick war ein Geraet am USB
    std::array<QToolButton*, 8> sceneButtons_{};
    QAction* connectAction_{nullptr};
    QAction* disconnectAction_{nullptr};
    QAction* prevAction_{nullptr};
    QAction* nextAction_{nullptr};
    QAction* refreshAction_{nullptr};
    QAction* saveBackupAction_{nullptr};
    QAction* restoreBackupAction_{nullptr};
    std::optional<qc::view::PresetView> view_;
    QString folder_;
    int position_{-1};
    bool dirty_{false};
    bool connected_{false};
    int catalogModels_{0};
    std::optional<qc::catalog::Catalog> catalog_;
};
