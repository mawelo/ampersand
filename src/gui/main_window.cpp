// main_window.cpp — Hauptfenster (siehe main_window.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "main_window.hpp"

#include "block_panel.hpp"
#include "catalog_dialog.hpp"
#include "qc/hid_transport.hpp"
#include "meter_view.hpp"
#include "io_meter_view.hpp"
#include "grid_view.hpp"
#include "tuner_view.hpp"
#include "library_panel.hpp"
#include "theme.hpp"

#include "qc/device.hpp"
#include <nlohmann/json.hpp>
#include "qc/version.hpp"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QEvent>
#include <QMouseEvent>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QLabel>
#include <QMenu>
#include <QToolBar>
#include <QTimer>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSlider>
#include <QStatusBar>
#include <QTime>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Ampersand"));
    resize(1500, 900);
    buildUi();
}

void MainWindow::buildUi() {
    // ── menu ────────────────────────────────────────────────────────────
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    connectAction_ = file->addAction(QStringLiteral("&Connect"), QKeySequence(QStringLiteral("Ctrl+K")), this, [this] { emit connectRequested(); });
    disconnectAction_ = file->addAction(QStringLiteral("&Disconnect"), this, [this] { emit disconnectRequested(); });
    refreshAction_ = file->addAction(QStringLiteral("&Refresh live grid"), QKeySequence(QStringLiteral("F5")), this, [this] { emit refreshRequested(); });
    file->addSeparator();
    file->addAction(QStringLiteral("&Export chain as JSON…"), QKeySequence(QStringLiteral("Ctrl+E")), this, [this] {
        if (!view_) {
            onError(QStringLiteral("nothing to export: no preset loaded"));
            return;
        }
        const QString suggested = QStringLiteral("%1.json").arg(QString::fromStdString(view_->name).replace(QLatin1Char('/'), QLatin1Char('_')));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export signal chain"), suggested, QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            onError(QStringLiteral("cannot write %1").arg(path));
            return;
        }
        f.write(QByteArray::fromStdString(qc::view::chain_report(*view_).dump(2)));
        f.write("\n");
        onLog(QStringLiteral("exported chain report to %1").arg(path));
    });
    file->addSeparator();
    saveBackupAction_ = file->addAction(QStringLiteral("&Save backup to file…"), this, [this] { doSaveBackup(); });
    saveBackupAction_->setToolTip(QStringLiteral("Have the unit build a LocalBackup (~90 s) and save it as a private 0600 file."));
    restoreBackupAction_ = file->addAction(QStringLiteral("Restore backup from file…"), this, [this] { doRestoreBackup(); });
    restoreBackupAction_->setToolTip(QStringLiteral("Restore a LocalBackup file — DESTRUCTIVE: overwrites presets, captures and IRs."));
    saveBackupAction_->setEnabled(false);
    restoreBackupAction_->setEnabled(false);
    file->addSeparator();
    file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);
    auto* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About"), this, [this] {
        onLog(QString::fromUtf8(qc::version().data(), static_cast<qsizetype>(qc::version().size())));
    });

    // ── toolbar ─────────────────────────────────────────────────────────
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setMovable(false);
    tb->addAction(connectAction_);
    tb->addAction(disconnectAction_);
    tb->addSeparator();
    prevAction_ = tb->addAction(QStringLiteral("◀"), this, [this] { emit stepRequested(-1); });
    auto* header = new QWidget;
    auto* hl = new QVBoxLayout(header);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(0);
    presetName_ = new QLabel(QStringLiteral("—"));
    presetName_->setObjectName(QStringLiteral("presetName"));
    presetSlot_ = new QLabel(QStringLiteral("not connected"));
    presetSlot_->setObjectName(QStringLiteral("presetSlot"));
    hl->addWidget(presetName_);
    hl->addWidget(presetSlot_);
    header->setMinimumWidth(260);
    tb->addWidget(header);
    nextAction_ = tb->addAction(QStringLiteral("▶"), this, [this] { emit stepRequested(1); });
    tb->addAction(refreshAction_);
    tb->addSeparator();

    auto* scenes = new QWidget;
    auto* sl = new QHBoxLayout(scenes);
    sl->setContentsMargins(8, 0, 8, 0);
    sl->setSpacing(4);
    for (int i = 0; i < 8; ++i) {
        auto* b = new QToolButton;
        b->setText(qc::view::scene_letter(i).c_str());
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setMinimumSize(40, 34);
        b->setToolTip(QStringLiteral("Scene %1 — double-click: rename, right-click: colour").arg(b->text()));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QToolButton::clicked, this, [this, i] { emit sceneRequested(i); });
        connect(b, &QToolButton::customContextMenuRequested, this, [this, i, b](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(QStringLiteral("Rename scene %1…").arg(qc::view::scene_letter(i).c_str()), this, [this, i] { renameScene(i); });
            menu.addAction(QStringLiteral("Colour…"), this, [this, i] { recolorScene(i); });
            menu.exec(b->mapToGlobal(pos));
        });
        sceneButtons_[static_cast<std::size_t>(i)] = b;
        sl->addWidget(b);
    }
    tb->addWidget(scenes);
    tb->addSeparator();
    tempo_ = new QLabel(QStringLiteral(""));
    tempo_->setMinimumWidth(70);
    tb->addWidget(tempo_);
    tb->addSeparator();
    tuner_ = new QToolButton;
    tuner_->setText(QStringLiteral("Tuner"));
    tuner_->setCheckable(true);
    // Works over MIDI CC#45 (docs/evidence/84); the protobuf route is ignored by
    // the unit. It never reports its tuner state back, so the button shows what
    // was last asked for, not what the screen does.
    tuner_->setToolTip(QStringLiteral("Show the unit's built-in tuner (sent as MIDI CC#45 — the unit does not "
                                      "report its tuner state back)"));
    connect(tuner_, &QToolButton::toggled, this, [this](bool on) {
        // Zwei Dinge auf einen Druck: das Stimmgeraet des Geraets per MIDI und
        // die eigene Anzeige aus dem Audiostrom. Die zweite geht auch, wenn das
        // Geraet gar nicht per USB-HID verbunden ist.
        emit tunerRequested(on);
        if (!tunerView_) return;
        tunerView_->setVisible(on);
        if (on) {
            tunerView_->start();
        } else {
            tunerView_->stop();
        }
    });
    tb->addWidget(tuner_);
    tb->addSeparator();
    auto* volWidget = new QWidget;
    auto* vl = new QHBoxLayout(volWidget);
    vl->setContentsMargins(8, 0, 8, 0);
    vl->addWidget(new QLabel(QStringLiteral("Master")));
    volume_ = new QSlider(Qt::Horizontal);
    volume_->setRange(0, 100);
    volume_->setMinimumWidth(140);
    volume_->setToolTip(QStringLiteral("Master volume (the unit's knob soft-takes-over afterwards)"));
    connect(volume_, &QSlider::sliderReleased, this, [this] { emit masterVolumeRequested(static_cast<float>(volume_->value()) / 100.0f); });
    vl->addWidget(volume_);
    tb->addWidget(volWidget);
    busy_ = new QProgressBar;
    busy_->setRange(0, 0);
    busy_->setMaximumWidth(80);
    busy_->setTextVisible(false);
    busy_->hide();
    tb->addWidget(busy_);

    // ── central grid, darueber die Stimmanzeige ─────────────────────────
    grid_ = new GridView;
    tunerView_ = new TunerView;
    tunerView_->hide();
    connect(tunerView_, &TunerView::failed, this, [this](const QString& reason) {
        // Ehrlich melden statt leer anzeigen: Meist haelt eine andere Anwendung
        // die Karte, oder das Geraet haengt nicht am USB.
        statusBar()->showMessage(QStringLiteral("tuner: %1").arg(reason), 8000);
        if (tuner_) tuner_->setChecked(false);
    });
    auto* centre = new QWidget;
    auto* cl = new QVBoxLayout(centre);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(tunerView_);
    cl->addWidget(grid_, 1);
    setCentralWidget(centre);

    // ── docks ───────────────────────────────────────────────────────────
    blockPanel_ = new BlockPanel;
    auto* blockDock = new QDockWidget(QStringLiteral("Block"), this);
    blockDock->setWidget(blockPanel_);
    blockDock->setMinimumWidth(320);
    blockDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::RightDockWidgetArea, blockDock);

    library_ = new LibraryPanel;
    auto* libDock = new QDockWidget(QStringLiteral("Library"), this);
    libDock->setWidget(library_);
    libDock->setMinimumWidth(300);
    libDock->setFeatures(QDockWidget::DockWidgetMovable);
    addDockWidget(Qt::LeftDockWidgetArea, libDock);

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    auto* logDock = new QDockWidget(QStringLiteral("Log"), this);
    logDock->setWidget(log_);
    logDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    resizeDocks({logDock}, {340}, Qt::Vertical);

    // Pegelanzeige: eigener Bereich, der NUR misst, solange er sichtbar ist.
    // Die Audiokarte laesst sich nur einmal oeffnen - liefe sie dauerhaft,
    // koennten weder das Stimmgeraet noch `qcctl meter` sie noch bekommen.
    meterView_ = new MeterView;
    auto* meterDock = new QDockWidget(QStringLiteral("Meters"), this);
    meterDock->setWidget(meterView_);
    meterDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, meterDock);
    meterDock_ = meterDock;
    tabifyDockWidget(blockDock, meterDock);
    blockDock->raise();
    connect(meterDock, &QDockWidget::visibilityChanged, this, [this](bool sichtbar) {
        if (sichtbar) meterView_->start();
        else meterView_->stop();
    });
    connect(meterView_, &MeterView::failed, this, [this](const QString& grund) {
        onLog(QStringLiteral("meters: %1").arg(grund));
    });

    // Pegel, die das Geraet selbst meldet: laeuft ueber die USB-Sitzung (HID),
    // nicht ueber die Audiokarte, und vertraegt sich deshalb mit "Meters" und
    // dem Stimmgeraet. Bestellt wird nur, solange das Dock sichtbar ist.
    ioMeterView_ = new IoMeterView;
    auto* ioMeterDock = new QDockWidget(QStringLiteral("Device meters"), this);
    ioMeterDock->setWidget(ioMeterView_);
    ioMeterDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, ioMeterDock);
    ioMeterDock_ = ioMeterDock;
    tabifyDockWidget(meterDock, ioMeterDock);
    blockDock->raise();
    connect(ioMeterDock, &QDockWidget::visibilityChanged, this, [this](bool sichtbar) { emit ioMeterRequested(sichtbar); });

    logDock->raise();

    status_ = new QLabel(QStringLiteral("not connected"));
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(new QLabel(QString::fromUtf8(qc::version().data(), static_cast<qsizetype>(qc::version().size()))));

    // ── wiring ──────────────────────────────────────────────────────────
    connect(grid_, &GridView::cellClicked, this, [this](int row, int col, bool occupied) {
        if (!view_) return;
        if (occupied) blockPanel_->showBlock(*view_->rows[static_cast<std::size_t>(row)].block_at(col));
        else blockPanel_->showEmpty(row, col);
    });
    connect(grid_, &GridView::cellDoubleClicked, this, [this](int row, int col, bool occupied) {
        if (!view_) return;
        if (occupied) {
            const auto* b = view_->rows[static_cast<std::size_t>(row)].block_at(col);
            emit bypassRequested(row, col, !b->bypassed);
        } else {
            chooseBlock(row, col);
        }
    });
    connect(grid_, &GridView::blockMoveRequested, this, &MainWindow::moveBlockRequested);
    connect(grid_, &GridView::inputClicked, this, &MainWindow::chooseInputPort);
    connect(grid_, &GridView::outputClicked, this, &MainWindow::chooseOutputPort);

    connect(blockPanel_, &BlockPanel::paramChanged, this, &MainWindow::paramRequested);
    connect(blockPanel_, &BlockPanel::bypassToggled, this, &MainWindow::bypassRequested);
    connect(blockPanel_, &BlockPanel::removeRequested, this, &MainWindow::removeBlockRequested);
    connect(blockPanel_, &BlockPanel::addRequested, this, &MainWindow::chooseBlock);

    connect(library_, &LibraryPanel::refreshRequested, this, &MainWindow::presetsRequested);
    connect(library_, &LibraryPanel::foldersRequested, this, &MainWindow::foldersRequested);
    connect(library_, &LibraryPanel::recallRequested, this, &MainWindow::recallRequested);
    connect(library_, &LibraryPanel::saveRequested, this, &MainWindow::saveRequested);

    for (int i = 0; i < 8; ++i) {
        auto* b = sceneButtons_[static_cast<std::size_t>(i)];
        b->installEventFilter(this);
    }

    disconnectAction_->setEnabled(false);
    refreshAction_->setEnabled(false);
    prevAction_->setEnabled(false);
    nextAction_->setEnabled(false);

    // Auf USB warten: Ohne Verbindung wird alle 3 s nachgesehen, ob ein Geraet
    // NEU angesteckt wurde (Wechsel von "keins" zu "da"), und dann verbunden.
    // Ein schon beim Start steckendes Geraet verbindet weiter nur `--connect`
    // oder File ▸ Connect.
    usbDa_ = !qc::hid::enumerate().empty();
    usbTimer_ = new QTimer(this);
    usbTimer_->setInterval(3000);
    connect(usbTimer_, &QTimer::timeout, this, &MainWindow::pruefeUsb);
    usbTimer_->start();
}

