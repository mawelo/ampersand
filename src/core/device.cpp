// device.cpp — QuadCortex-API (siehe device.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "qc/device.hpp"

#include "qc/hid_transport.hpp"
#include "qc/log.hpp"
#include "qc/midi.hpp"

#include <cmath>
#include <algorithm>
#include <format>

namespace qc {

namespace pa = cortex_protobuf_v2;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kLaneOutputModel = 23000;
constexpr std::uint32_t kTempoModel = 25000;

/// Written values come back bit-exact, so a hair's breadth is enough to allow
/// for float representation without letting a real deviation pass.
constexpr float kParamTolerance = 1e-3f;

std::string range_error(std::string_view what, int value, int lo, int hi) {
    return std::format("{} {} out of range {}..{}", what, value, lo, hi);
}

/// Rows and columns are reported the way a person sees them (1-based), even
/// though the library addresses them from 0 — a message naming the internal
/// index sends the reader looking at the wrong cell.
std::string cell_error(std::string_view what, int user_value, int count) {
    return std::format("{} {} out of range: {}s are numbered 1 to {}, as on the unit", what, user_value, what, count);
}

}  // namespace

// ── lifecycle ───────────────────────────────────────────────────────────────

std::expected<std::unique_ptr<QuadCortex>, std::string> QuadCortex::open(Session::Options options) {
    auto t = hid::HidrawTransport::open_first();
    if (!t) return std::unexpected(t.error());
    return std::make_unique<QuadCortex>(std::move(*t), std::move(options));
}

QuadCortex::QuadCortex(std::unique_ptr<Transport> transport, Session::Options options)
    : session_(std::move(transport), std::move(options)) {
    session_.set_listener([this](const Inbound& in) { on_inbound(in); });
    session_.set_lost_handler([this](const std::string& reason) { emit_event(event::Lost{reason}); });
    session_.start();
}

QuadCortex::~QuadCortex() {
    session_.set_listener({});
    session_.set_lost_handler({});
    session_.stop();
}

std::expected<void, std::string> QuadCortex::connect() { return session_.connect(); }

void QuadCortex::disconnect() { session_.disconnect(); }

void QuadCortex::set_event_sink(EventSink sink) {
    std::lock_guard cl{callback_mutex_};  // wartet, bis ein laufender Aufruf fertig ist
    std::lock_guard lk{sink_mutex_};
    sink_ = std::move(sink);
}

void QuadCortex::emit_event(const Event& e) {
    std::lock_guard cl{callback_mutex_};
    EventSink sink;
    {
        std::lock_guard lk{sink_mutex_};
        sink = sink_;
    }
    if (sink) sink(e);
}

void QuadCortex::on_inbound(const Inbound& in) {
    if (in.decrypt_failed) {
        emit_event(event::Raw{in.type, in.payload.size()});
        return;
    }
    switch (in.type) {
        case MessageType::RecallPreset:
            if (auto m = in.parse<pa::RecallPresetMessage>(); m && m->has_preset()) {
                event::PresetRecalled ev{m->preset(), static_cast<int>(m->reason()), {}};
                if (m->has_request_id()) ev.request_id = m->request_id();
                emit_event(ev);
                return;
            }
            break;
        case MessageType::Grid:
            if (auto m = in.parse<pa::GridMessage>(); m && m->has_preset()) {
                emit_event(event::GridDelta{m->preset(), static_cast<int>(m->action())});
                return;
            }
            break;
        case MessageType::Scene:
            if (auto m = in.parse<pa::SceneMessage>(); m && m->has_selected_scene()) {
                emit_event(event::SceneSelected{static_cast<int>(m->selected_scene())});
                return;
            }
            break;
        case MessageType::SceneLabel:
            if (auto m = in.parse<pa::SceneLabelMessage>()) {
                emit_event(event::SceneLabel{m->index(), m->label()});
                return;
            }
            break;
        case MessageType::SceneColor:
            if (auto m = in.parse<pa::SceneColorMessage>()) {
                emit_event(event::SceneColor{m->index(), m->color()});
                return;
            }
            break;
        case MessageType::SetlistPosition:
            if (auto m = in.parse<pa::SetlistPositionMessage>(); m && m->has_folder_key()) {
                emit_event(event::SetlistPosition{m->folder_key(), static_cast<int>(m->position()), m->is_factory()});
                return;
            }
            break;
        case MessageType::Mode:
            if (auto m = in.parse<pa::ModeMessage>(); m && m->has_mode()) {
                emit_event(event::ModeChanged{static_cast<int>(m->mode())});
                return;
            }
            break;
        case MessageType::MasterVolume:
            if (auto m = in.parse<pa::MasterVolumeMessage>(); m && m->has_volume()) {
                emit_event(event::MasterVolume{m->volume()});
                return;
            }
            break;
        case MessageType::PresetDirty:
            if (auto m = in.parse<pa::PresetDirtyMessage>()) {
                emit_event(event::PresetDirty{m->is_dirty()});
                return;
            }
            break;
        case MessageType::File:
            if (auto m = in.parse<pa::FileMessage>(); m && m->has_folder()) {
                emit_event(event::FolderListing{m->folder(), m->has_type() ? m->type() : 0});
                return;
            }
            break;
        case MessageType::GlobalTempo:
            if (auto m = in.parse<pa::GlobalTempoMessage>()) {
                event::Tempo t;
                if (m->params_size() > 0 && m->params(0).param_values_size() > 0 &&
                    m->params(0).param_values(0).value_case() == ::ParamValue::kFloatValue) {
                    t.bpm = 40.0f + m->params(0).param_values(0).float_value() * 200.0f;
                }
                emit_event(t);
                return;
            }
            break;
        case MessageType::IOMeter:
            if (auto m = in.parse<pa::IOMeterMessage>()) {
                iometer::Levels stand;
                {
                    std::lock_guard lk{io_mutex_};
                    iometer::merge(io_levels_, *m);
                    stand = io_levels_;
                }
                emit_event(event::IoMeter{stand});
                return;
            }
            break;
        default:
            break;
    }
    emit_event(event::Raw{in.type, in.payload.size()});
}

