// controller.cpp — Worker-seitige Geraetesteuerung (siehe controller.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "controller.hpp"

#include "qc/file_write.hpp"
#include "qc/log.hpp"

#include <QThread>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}  // namespace

Controller::Controller(QObject* parent) : QObject(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(350);
    connect(refreshTimer_, &QTimer::timeout, this, &Controller::refreshCurrent);
    ioTimer_ = new QTimer(this);
    ioTimer_->setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(qc::iometer::kResubscribeInterval));
    connect(ioTimer_, &QTimer::timeout, this, &Controller::subscribeIo);
}

Controller::~Controller() {
    if (qc_) {
        qc_->set_event_sink({});
        qc_->disconnect();
    }
}

void Controller::registerTypes() {
    qRegisterMetaType<qc::view::PresetView>("qc::view::PresetView");
    qRegisterMetaType<qc::catalog::Catalog>("qc::catalog::Catalog");
    qRegisterMetaType<qc::Folder>("qc::Folder");
    qRegisterMetaType<qc::PresetEntry>("qc::PresetEntry");
    qRegisterMetaType<QVector<qc::Folder>>("QVector<qc::Folder>");
    qRegisterMetaType<QVector<qc::PresetEntry>>("QVector<qc::PresetEntry>");
    qRegisterMetaType<qc::iometer::Levels>("qc::iometer::Levels");
}

template <class F>
void Controller::guarded(const char* what, F&& fn) {
    if (!qc_ || !qc_->connected()) {
        emit errorOccurred(QStringLiteral("%1: not connected").arg(what));
        return;
    }
    auto r = fn(*qc_);
    if (!r) emit errorOccurred(QStringLiteral("%1: %2").arg(what, qs(r.error())));
}

void Controller::connectDevice() {
    if (qc_ && qc_->connected()) return;
    emit busy(true);
    emit logMessage(QStringLiteral("opening USB device ..."));
    qc::Session::Options opts;
    auto dev = qc::QuadCortex::open(opts);
    if (!dev) {
        emit busy(false);
        emit errorOccurred(qs(dev.error()));
        return;
    }
    qc_ = std::move(*dev);
    qc_->set_event_sink([this](const qc::Event& e) {
        // RX thread -> our worker thread.
        QMetaObject::invokeMethod(this, [this, e] { handleEvent(e); }, Qt::QueuedConnection);
    });
    emit logMessage(QStringLiteral("handshake ..."));
    if (auto c = qc_->connect(); !c) {
        emit busy(false);
        emit errorOccurred(qs(c.error()));
        qc_.reset();
        return;
    }
    const auto v = qc_->version();
    // The unit carries its CorOS release in the zenos field (e.g. "4.1.0");
    // when it is a git hash instead, fall back to the raw fw id.
    const bool zenos_is_release = !v.zenos_git_hash.empty() && v.zenos_git_hash.size() <= 12 &&
        std::all_of(v.zenos_git_hash.begin(), v.zenos_git_hash.end(),
                    [](unsigned char c) { return std::isdigit(c) || c == '.'; });
    const QString coros = zenos_is_release ? QStringLiteral("CorOS %1").arg(qs(v.zenos_git_hash))
                                           : QStringLiteral("fw %1").arg(qs(v.app_fw_version));
    const QString seriennummer = redact_serial_ ? QStringLiteral("(hidden)") : qs(v.serial_number);
    emit connected(QStringLiteral("%1  %2  serial %3")
                       .arg(v.custom_name.empty() ? QStringLiteral("Quad Cortex") : qs(v.custom_name), coros,
                            seriennummer));
    emit logMessage(QStringLiteral("connected on %1").arg(qs(qc_->session().transport_description())));

    if (auto cat = qc_->catalog(30000ms)) {
        catalog_ = *cat;
        emit catalogReady(static_cast<int>(catalog_->size()));
        emit catalogLoaded(*catalog_);
    } else {
        emit logMessage(QStringLiteral("catalog unavailable: %1").arg(qs(cat.error())));
    }
    if (auto pos = qc_->current_position(5000ms)) {
        folder_ = qs(pos->folder_key);
        position_ = pos->position;
        factory_ = pos->is_factory;
        emit positionChanged(folder_, position_, factory_);
    }
    if (auto sc = qc_->active_scene(5000ms)) {
        scene_ = *sc;
        emit sceneChanged(scene_);
    }
    if (auto mv = qc_->master_volume(3000ms)) emit masterVolumeChanged(*mv);
    refreshCurrent();
    if (ioWanted_) {
        subscribeIo();
        ioTimer_->start();
    }
    emit busy(false);
}