void MainWindow::pruefeUsb() {
    const bool da = !qc::hid::enumerate().empty();
    const bool neu = da && !usbDa_;
    usbDa_ = da;
    if (neu && !connected_) {
        onLog(QStringLiteral("Quad Cortex found on USB - connecting"));
        emit connectRequested();
    }
}

void MainWindow::onConnected(const QString& summary) {
    connected_ = true;
    status_->setText(summary);
    connectAction_->setEnabled(false);
    disconnectAction_->setEnabled(true);
    refreshAction_->setEnabled(true);
    prevAction_->setEnabled(true);
    nextAction_->setEnabled(true);
    saveBackupAction_->setEnabled(true);
    restoreBackupAction_->setEnabled(true);
    library_->setEnabledAll(true);
    onLog(QStringLiteral("connected: %1").arg(summary));
    updateHeader();
    emit presetsRequested(library_->currentFolder());
}

void MainWindow::onDisconnected(const QString& reason) {
    connected_ = false;
    if (ioMeterView_) ioMeterView_->clear();
    status_->setText(reason);
    connectAction_->setEnabled(true);
    disconnectAction_->setEnabled(false);
    refreshAction_->setEnabled(false);
    prevAction_->setEnabled(false);
    nextAction_->setEnabled(false);
    saveBackupAction_->setEnabled(false);
    restoreBackupAction_->setEnabled(false);
    library_->setEnabledAll(false);
    view_.reset();
    catalog_.reset();
    grid_->clear();
    blockPanel_->showNothing();
    presetName_->setText(QStringLiteral("—"));
    presetSlot_->setText(QStringLiteral("not connected"));
    onLog(reason);
}