// ── catalog ─────────────────────────────────────────────────────────────────

std::expected<const catalog::Catalog*, std::string> QuadCortex::catalog(ms timeout) {
    {
        std::lock_guard lk{catalog_mutex_};
        if (catalog_) return &*catalog_;
    }
    auto payload = session_.model_repo_payload();
    if (!payload) {
        auto in = session_.await(
            MessageType::ModelRepo, [&] { (void)session_.send_read(MessageType::ModelRepo); }, timeout,
            [](const Inbound& i) {
                auto m = i.parse<pa::ModelRepoMessage>();
                return m && m->has_model_repo_payload() && !m->model_repo_payload().empty();
            });
        if (!in) return std::unexpected("ModelRepo: " + in.error());
        payload = session_.model_repo_payload();
        if (!payload) {
            auto m = in->parse<pa::ModelRepoMessage>();
            const auto& p = m->model_repo_payload();
            payload = std::vector<std::uint8_t>(p.begin(), p.end());
        }
    }
    auto cat = catalog::parse_model_repo(*payload);
    if (!cat) return std::unexpected(cat.error());
    std::lock_guard lk{catalog_mutex_};
    catalog_ = std::move(*cat);
    return &*catalog_;
}

// ── read ────────────────────────────────────────────────────────────────────

std::expected<DeviceVersion, std::string> QuadCortex::read_version(ms timeout) {
    // The unit sends the full version blob exactly once: during the handshake.
    // A later READ is answered only in part, without app_fw_version, so a
    // waiter for it ran into the timeout — the same call succeeded or failed
    // depending on who won the race (seen on the unit 2026-09-10: `qcctl
    // version` fine, `qcctl coros` reporting "no Version reply"). Take what the
    // handshake already gave us.
    if (auto known = session_.device_version(); !known.app_fw_version.empty()) return known;
    auto in = session_.await(
        MessageType::Version, [&] { (void)session_.send_read(MessageType::Version); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::VersionMessage>();
            return m && m->has_app_fw_version();
        });
    if (!in) return std::unexpected(in.error());
    return session_.device_version();
}

std::expected<::BinaryPreset, std::string> QuadCortex::read_current_preset(ms timeout) {
    const auto rid = session_.next_request_id();
    pa::RecallPresetMessage req;
    req.set_action(pa::MessageAction::READ);
    req.set_request_id(rid);
    auto in = session_.await(
        MessageType::RecallPreset, [&] { (void)session_.send(MessageType::RecallPreset, req); }, timeout,
        [rid](const Inbound& i) {
            auto m = i.parse<pa::RecallPresetMessage>();
            return m && m->has_request_id() && m->request_id() == rid && m->has_preset();
        });
    if (!in) return std::unexpected("RecallPreset READ: " + in.error());
    return in->parse<pa::RecallPresetMessage>()->preset();
}