void Controller::disconnectDevice() {
    if (!qc_) return;
    ioTimer_->stop();
    qc_->set_event_sink({});
    qc_->disconnect();
    qc_.reset();
    catalog_ = nullptr;
    preset_.reset();
    emit disconnected(QStringLiteral("disconnected"));
}

void Controller::refreshCurrent() {
    if (!qc_ || !qc_->connected()) return;
    auto p = qc_->read_current_preset(15000ms);
    if (!p) {
        emit logMessage(QStringLiteral("live read: %1").arg(qs(p.error())));
        return;
    }
    preset_ = std::move(*p);
    rebuildView();
}

void Controller::rebuildView() {
    if (!preset_) return;
    emit presetViewChanged(qc::view::build(*preset_, catalog_, scene_));
}

void Controller::scheduleRefresh() { refreshTimer_->start(); }

void Controller::handleEvent(const qc::Event& e) {
    std::visit(
        [this](const auto& ev) {
            using T = std::decay_t<decltype(ev)>;
            if constexpr (std::is_same_v<T, qc::event::PresetRecalled>) {
                preset_ = ev.preset;
                if (ev.reason == 0 && !ev.request_id) scene_ = static_cast<int>(ev.preset.default_scene());
                rebuildView();
                emit logMessage(QStringLiteral("preset: %1").arg(qs(ev.preset.name())));
            } else if constexpr (std::is_same_v<T, qc::event::GridDelta>) {
                scheduleRefresh();
            } else if constexpr (std::is_same_v<T, qc::event::SceneSelected>) {
                if (scene_ != ev.scene) {
                    scene_ = ev.scene;
                    emit sceneChanged(scene_);
                    rebuildView();
                }
            } else if constexpr (std::is_same_v<T, qc::event::SceneLabel> || std::is_same_v<T, qc::event::SceneColor>) {
                scheduleRefresh();
            } else if constexpr (std::is_same_v<T, qc::event::SetlistPosition>) {
                folder_ = qs(ev.folder_key);
                position_ = ev.position;
                factory_ = ev.is_factory;
                emit positionChanged(folder_, position_, factory_);
            } else if constexpr (std::is_same_v<T, qc::event::MasterVolume>) {
                emit masterVolumeChanged(ev.volume);
            } else if constexpr (std::is_same_v<T, qc::event::PresetDirty>) {
                emit dirtyChanged(ev.dirty);
            } else if constexpr (std::is_same_v<T, qc::event::Tempo>) {
                if (ev.bpm) emit tempoChanged(*ev.bpm);
            } else if constexpr (std::is_same_v<T, qc::event::IoMeter>) {
                emit ioLevelsChanged(ev.levels);
            } else if constexpr (std::is_same_v<T, qc::event::Lost>) {
                ioTimer_->stop();
                emit disconnected(QStringLiteral("device lost: %1").arg(qs(ev.reason)));
                catalog_ = nullptr;
            }
        },
        e);
}

void Controller::recallPreset(const QString& folder, int position) {
    guarded("recall", [&](qc::QuadCortex& d) { return d.recall_preset(folder.toStdString(), position); });
}