void MainWindow::onPresetView(const qc::view::PresetView& view) {
    view_ = view;
    grid_->setPreset(view);
    blockPanel_->update(view);
    for (int i = 0; i < 8; ++i) {
        auto* b = sceneButtons_[static_cast<std::size_t>(i)];
        const QString label = QString::fromStdString(view.scene_labels[static_cast<std::size_t>(i)]).trimmed();
        b->setToolTip(label.isEmpty() ? QStringLiteral("Scene %1").arg(qc::view::scene_letter(i).c_str()) : label);
        const QColor c = QColor::fromRgba(view.scene_colors[static_cast<std::size_t>(i)]);
        b->setStyleSheet(QStringLiteral("QToolButton { border: 2px solid %1; } QToolButton:checked { background: %1; color: #141416; }")
                             .arg(c.name()));
        b->setChecked(i == view.scene);
    }
    if (view.tempo_bpm) onTempo(*view.tempo_bpm);
    updateHeader();
}

void MainWindow::updateHeader() {
    if (!connected_) return;
    presetName_->setText(view_ ? QString::fromStdString(view_->name.empty() ? std::string("(unnamed)") : view_->name)
                               : QStringLiteral("loading…"));
    QString slot = position_ >= 0 ? QString::fromStdString(qc::view::position_to_slot(position_)) : QStringLiteral("?");
    QString where = folder_.isEmpty() ? QString() : (qc::view::is_factory_setlist(folder_.toStdString()) ? QStringLiteral("Factory") : QStringLiteral("User"));
    QString sceneText = view_ ? QString::fromStdString(qc::view::scene_letter(view_->scene)) : QStringLiteral("?");
    if (view_) {
        const QString label = QString::fromStdString(view_->scene_labels[static_cast<std::size_t>(std::clamp(view_->scene, 0, 7))]).trimmed();
        if (!label.isEmpty()) sceneText += QStringLiteral(" \"%1\"").arg(label);
    }
    presetSlot_->setText(QStringLiteral("#%1 %2 · %3%4 · scene %5")
                             .arg(position_ >= 0 ? QString::number(position_) : QStringLiteral("?"), slot, where,
                                  dirty_ ? QStringLiteral(" · edited") : QString(), sceneText));
}

