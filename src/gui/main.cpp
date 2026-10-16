// main.cpp — Ampersand: Qt-GUI fuer den Neural DSP Quad Cortex
//
// Wer:      Martin.Lonkwitz
// Was:      Startet Anwendung, Worker-Thread mit Controller und Hauptfenster;
//           verdrahtet Signale. Optionen: --connect (sofort verbinden),
//           --screenshot <png> [--after <ms>] (Bild speichern und beenden;
//           mit QT_QPA_PLATFORM=offscreen headless nutzbar).
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "controller.hpp"
#include "main_window.hpp"
#include "theme.hpp"

#include "qc/log.hpp"
#include "qc/version.hpp"

#include <string_view>

#include <QApplication>
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QThread>
#include <QTimer>

#include <print>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ampersand"));
    // qc::version() beginnt mit dem Programmnamen ("ampersand 0.1.0+..."), und
    // Qt stellt bei --version den Anwendungsnamen ohnehin voran. Ohne das
    // Abschneiden stuende der Name doppelt da.
    const auto full = qc::version();
    const auto space = full.find(' ');
    const auto ver = space == std::string_view::npos ? full : full.substr(space + 1);
    QApplication::setApplicationVersion(QString::fromUtf8(ver.data(), static_cast<qsizetype>(ver.size())));
    app.setStyleSheet(theme::stylesheet());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Linux control software for the Neural DSP Quad Cortex"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption connectOpt(QStringLiteral("connect"), QStringLiteral("connect to the device on startup"));
    QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("save a PNG of the window and quit"), QStringLiteral("file"));
    QCommandLineOption afterOpt(QStringLiteral("after"), QStringLiteral("delay before the screenshot in ms (default 4000)"), QStringLiteral("ms"), QStringLiteral("4000"));
    QCommandLineOption selectOpt(QStringLiteral("select"),
                                 QStringLiteral("select a grid cell before the screenshot, 1-based as the unit labels them"),
                                 QStringLiteral("row,col"));
    QCommandLineOption tunerOpt(QStringLiteral("tuner"), QStringLiteral("start with the tuner showing (audio-based, needs the unit's sound card)"));
    QCommandLineOption verboseOpt(QStringLiteral("verbose"), QStringLiteral("debug log on stderr"));
    parser.addOption(connectOpt);
    parser.addOption(shotOpt);
    parser.addOption(afterOpt);
    parser.addOption(selectOpt);
    QCommandLineOption metersOpt(QStringLiteral("meters"), QStringLiteral("start with the level meters showing (audio-based, needs the unit's sound card)"));
    parser.addOption(tunerOpt);
    parser.addOption(metersOpt);
    QCommandLineOption deviceMetersOpt(QStringLiteral("device-meters"), QStringLiteral("start with the levels the unit reports itself (IOMeter over USB; only while a signal is present)"));
    parser.addOption(deviceMetersOpt);
    QCommandLineOption redactOpt(QStringLiteral("redact-serial"), QStringLiteral("hide the serial number in the window (for screenshots)"));
    parser.addOption(redactOpt);
    parser.addOption(verboseOpt);
    parser.process(app);

    qc::log::set_level(parser.isSet(verboseOpt) ? qc::log::Level::Debug : qc::log::Level::Info);
    Controller::registerTypes();
    // Welche Plattform Qt gewaehlt hat (wayland, xcb, offscreen) - die AppImage
    // bringt seit 2026-10-02 wayland und xcb mit (Beleg 188).
    qc::log::info("Qt platform: {}", QGuiApplication::platformName().toStdString());

    MainWindow window;
    const bool startWithTuner = parser.isSet(tunerOpt);
    QThread worker;
    worker.setObjectName(QStringLiteral("qc-worker"));
    auto* controller = new Controller;
    controller->setRedactSerial(parser.isSet(redactOpt));
    controller->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished, controller, &QObject::deleteLater);

    // GUI -> controller (queued: the controller lives in the worker thread)
    QObject::connect(&window, &MainWindow::connectRequested, controller, &Controller::connectDevice);
    QObject::connect(&window, &MainWindow::disconnectRequested, controller, &Controller::disconnectDevice);
    QObject::connect(&window, &MainWindow::refreshRequested, controller, &Controller::refreshCurrent);
    QObject::connect(&window, &MainWindow::recallRequested, controller, &Controller::recallPreset);
    QObject::connect(&window, &MainWindow::stepRequested, controller, &Controller::stepPreset);
    QObject::connect(&window, &MainWindow::sceneRequested, controller, &Controller::switchScene);
    QObject::connect(&window, &MainWindow::paramRequested, controller, &Controller::setParam);
    QObject::connect(&window, &MainWindow::bypassRequested, controller, &Controller::setBypass);
    QObject::connect(&window, &MainWindow::removeBlockRequested, controller, &Controller::removeBlock);
    QObject::connect(&window, &MainWindow::setBlockRequested, controller, &Controller::setBlock);
    QObject::connect(&window, &MainWindow::moveBlockRequested, controller, &Controller::moveBlock);
    QObject::connect(&window, &MainWindow::chainInputRequested, controller, &Controller::setChainInput);
    QObject::connect(&window, &MainWindow::chainOutputRequested, controller, &Controller::setChainOutput);
    QObject::connect(&window, &MainWindow::saveRequested, controller, &Controller::savePreset);
    QObject::connect(&window, &MainWindow::masterVolumeRequested, controller, &Controller::setMasterVolume);
    QObject::connect(&window, &MainWindow::tunerRequested, controller, &Controller::setTuner);
    QObject::connect(&window, &MainWindow::ioMeterRequested, controller, &Controller::setIoMeter);
    QObject::connect(&window, &MainWindow::foldersRequested, controller, &Controller::listFolders);
    QObject::connect(&window, &MainWindow::presetsRequested, controller, &Controller::listPresets);
    QObject::connect(&window, &MainWindow::sceneLabelRequested, controller, &Controller::setSceneLabel);
    QObject::connect(&window, &MainWindow::sceneColorRequested, controller, &Controller::setSceneColor);
    QObject::connect(&window, &MainWindow::saveBackupRequested, controller, &Controller::saveBackup);
    QObject::connect(&window, &MainWindow::restoreBackupRequested, controller, &Controller::restoreBackup);
    QObject::connect(&window, &MainWindow::deletePresetRequested, controller, &Controller::deletePreset);
    QObject::connect(&window, &MainWindow::movePresetRequested, controller, &Controller::movePreset);

    // controller -> GUI
    QObject::connect(controller, &Controller::connected, &window, &MainWindow::onConnected);
    QObject::connect(controller, &Controller::disconnected, &window, &MainWindow::onDisconnected);
    QObject::connect(controller, &Controller::presetViewChanged, &window, &MainWindow::onPresetView);
    QObject::connect(controller, &Controller::sceneChanged, &window, &MainWindow::onScene);
    QObject::connect(controller, &Controller::positionChanged, &window, &MainWindow::onPosition);
    QObject::connect(controller, &Controller::dirtyChanged, &window, &MainWindow::onDirty);
    QObject::connect(controller, &Controller::masterVolumeChanged, &window, &MainWindow::onMasterVolume);
    QObject::connect(controller, &Controller::ioLevelsChanged, &window, &MainWindow::onIoLevels);
    QObject::connect(controller, &Controller::tempoChanged, &window, &MainWindow::onTempo);
    QObject::connect(controller, &Controller::foldersListed, &window, &MainWindow::onFolders);
    QObject::connect(controller, &Controller::presetsListed, &window, &MainWindow::onPresets);
    QObject::connect(controller, &Controller::catalogReady, &window, &MainWindow::onCatalog);
    QObject::connect(controller, &Controller::catalogLoaded, &window, &MainWindow::onCatalogLoaded);
    QObject::connect(controller, &Controller::logMessage, &window, &MainWindow::onLog);
    QObject::connect(controller, &Controller::errorOccurred, &window, &MainWindow::onError);
    QObject::connect(controller, &Controller::busy, &window, &MainWindow::onBusy);
    QObject::connect(controller, &Controller::backupFinished, &window, &MainWindow::onBackupFinished);

    worker.start();
    window.show();
    if (startWithTuner) window.setTunerActive(true);
    if (parser.isSet(metersOpt)) window.showMeters();
    if (parser.isSet(deviceMetersOpt)) window.showDeviceMeters();
    window.onLog(QApplication::applicationVersion());

    if (parser.isSet(connectOpt) || parser.isSet(shotOpt)) {
        QMetaObject::invokeMethod(controller, &Controller::connectDevice, Qt::QueuedConnection);
    }
    if (parser.isSet(shotOpt)) {
        const QString path = parser.value(shotOpt);
        const int after = parser.value(afterOpt).toInt();
        // Zeile und Spalte sind 1-basiert wie ueberall am Geraet und in qcctl.
        int selRow = -1;
        int selCol = -1;
        if (parser.isSet(selectOpt)) {
            const auto teile = parser.value(selectOpt).split(QLatin1Char(','));
            if (teile.size() != 2) {
                std::println(stderr, "--select wants row,col (1-based), e.g. --select 1,2");
                return 2;
            }
            selRow = teile[0].trimmed().toInt() - 1;
            selCol = teile[1].trimmed().toInt() - 1;
        }
        QTimer::singleShot(after, &window, [&window, path, selRow, selCol] {
            if (selRow >= 0) window.selectCell(selRow, selCol);
            // Ein Durchlauf der Ereignisschlange, damit der Editor steht,
            // bevor das Bild genommen wird.
            QTimer::singleShot(250, &window, [&window, path] {
                const bool ok = window.saveScreenshot(path);
                std::println(stderr, "screenshot {}: {}", path.toStdString(), ok ? "saved" : "FAILED");
                QApplication::exit(ok ? 0 : 1);
            });
        });
    }

    const int rc = QApplication::exec();
    QMetaObject::invokeMethod(controller, &Controller::disconnectDevice, Qt::BlockingQueuedConnection);
    worker.quit();
    worker.wait(5000);
    return rc;
}