void Controller::stepPreset(int delta) {
    if (position_ < 0 || folder_.isEmpty()) {
        emit errorOccurred(QStringLiteral("current position unknown"));
        return;
    }
    const int next = std::clamp(position_ + delta, 0, qc::view::kSlotsPerSetlist - 1);
    recallPreset(folder_, next);
}

void Controller::switchScene(int scene) {
    guarded("scene", [&](qc::QuadCortex& d) { return d.switch_scene(scene); });
    scene_ = scene;
    emit sceneChanged(scene_);
    rebuildView();
}

void Controller::setParam(int row, int column, int index, float value) {
    // No read-back here: a knob sends a stream of values, and reading the whole
    // preset after each one would make the control lag. The view stays in step
    // through the cached preset below and the unit's own Grid push.
    guarded("set param", [&](qc::QuadCortex& d) { return d.set_param(row, column, index, value, {}, false); });
    // Keep the cached preset in step so the view does not snap back before the echo arrives.
    if (preset_ && row < preset_->chains_size()) {
        auto* chain = preset_->mutable_chains(row);
        for (int mi = 0; mi < chain->models_size(); ++mi) {
            auto* m = chain->mutable_models(mi);
            const int col = m->has_column() ? static_cast<int>(m->column()) : mi;
            if (col != column) continue;
            if (index < m->params_size()) {
                auto* p = m->mutable_params(index);
                const int idx = (p->scene_mode() && scene_ < p->param_values_size()) ? scene_ : 0;
                if (p->param_values_size() == 0) p->add_param_values();
                p->mutable_param_values(idx)->set_float_value(value);
            }
        }
    }
}

void Controller::setParamText(int row, int column, int index, const QString& text) {
    guarded("set param", [&](qc::QuadCortex& d) { return d.set_param_text(row, column, index, text.toStdString()); });
    scheduleRefresh();
}

void Controller::setBypass(int row, int column, bool bypassed) {
    guarded("bypass", [&](qc::QuadCortex& d) { return d.set_bypass(row, column, bypassed); });
    scheduleRefresh();
}

void Controller::removeBlock(int row, int column) {
    guarded("remove block", [&](qc::QuadCortex& d) { return d.remove_block(row, column); });
    scheduleRefresh();
}

void Controller::setBlock(int row, int column, quint32 modelId) {
    emit busy(true);
    guarded("place block", [&](qc::QuadCortex& d) { return d.set_block(row, column, modelId); });
    emit busy(false);
    scheduleRefresh();
}

void Controller::moveBlock(int fromRow, int fromColumn, int toRow, int toColumn) {
    guarded("move block", [&](qc::QuadCortex& d) { return d.move_block(fromRow, fromColumn, toRow, toColumn); });
    emit logMessage(QStringLiteral("move block row %1 col %2 -> row %3 col %4%5")
                        .arg(fromRow + 1).arg(fromColumn + 1).arg(toRow + 1).arg(toColumn + 1)
                        .arg(fromRow != toRow ? QStringLiteral(" (cross-row: the unit may create a parallel path)") : QString()));
    scheduleRefresh();
}

void Controller::setChainInput(int row, int port) {
    guarded("input", [&](qc::QuadCortex& d) { return d.set_chain_input(row, port); });
    scheduleRefresh();
}

void Controller::setChainOutput(int row, int port) {
    guarded("output", [&](qc::QuadCortex& d) { return d.set_chain_output(row, port); });
    scheduleRefresh();
}

void Controller::savePreset(const QString& folder, int position, const QString& name, int instrument) {
    guarded("save", [&](qc::QuadCortex& d) {
        return d.save_current_preset(folder.toStdString(), position, name.toStdString(), instrument);
    });
    emit logMessage(QStringLiteral("saved \"%1\" to %2 %3").arg(name, folder, qs(qc::view::position_to_slot(position))));
}

void Controller::setMasterVolume(float volume) {
    guarded("master volume", [&](qc::QuadCortex& d) { return d.set_master_volume(volume); });
}