void MainWindow::onScene(int scene) {
    if (scene >= 0 && scene < 8) sceneButtons_[static_cast<std::size_t>(scene)]->setChecked(true);
    if (view_) {
        view_->scene = scene;
        updateHeader();
    }
}

void MainWindow::onPosition(const QString& folder, int position, bool) {
    folder_ = folder;
    position_ = position;
    library_->setCurrent(folder, position);
    updateHeader();
}

void MainWindow::onDirty(bool dirty) {
    dirty_ = dirty;
    updateHeader();
}

void MainWindow::onMasterVolume(float volume) {
    if (!volume_->isSliderDown()) volume_->setValue(static_cast<int>(std::lround(volume * 100.0f)));
}

void MainWindow::onTempo(float bpm) { tempo_->setText(QStringLiteral("%1 bpm").arg(static_cast<int>(std::lround(bpm)))); }

void MainWindow::onFolders(const QVector<qc::Folder>& folders) { library_->setFolders(folders); }

void MainWindow::onPresets(const QString& folder, const QVector<qc::PresetEntry>& presets) {
    library_->setPresets(folder, presets);
    library_->setCurrent(folder_, position_);
}

void MainWindow::onCatalog(int models) {
    catalogModels_ = models;
    onLog(QStringLiteral("catalog ready: %1 models").arg(models));
}