std::expected<::BinaryPreset, std::string> QuadCortex::read_preset(std::string_view folder_key, int position, ms timeout) {
    const auto rid = session_.next_request_id();
    auto in = session_.await(
        MessageType::RecallPreset, [&] { (void)recall_preset(folder_key, position, rid); }, timeout,
        [rid](const Inbound& i) {
            auto m = i.parse<pa::RecallPresetMessage>();
            return m && m->has_request_id() && m->request_id() == rid && m->has_preset();
        });
    if (!in) return std::unexpected("RecallPreset push: " + in.error());
    return in->parse<pa::RecallPresetMessage>()->preset();
}

std::expected<::cortex_protobuf_v2::PortSettings, std::string> QuadCortex::io_settings(ms timeout) {
    auto in = session_.await(
        MessageType::IOSettings, [&] { (void)session_.send_read(MessageType::IOSettings); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::IOSettingsMessage>();
            return m && m->has_settings();
        });
    if (!in) return std::unexpected(in.error());
    auto m = in->parse<pa::IOSettingsMessage>();
    if (!m || !m->has_settings()) return std::unexpected("IOSettings reply carried no settings");
    return m->settings();
}

std::expected<::cortex_protobuf_v2::GeneralSettingsMessage, std::string> QuadCortex::general_settings(ms timeout) {
    auto in = session_.await(
        MessageType::GeneralSettings, [&] { (void)session_.send_read(MessageType::GeneralSettings); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::GeneralSettingsMessage>();
            // Auf die Antwort warten, nicht auf das Echo der eigenen Anfrage.
            return m && (m->has_screen_brightness() || m->has_midi_channel() || m->has_total_disk_space());
        });
    if (!in) return std::unexpected(in.error());
    auto m = in->parse<pa::GeneralSettingsMessage>();
    if (!m) return std::unexpected("GeneralSettings reply could not be parsed");
    return *m;
}

std::expected<int, std::string> QuadCortex::active_scene(ms timeout) {
    pa::SceneMessage req;
    req.set_action(pa::MessageAction::READ);
    auto reply = session_.request(MessageType::Scene, req, timeout);
    if (!reply) return std::unexpected(reply.error());
    if (!reply->has_selected_scene()) return std::unexpected("Scene reply carries no selected_scene");
    return static_cast<int>(reply->selected_scene());
}

std::expected<event::SetlistPosition, std::string> QuadCortex::current_position(ms timeout) {
    auto in = session_.await(
        MessageType::SetlistPosition, [&] { (void)session_.send_read(MessageType::SetlistPosition); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::SetlistPositionMessage>();
            return m && m->has_folder_key();
        });
    if (!in) return std::unexpected(in.error());
    auto m = in->parse<pa::SetlistPositionMessage>();
    return event::SetlistPosition{m->folder_key(), static_cast<int>(m->position()), m->is_factory()};
}

std::expected<bool, std::string> QuadCortex::preset_dirty(ms timeout) {
    pa::PresetDirtyMessage req;
    req.set_action(pa::MessageAction::READ);
    auto reply = session_.request(MessageType::PresetDirty, req, timeout);
    if (!reply) return std::unexpected(reply.error());
    return reply->is_dirty();
}

std::expected<float, std::string> QuadCortex::master_volume(ms timeout) {
    auto in = session_.await(
        MessageType::MasterVolume, [&] { (void)session_.send_read(MessageType::MasterVolume); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::MasterVolumeMessage>();
            return m && m->has_volume();
        });
    if (!in) return std::unexpected(in.error());
    return in->parse<pa::MasterVolumeMessage>()->volume();
}

std::expected<int, std::string> QuadCortex::mode(ms timeout) {
    auto in = session_.await(
        MessageType::Mode, [&] { (void)session_.send_read(MessageType::Mode); }, timeout,
        [](const Inbound& i) {
            auto m = i.parse<pa::ModeMessage>();
            return m && m->has_mode();
        });
    if (!in) return std::unexpected(in.error());
    return static_cast<int>(in->parse<pa::ModeMessage>()->mode());
}

std::expected<std::vector<Folder>, std::string> QuadCortex::list_folders(int type, ms window) {
    pa::FileMessage req;
    req.set_action(pa::MessageAction::READ);
    if (type != 0) req.set_type(type);
    auto pushes = session_.collect(
        MessageType::File, [&] { (void)session_.send(MessageType::File, req); }, window,
        [](const Inbound& i) {
            auto m = i.parse<pa::FileMessage>();
            return m && m->has_folder();
        });
    std::vector<Folder> out;
    for (const auto& in : pushes) {
        auto m = in.parse<pa::FileMessage>();
        if (!m) continue;
        const auto& f = m->folder();
        Folder folder{f.key(), f.name(), f.parent_key(), f.is_factory(), f.is_user_default(),
                      m->has_type() ? m->type() : 0, f.files_size()};
        auto dup = std::find_if(out.begin(), out.end(), [&](const Folder& x) { return x.key == folder.key; });
        if (dup == out.end()) out.push_back(std::move(folder));
        else if (folder.file_count > dup->file_count) *dup = std::move(folder);
    }
    if (out.empty()) return std::unexpected(std::format("no folder listing arrived within {} ms (ask again: delivery is lazy)", window.count()));
    return out;
}