void Controller::setTuner(bool show) {
    guarded("tuner", [&](qc::QuadCortex& d) { return d.set_tuner(show); });
}

void Controller::setIoMeter(bool on) {
    ioWanted_ = on;
    if (!on) {
        ioTimer_->stop();
        if (qc_ && qc_->connected()) (void)qc_->subscribe_io_meter(false);
        return;
    }
    if (!qc_ || !qc_->connected()) return;  // wird nach dem Verbinden bestellt
    subscribeIo();
    ioTimer_->start();
}

void Controller::subscribeIo() {
    if (!qc_ || !qc_->connected()) return;
    if (auto r = qc_->subscribe_io_meter(true); !r) {
        emit logMessage(QStringLiteral("iometer: %1").arg(qs(r.error())));
    }
}

void Controller::setSceneLabel(int scene, const QString& label) {
    guarded("scene label", [&](qc::QuadCortex& d) { return d.set_scene_label(scene, label.toStdString()); });
    scheduleRefresh();
}

void Controller::setSceneColor(int scene, quint32 argb) {
    guarded("scene colour", [&](qc::QuadCortex& d) { return d.set_scene_color(scene, argb); });
    scheduleRefresh();
}

void Controller::saveBackup(const QString& path) {
    if (!qc_ || !qc_->connected()) {
        emit backupFinished(false, QStringLiteral("not connected"));
        return;
    }
    emit busy(true);
    auto b = qc_->read_backup();  // Geraet baut das Backup (~90 s)
    bool ok = false;
    QString msg;
    if (!b) {
        msg = qs(b.error());
    } else if (auto w = qc::io::write_private_atomic(path.toStdString(), *b); !w) {
        msg = qs(w.error());
    } else {
        ok = true;
        msg = QStringLiteral("saved %1 bytes (mode 0600) to %2").arg(b->size()).arg(path);
    }
    emit busy(false);
    emit logMessage(QStringLiteral("backup-save: %1").arg(msg));
    emit backupFinished(ok, msg);
}

void Controller::restoreBackup(const QString& path) {
    if (!qc_ || !qc_->connected()) {
        emit backupFinished(false, QStringLiteral("not connected"));
        return;
    }
    // Datei im Worker-Thread lesen, nicht im GUI-Thread.
    std::ifstream f(path.toStdString(), std::ios::binary);
    if (!f) {
        emit backupFinished(false, QStringLiteral("cannot open %1").arg(path));
        return;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) {
        emit backupFinished(false, QStringLiteral("cannot read %1").arg(path));
        return;
    }
    emit busy(true);
    auto r = qc_->restore_backup(json);
    emit busy(false);
    const bool ok = r.has_value();
    const QString msg = ok ? QStringLiteral("device applied the backup (%1 bytes)").arg(json.size()) : qs(r.error());
    emit logMessage(QStringLiteral("backup-restore: %1").arg(msg));
    emit backupFinished(ok, msg);
    if (ok) scheduleRefresh();
}

void Controller::listFolders() {
    if (!qc_ || !qc_->connected()) return;
    emit busy(true);
    auto f = qc_->list_folders(0, 12000ms);
    emit busy(false);
    if (!f) {
        emit errorOccurred(qs(f.error()));
        return;
    }
    QVector<qc::Folder> out;
    for (auto& x : *f) out.push_back(std::move(x));
    emit foldersListed(out);
}

void Controller::listPresets(const QString& folder) {
    if (!qc_ || !qc_->connected()) return;
    emit busy(true);
    auto l = qc_->list_presets(folder.toStdString(), false, 20000ms);
    emit busy(false);
    if (!l) {
        emit errorOccurred(qs(l.error()));
        return;
    }
    QVector<qc::PresetEntry> out;
    for (auto& e : *l) out.push_back(std::move(e));
    emit presetsListed(folder, out);
}