void MainWindow::onLog(const QString& line) {
    log_->appendPlainText(QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line));
}

void MainWindow::onError(const QString& message) {
    onLog(QStringLiteral("ERROR: %1").arg(message));
    status_->setText(message);
}

void MainWindow::onBusy(bool on) { busy_->setVisible(on); }

void MainWindow::chooseInputPort(int row) {
    if (!connected_) return;
    QStringList names;
    QList<int> ids;
    for (const auto& [id, n] : qc::view::input_ports()) {
        names << QStringLiteral("%1 (%2)").arg(QString::fromStdString(n)).arg(id);
        ids << id;
    }
    int current = 0;
    if (view_) {
        const int cur = view_->rows[static_cast<std::size_t>(row)].in_port;
        const auto idx = ids.indexOf(cur);
        if (idx >= 0) current = static_cast<int>(idx);
    }
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, QStringLiteral("Row %1 input").arg(row + 1), QStringLiteral("Input port"), names, current, false, &ok);
    if (!ok) return;
    const auto idx = names.indexOf(pick);
    if (idx >= 0) emit chainInputRequested(row, ids[static_cast<int>(idx)]);
}

void MainWindow::chooseOutputPort(int row) {
    if (!connected_) return;
    QStringList names;
    QList<int> ids;
    for (const auto& [id, n] : qc::view::output_ports()) {
        names << QStringLiteral("%1 (%2)").arg(QString::fromStdString(n)).arg(id);
        ids << id;
    }
    int current = 0;
    if (view_) {
        const int cur = view_->rows[static_cast<std::size_t>(row)].out_port;
        const auto idx = ids.indexOf(cur);
        if (idx >= 0) current = static_cast<int>(idx);
    }
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, QStringLiteral("Row %1 output").arg(row + 1), QStringLiteral("Output port"), names, current, false, &ok);
    if (!ok) return;
    const auto idx = names.indexOf(pick);
    if (idx >= 0) emit chainOutputRequested(row, ids[static_cast<int>(idx)]);
}

void MainWindow::chooseBlock(int row, int column) {
    if (!connected_) return;
    if (catalog_) {
        if (auto id = CatalogDialog::pick(*catalog_, row, column, this)) emit setBlockRequested(row, column, static_cast<quint32>(*id));
        return;
    }
    bool ok = false;
    const int id = QInputDialog::getInt(this, QStringLiteral("Place block at row %1 column %2").arg(row + 1).arg(column + 1),
                                        QStringLiteral("Catalog not loaded yet - model id (see qcctl catalog <search>):"), 3001, 1, 999999, 1, &ok);
    if (ok) emit setBlockRequested(row, column, static_cast<quint32>(id));
}