std::expected<std::vector<PresetEntry>, std::string> QuadCortex::list_presets(std::string_view folder_key,
                                                                              bool include_empty, ms timeout) {
    pa::FileMessage req;
    req.set_action(pa::MessageAction::READ);
    auto in = session_.await(
        MessageType::File, [&] { (void)session_.send(MessageType::File, req); }, timeout,
        [folder_key](const Inbound& i) {
            auto m = i.parse<pa::FileMessage>();
            return m && m->has_folder() && view::same_folder(m->folder().key(), folder_key) && m->folder().files_size() > 0;
        });
    if (!in) return std::unexpected(std::format("no listing for '{}': {}", folder_key, in.error()));
    auto m = in->parse<pa::FileMessage>();
    std::vector<PresetEntry> out;
    for (const auto& f : m->folder().files()) {
        if (!include_empty && (!f.has_name() || f.name().empty())) continue;
        PresetEntry e;
        e.index = f.has_index() ? f.index() : -1;
        e.slot = view::position_to_slot(e.index);
        e.name = f.name();
        e.author = f.author();
        e.instrument = f.has_instrument() ? f.instrument() : 0;
        e.key = f.key();
        e.date = f.date();
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const PresetEntry& a, const PresetEntry& b) { return a.index < b.index; });
    return out;
}

std::expected<PresetEntry, std::string> QuadCortex::find_preset(std::string_view name, std::string_view folder_key,
                                                                ms timeout) {
    auto list = list_presets(folder_key, false, timeout);
    if (!list) return std::unexpected(list.error());
    for (const auto& e : *list) {
        if (e.name == name) return e;
    }
    return std::unexpected(std::format("no preset named '{}' in '{}'", name, folder_key));
}

// ── navigation ──────────────────────────────────────────────────────────────

std::expected<void, std::string> QuadCortex::recall_preset(std::string_view folder_key, int position,
                                                           std::optional<std::uint64_t> request_id) {
    if (position < 0 || position >= view::kSlotsPerSetlist) return std::unexpected(range_error("position", position, 0, 255));
    pa::SetlistPositionMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_folder_key(std::string(folder_key));
    msg.set_position(static_cast<std::uint32_t>(position));
    msg.set_is_factory(view::is_factory_setlist(folder_key));
    if (request_id) msg.set_request_id(*request_id);
    return session_.send(MessageType::SetlistPosition, msg);
}

std::expected<void, std::string> QuadCortex::switch_scene(int scene) {
    if (scene < 0 || scene >= view::kScenes) return std::unexpected(range_error("scene", scene, 0, 7));
    pa::SceneMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_selected_scene(static_cast<std::uint32_t>(scene));
    return session_.send(MessageType::Scene, msg);
}

std::expected<void, std::string> QuadCortex::set_tuner(bool show) {
    // Nicht ueber das Protobuf-Protokoll: ShowTuner (27) wird vom Geraet in
    // jeder Form ignoriert - kein Echo, keine Wirkung (docs/evidence/82). Der
    // Weg fuehrt ueber MIDI CC#45, am Geraet in beide Richtungen belegt
    // (docs/evidence/84). Der Port wird je Aufruf geoeffnet; fuer drei Bytes
    // lohnt kein dauerhafter Zustand.
    auto port = midi::Port::open_quad_cortex();
    if (!port) return std::unexpected(std::format("tuner needs the unit's MIDI port: {}", port.error()));
    // Kanal nicht raten: Wer am Geraet einen anderen einstellt, haette sonst
    // einen Knopf, der wortlos nichts tut. Der Wert stammt aus den
    // GeneralSettings des Handshakes und kostet deshalb keine Extra-Abfrage.
    const int channel = session_.midi_channel().value_or(0);
    return port->tuner(show, channel);
}

std::expected<void, std::string> QuadCortex::subscribe_io_meter(bool on) {
    // Gemessen (Beleg 144: Mitschnitt von Cortex Control; Beleg 182: Antworten
    // am Geraet): CREATE mit request_id startet, DELETE beendet. CREATE ist der
    // Nullwert des Enums und steht deshalb gar nicht auf dem Draht. Ein READ
    // bleibt unbeantwortet. ProductionAutomationMode NICHT senden - laut
    // CortexUSB trennt es die Verbindung und ist ohne Belang.
    pa::IOMeterMessage msg;
    msg.set_action(on ? pa::MessageAction::CREATE : pa::MessageAction::DELETE);
    msg.set_request_id(session_.next_request_id());
    return session_.send(MessageType::IOMeter, msg);
}