void MainWindow::onCatalogLoaded(const qc::catalog::Catalog& catalog) { catalog_ = catalog; }

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        for (int i = 0; i < 8; ++i) {
            if (watched == sceneButtons_[static_cast<std::size_t>(i)]) {
                renameScene(i);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::selectCell(int row, int column) {
    if (!view_ || row < 0 || static_cast<std::size_t>(row) >= view_->rows.size()) return;
    grid_->setSelected(row, column);
    if (const auto* b = view_->rows[static_cast<std::size_t>(row)].block_at(column)) {
        blockPanel_->showBlock(*b);
    } else {
        blockPanel_->showEmpty(row, column);
    }
}

void MainWindow::showDeviceMeters() {
    if (!ioMeterDock_) return;
    ioMeterDock_->show();
    ioMeterDock_->raise();
}

void MainWindow::onIoLevels(const qc::iometer::Levels& levels) {
    if (ioMeterView_) ioMeterView_->setLevels(levels);
}

void MainWindow::showMeters() {
    if (!meterDock_) return;
    meterDock_->show();
    meterDock_->raise();
}

void MainWindow::setTunerActive(bool on) {
    if (tuner_) tuner_->setChecked(on);  // loest das Umschalten samt Anzeige aus
}

void MainWindow::renameScene(int scene) {
    if (!connected_ || !view_) return;
    const QString current = QString::fromStdString(view_->scene_labels[static_cast<std::size_t>(scene)]).trimmed();
    bool ok = false;
    const QString label = QInputDialog::getText(this, QStringLiteral("Rename scene %1").arg(qc::view::scene_letter(scene).c_str()),
                                                QStringLiteral("Label (empty clears it):"), QLineEdit::Normal, current, &ok);
    if (!ok) return;
    emit sceneLabelRequested(scene, label.trimmed());
    onLog(QStringLiteral("scene %1 label -> \"%2\"").arg(qc::view::scene_letter(scene).c_str(), label.trimmed()));
}

void MainWindow::recolorScene(int scene) {
    if (!connected_ || !view_) return;
    const QColor current = QColor::fromRgba(view_->scene_colors[static_cast<std::size_t>(scene)]);
    const QColor picked = QColorDialog::getColor(current, this, QStringLiteral("Colour of scene %1").arg(qc::view::scene_letter(scene).c_str()));
    if (!picked.isValid()) return;
    emit sceneColorRequested(scene, 0xFF000000u | (static_cast<quint32>(picked.rgb()) & 0x00FFFFFFu));
}

void MainWindow::doSaveBackup() {
    if (!connected_) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save LocalBackup"),
                                                      QStringLiteral("quad-cortex-backup.json"), QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    saveBackupAction_->setEnabled(false);
    restoreBackupAction_->setEnabled(false);
    onLog(QStringLiteral("backup-save: asking the unit to build a LocalBackup (~90 s) …"));
    emit saveBackupRequested(path);
}

void MainWindow::doRestoreBackup() {
    if (!connected_) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Restore LocalBackup"), QString(),
                                                      QStringLiteral("JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    const auto choice = QMessageBox::warning(
        this, QStringLiteral("Restore backup?"),
        QStringLiteral("This overwrites the unit's presets, captures and IRs with the contents of\n\n%1\n\n"
                       "This cannot be undone. Continue?")
            .arg(path),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes) return;
    saveBackupAction_->setEnabled(false);
    restoreBackupAction_->setEnabled(false);
    onLog(QStringLiteral("backup-restore: sending %1 to the unit …").arg(path));
    emit restoreBackupRequested(path);
}

void MainWindow::onBackupFinished(bool ok, const QString& message) {
    if (connected_) {
        saveBackupAction_->setEnabled(true);
        restoreBackupAction_->setEnabled(true);
    }
    onLog(QStringLiteral("backup: %1").arg(message));
    if (ok)
        QMessageBox::information(this, QStringLiteral("Backup"), message);
    else
        QMessageBox::critical(this, QStringLiteral("Backup failed"), message);
}

bool MainWindow::saveScreenshot(const QString& path) {
    const QPixmap pm = grab();
    return pm.save(path, "PNG");
}