iometer::Levels QuadCortex::io_levels() const {
    std::lock_guard lk{io_mutex_};
    return io_levels_;
}

std::expected<void, std::string> QuadCortex::set_scene_label(int scene, std::string_view label) {
    if (scene < 0 || scene >= view::kScenes) return std::unexpected(range_error("scene", scene, 0, 7));
    pa::SceneLabelMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_index(scene);
    msg.set_label(label.empty() ? std::string(" ") : std::string(label));  // an unlabelled scene is one space
    return session_.send(MessageType::SceneLabel, msg);
}

std::expected<void, std::string> QuadCortex::set_scene_color(int scene, std::uint32_t argb) {
    if (scene < 0 || scene >= view::kScenes) return std::unexpected(range_error("scene", scene, 0, 7));
    pa::SceneColorMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_index(scene);
    msg.set_color(argb);
    return session_.send(MessageType::SceneColor, msg);
}

std::expected<void, std::string> QuadCortex::copy_scene(int from, int to, bool swap) {
    if (from < 0 || from >= view::kScenes) return std::unexpected(range_error("from", from, 0, 7));
    if (to < 0 || to >= view::kScenes) return std::unexpected(range_error("to", to, 0, 7));
    pa::SceneCopyMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);  // confirmed: UPDATE, not COPY (device's own broadcast)
    msg.set_from_index(from);
    msg.set_to_index(to);
    msg.set_is_swap(swap);
    return session_.send(MessageType::SceneCopy, msg);
}

std::expected<void, std::string> QuadCortex::set_mode(int m) {
    pa::ModeMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_mode(static_cast<std::uint32_t>(m));
    return session_.send(MessageType::Mode, msg);
}

// ── grid edits ──────────────────────────────────────────────────────────────

std::expected<void, std::string> QuadCortex::check_cell(int row, int column) const {
    if (row < 0 || row >= view::kRows) return std::unexpected(cell_error("row", view::row_to_user(row), view::kRows));
    if (column < 0 || column >= view::kColumns) return std::unexpected(cell_error("column", view::column_to_user(column), view::kColumns));
    return {};
}

std::expected<void, std::string> QuadCortex::send_grid(const ::BinaryPreset& sparse, Action action) {
    pa::GridMessage msg;
    msg.set_action(static_cast<pa::MessageAction::Enum>(action));
    *msg.mutable_preset() = sparse;
    return session_.send(MessageType::Grid, msg);
}

std::expected<void, std::string> QuadCortex::set_param(int row, int column, int param_index, float value,
                                                       std::optional<int> scene, bool verify, ms timeout) {
    if (auto c = check_cell(row, column); !c) return c;
    if (scene) {
        // Per-scene value: promote (flag ALONE), sit on the scene, then write.
        if (auto r = set_param_scene_mode(row, column, param_index, true); !r) return r;
        if (auto r = switch_scene(*scene); !r) return r;
    }
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* model = chain->add_models();
    model->set_column(static_cast<std::uint32_t>(column));
    auto* prm = model->add_params();
    prm->set_index(static_cast<std::uint32_t>(param_index));
    prm->add_param_values()->set_float_value(value);
    if (auto sent = send_grid(p); !sent) return sent;
    if (!verify) return {};

    // "ok" used to mean "sent", not "arrived": a write to an empty cell vanishes
    // without a word from the unit. So ask the cell itself. Values come back
    // bit-exact (measured 2026-09-10: 0.37 -> 0.3700000047683716), which is why
    // the comparison may be this strict.
    auto preset = read_current_preset(timeout);
    if (!preset) {
        return std::unexpected(std::format("parameter {} at row {} column {} was sent, but reading it back failed ({})",
                                           param_index, view::row_to_user(row), view::column_to_user(column),
                                           preset.error()));
    }
    auto stored = [&](int look_at) -> std::optional<view::ParamView> {
        const auto v = view::build(*preset, nullptr, look_at);
        const auto* b = v.rows[static_cast<std::size_t>(row)].block_at(column);
        if (!b) return std::nullopt;
        for (const auto& pv : b->params) {
            if (pv.index == param_index) return pv;
        }
        return std::nullopt;
    };
    auto got = stored(scene.value_or(0));
    // A per-scene value written without naming a scene landed in the active one.
    if (got && got->scene_mode && !scene) {
        if (auto active = active_scene(); active) got = stored(*active);
    }
    if (!got) {
        const auto v = view::build(*preset, nullptr, scene.value_or(0));
        const auto* b = v.rows[static_cast<std::size_t>(row)].block_at(column);
        return std::unexpected(std::format("parameter {} did not arrive at row {} column {}: the cell is {}",
                                           param_index, view::row_to_user(row), view::column_to_user(column),
                                           b ? std::format("{} without that parameter", b->name) : "empty"));
    }
    if (!got->value || std::fabs(*got->value - value) > kParamTolerance) {
        return std::unexpected(std::format("parameter {} at row {} column {} reads {} after writing {:.4f}",
                                           param_index, view::row_to_user(row), view::column_to_user(column),
                                           got->value ? std::format("{:.4f}", *got->value) : "nothing", value));
    }
    return {};
}

std::expected<void, std::string> QuadCortex::set_param_text(int row, int column, int param_index, std::string_view text) {
    if (auto c = check_cell(row, column); !c) return c;
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* model = chain->add_models();
    model->set_column(static_cast<std::uint32_t>(column));
    auto* prm = model->add_params();
    prm->set_index(static_cast<std::uint32_t>(param_index));
    prm->add_param_values()->set_string_value(std::string(text));
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_param_scene_mode(int row, int column, int param_index, bool enabled) {
    if (auto c = check_cell(row, column); !c) return c;
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* model = chain->add_models();
    model->set_column(static_cast<std::uint32_t>(column));
    auto* prm = model->add_params();
    prm->set_index(static_cast<std::uint32_t>(param_index));
    prm->set_scene_mode(enabled);  // must travel ALONE
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_bypass(int row, int column, bool bypassed, std::optional<int> scene) {
    if (auto c = check_cell(row, column); !c) return c;
    if (scene) {
        if (auto r = switch_scene(*scene); !r) return r;
    }
    ::BinaryPreset p;  // no `chains` sibling: an empty one makes the device drop the write
    auto* bp = p.add_bypass();
    bp->set_row(static_cast<std::uint32_t>(row));
    auto* cb = bp->add_colbypass();
    cb->set_column(static_cast<std::uint32_t>(column));
    cb->add_scenebypass()->set_bypass(bypassed);  // index 0 = the ACTIVE scene
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_block(int row, int column, std::uint32_t model_id, bool verify,
                                                       ms timeout) {
    if (auto c = check_cell(row, column); !c) return c;
    if (model_id == 0) return std::unexpected("model id 0 means 'empty'; use remove_block");
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* model = chain->add_models();
    model->set_column(static_cast<std::uint32_t>(column));
    model->set_hash(model_id);
    if (!verify) return send_grid(p);

    auto echo = session_.await(
        MessageType::Grid, [&] { (void)send_grid(p); }, timeout,
        [row, column, model_id](const Inbound& i) {
            auto m = i.parse<pa::GridMessage>();
            if (!m || !m->has_preset()) return false;
            const auto& pr = m->preset();
            for (int ci = 0; ci < pr.chains_size(); ++ci) {
                const auto& ch = pr.chains(ci);
                const int r = ch.has_row() ? static_cast<int>(ch.row()) : ci;
                if (r != row) continue;
                for (int mi = 0; mi < ch.models_size(); ++mi) {
                    const auto& md = ch.models(mi);
                    const int col = md.has_column() ? static_cast<int>(md.column()) : mi;
                    if (col == column && md.has_hash() && md.hash() == model_id) return true;
                }
            }
            return false;
        });
    if (!echo) {
        // No echo is not the same as no block: read the preset back and let the
        // grid itself decide. Reported the other way round, a placed block looked
        // like a refusal — and a caller that believed it removed the wrong cell.
        // Same budget as the echo it stands in for. Five seconds looked tidier
        // but was too short: in a fresh session the unit is still working
        // through its seed push, and the preset reply only follows that
        // (evidence/78) — the check would fail while the block sat fine.
        auto preset = read_current_preset(timeout);
        if (!preset) {
            return std::unexpected(std::format(
                "model {} at row {} column {}: no Grid echo within {} ms, and reading the preset back failed too ({})",
                model_id, view::row_to_user(row), view::column_to_user(column), timeout.count(), preset.error()));
        }
        const auto placed = view::build(*preset, nullptr, 0);
        const auto* block = placed.rows[static_cast<std::size_t>(row)].block_at(column);
        if (block && block->model_id == model_id) return {};
        return std::unexpected(std::format(
            "the device did not accept model {} at row {} column {} (no Grid echo within {} ms, and the cell is {}). "
            "Usual cause: no DSP capacity left in this preset",
            model_id, view::row_to_user(row), view::column_to_user(column), timeout.count(),
            block ? std::format("holding model {}", block->model_id) : "empty"));
    }
    return {};
}

std::expected<void, std::string> QuadCortex::remove_block(int row, int column) {
    if (auto c = check_cell(row, column); !c) return c;
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* model = chain->add_models();
    model->set_column(static_cast<std::uint32_t>(column));
    model->set_hash(0);
    return send_grid(p, Action::Delete);  // the ACTION marks the removal
}

std::expected<void, std::string> QuadCortex::move_block(int from_row, int from_col, int to_row, int to_col) {
    if (auto c = check_cell(from_row, from_col); !c) return c;
    if (auto c = check_cell(to_row, to_col); !c) return c;
    pa::GridMoveMessage msg;
    auto* mv = msg.add_move();
    mv->set_from_row(static_cast<std::uint32_t>(from_row));
    mv->set_from_col(static_cast<std::uint32_t>(from_col));
    mv->set_to_row(static_cast<std::uint32_t>(to_row));
    mv->set_to_col(static_cast<std::uint32_t>(to_col));
    mv->set_is_drop(true);
    return session_.send(MessageType::GridMove, msg);
}

std::expected<void, std::string> QuadCortex::set_chain_input(int row, int in_port) {
    if (row < 0 || row >= view::kRows) return std::unexpected(cell_error("row", view::row_to_user(row), view::kRows));
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    chain->set_in_portid(static_cast<std::uint32_t>(in_port));
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_chain_output(int row, int out_port) {
    if (row < 0 || row >= view::kRows) return std::unexpected(cell_error("row", view::row_to_user(row), view::kRows));
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    chain->set_out_portid(static_cast<std::uint32_t>(out_port));
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_split(int row, int split_column, int mix_column) {
    if (row != 0 && row != 2) return std::unexpected("a branch can only originate on row 0 or 2");
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* scp = chain->add_split_control_points();
    scp->set_split(split_column);
    scp->set_mix(mix_column);
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_lane_output(int row, int param_index, float value) {
    if (row < 0 || row >= view::kRows) return std::unexpected(cell_error("row", view::row_to_user(row), view::kRows));
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_row(static_cast<std::uint32_t>(row));
    auto* oc = chain->add_output_control();
    oc->set_hash(kLaneOutputModel);
    auto* prm = oc->add_params();
    prm->set_index(static_cast<std::uint32_t>(param_index));
    prm->add_param_values()->set_float_value(value);
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::set_tempo_bpm(float bpm) {
    if (bpm < 40.0f || bpm > 240.0f) return std::unexpected("tempo must be 40..240 bpm");
    ::BinaryPreset p;
    auto* tempo = p.add_tempoprogramdata();
    tempo->set_hash(kTempoModel);
    auto* prm = tempo->add_params();
    prm->set_index(0);  // TEMPO
    prm->add_param_values()->set_float_value((bpm - 40.0f) / 200.0f);
    return send_grid(p);
}

std::expected<void, std::string> QuadCortex::sync_time(std::uint64_t ms_since_epoch) {
    pa::SystemTimeSyncMessage msg;
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_ms_since_epoch(ms_since_epoch);
    return session_.send(MessageType::SystemTimeSync, msg);
}

// ── files ───────────────────────────────────────────────────────────────────

std::expected<void, std::string> QuadCortex::save_current_preset(std::string_view folder_key, int position,
                                                                 std::string_view name, int instrument) {
    if (position < 0 || position >= view::kSlotsPerSetlist) return std::unexpected(range_error("position", position, 0, 255));
    if (name.empty()) return std::unexpected("preset name must not be empty");
    if (view::is_factory_setlist(folder_key)) return std::unexpected("refusing to save into a factory folder");
    pa::FileMessage msg;  // action CREATE = 0 = proto3 default (omitted on the wire)
    msg.set_type(0);
    auto* folder = msg.mutable_folder();
    folder->set_key(std::string(folder_key));
    folder->set_is_factory(false);
    auto* file = folder->add_files();
    file->set_index(position);
    file->set_name(std::string(name));
    file->set_instrument(instrument);
    return session_.send(MessageType::File, msg);
}

std::expected<void, std::string> QuadCortex::delete_preset(std::string_view folder_key, std::string_view name) {
    if (view::is_factory_setlist(folder_key)) return std::unexpected("refusing to delete from a factory folder");
    pa::FileMessage msg;
    msg.set_action(pa::MessageAction::DELETE);
    msg.set_type(0);
    auto* folder = msg.mutable_folder();
    folder->set_key(std::string(folder_key));
    folder->set_is_factory(false);
    folder->add_files()->set_key(std::format("{}/{}.pb", folder_key, name));
    return session_.send(MessageType::File, msg);
}

std::expected<void, std::string> QuadCortex::move_preset(std::string_view folder_key, std::string_view name,
                                                         int to_position) {
    if (to_position < 0 || to_position >= view::kSlotsPerSetlist) return std::unexpected(range_error("position", to_position, 0, 255));
    pa::FileMessage msg;
    msg.set_action(pa::MessageAction::MOVE);
    msg.set_type(0);
    auto* folder = msg.mutable_folder();
    folder->set_key(std::string(folder_key));
    folder->add_files()->set_key(std::format("{}/{}.pb", folder_key, name));
    auto* to = msg.mutable_to_folder();
    to->set_key(std::string(folder_key));
    to->add_files()->set_index(to_position);
    return session_.send(MessageType::File, msg);
}

// ── global ──────────────────────────────────────────────────────────────────

std::expected<void, std::string> QuadCortex::set_master_volume(float volume) {
    if (volume < 0.0f || volume > 1.0f) return std::unexpected("master volume is normalized 0..1");
    pa::MasterVolumeMessage msg;  // never set `calibrate`: it opens the calibration dialog
    msg.set_action(pa::MessageAction::UPDATE);
    msg.set_volume(volume);
    return session_.send(MessageType::MasterVolume, msg);
}

std::expected<std::string, std::string> QuadCortex::read_backup(ms window) {
    pa::LocalBackupMessage req;
    // CREATE, nicht READ. READ laesst das Geraet stumm (evidence/37, damals als
    // "Backup ueber USB nicht erreichbar" gedeutet) - CREATE heisst "leg ein
    // Backup an": das Geraet zeigt "Local Backup operation in progress", zaehlt
    // seine Ordner als File-Pushes auf und schickt danach das fertige
    // backup_json in Stuecken (evidence/100). Dasselbe Muster wie bei den
    // Messwerten: die Bestellung ist CREATE, nicht READ.
    req.set_action(pa::MessageAction::CREATE);
    auto pushes = session_.collect(
        MessageType::LocalBackup, [&] { (void)session_.send(MessageType::LocalBackup, req); }, window,
        [](const Inbound& i) {
            auto m = i.parse<pa::LocalBackupMessage>();
            return m && m->has_backup_json();
        });
    if (pushes.empty()) return std::unexpected("no LocalBackup data arrived (nothing to back up, or window too short)");
    std::string out;
    for (const auto& in : pushes) {
        auto m = in.parse<pa::LocalBackupMessage>();
        if (!m || !m->has_backup_json()) continue;
        out += m->backup_json();
        if (m->has_is_last_chunk() && m->is_last_chunk()) break;
    }
    if (out.empty()) return std::unexpected("LocalBackup contained no backup_json");
    return out;
}

std::expected<void, std::string> QuadCortex::restore_backup(std::string_view backup_json, std::size_t chunk, ms timeout) {
    if (backup_json.empty()) return std::unexpected("empty backup");
    if (chunk == 0) chunk = backup_json.size();
    for (std::size_t off = 0; off < backup_json.size();) {
        const std::size_t n = std::min(chunk, backup_json.size() - off);
        const bool last = (off + n >= backup_json.size());
        pa::LocalBackupMessage m;
        m.set_action(pa::MessageAction::UPDATE);
        m.set_backup_json(std::string(backup_json.substr(off, n)));
        m.set_is_last_chunk(last);
        if (last) {
            // Send the final chunk and wait for the device's applied_backup ack.
            auto in = session_.await(
                MessageType::LocalBackup, [&] { (void)session_.send(MessageType::LocalBackup, m); }, timeout,
                [](const Inbound& i) {
                    auto r = i.parse<pa::LocalBackupMessage>();
                    return r && r->has_applied_backup();
                });
            if (!in) return std::unexpected("no applied_backup ack: " + in.error());
            auto r = in->parse<pa::LocalBackupMessage>();
            if (!r || !r->applied_backup()) return std::unexpected("device reported the backup was NOT applied");
            return {};
        }
        if (auto s = session_.send(MessageType::LocalBackup, m); !s) return std::unexpected(s.error());
        off += n;
    }
    return {};
}

std::expected<Inbound, std::string> QuadCortex::await_raw(MessageType type, ms timeout) {
    return session_.await(type, [] {}, timeout);
}

}  // namespace qc
