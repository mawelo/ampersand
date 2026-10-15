// device.hpp — QuadCortex: die fachliche API ueber der Session
//
// Wer:      Martin.Lonkwitz
// Was:      Presets lesen/recallen/speichern, Szenen, Grid-Edits (Parameter,
//           Bypass, Bloecke, Routing, Split), Master-Volume, Tempo, Katalog,
//           Ordner-/Preset-Listen; Ereignis-Strom fuer GUI/MCP.
// Wann:     2026-09-04
// Warum:    CLI, MCP-Server und GUI sollen dieselbe, geprueft korrekte
//           Nachrichtenform nutzen (SSOT fuer die Wire-Shapes).
// Projekt:  Ampersand
// Abhaengig: session, catalog, preset_view, Protobuf-Klassen
// Nutzung:  auto qc = qc::QuadCortex::open(); qc->connect(); qc->switch_scene(1);
// Referenz: Jede Wire-Shape hier ist in docs/protocol.md mit ihrem Beleg
//           (Capture/Hardware-Readback der Referenzprojekte) aufgefuehrt.
// Historie: git log
#pragma once

#include "qc/catalog.hpp"
#include "qc/io_meter.hpp"
#include "qc/preset_view.hpp"
#include "qc/session.hpp"

#include <Preset.pb.h>
#include <ProductionAutomation.pb.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qc {

// ── events pushed by the device (delivered on the RX thread) ────────────────
namespace event {
struct PresetRecalled { ::BinaryPreset preset; int reason{0}; std::optional<std::uint64_t> request_id; };
struct GridDelta { ::BinaryPreset sparse; int action{1}; };
struct SceneSelected { int scene{0}; };
struct SceneLabel { int index{0}; std::string label; };
struct SceneColor { int index{0}; std::uint32_t color{0}; };
struct SetlistPosition { std::string folder_key; int position{0}; bool is_factory{false}; };
struct ModeChanged { int mode{0}; };
struct MasterVolume { float volume{1.0f}; };
struct PresetDirty { bool dirty{false}; };
struct FolderListing { cortex_protobuf_v2::FolderInfo folder; int type{0}; };
struct Tempo { std::optional<float> bpm; };
/// Pegel, die das Geraet selbst meldet - nur solange Signal anliegt (Beleg 182).
struct IoMeter { iometer::Levels levels; };
struct Raw { MessageType type; std::size_t bytes{0}; };
struct Lost { std::string reason; };
}  // namespace event

using Event = std::variant<event::PresetRecalled, event::GridDelta, event::SceneSelected, event::SceneLabel,
                           event::SceneColor, event::SetlistPosition, event::ModeChanged, event::MasterVolume,
                           event::PresetDirty, event::FolderListing, event::Tempo, event::IoMeter, event::Raw,
                           event::Lost>;

struct PresetEntry {
    int index{0};
    std::string slot;   ///< "28C"
    std::string name;
    std::string author;
    int instrument{0};
    std::string key;    ///< device file path
    std::string date;
};

struct Folder {
    std::string key;
    std::string name;
    std::string parent_key;
    bool is_factory{false};
    bool is_user_default{false};
    int type{0};
    int file_count{0};
};

class QuadCortex {
public:
    using ms = std::chrono::milliseconds;
    using EventSink = std::function<void(const Event&)>;

    /// Open the first Quad Cortex on USB (no handshake yet).
    [[nodiscard]] static std::expected<std::unique_ptr<QuadCortex>, std::string> open(Session::Options options = {});
    /// Wrap an arbitrary transport (tests).
    explicit QuadCortex(std::unique_ptr<Transport> transport, Session::Options options = {});
    ~QuadCortex();

    [[nodiscard]] std::expected<void, std::string> connect();
    void disconnect();
    [[nodiscard]] bool connected() const noexcept { return session_.connected(); }
    [[nodiscard]] bool lost() const noexcept { return session_.lost(); }
    [[nodiscard]] Session& session() noexcept { return session_; }
    [[nodiscard]] DeviceVersion version() const { return session_.device_version(); }
    void set_event_sink(EventSink sink);

    // ── catalog ─────────────────────────────────────────────────────────
    /// Parsed ModelRepo (fetched during the handshake or on demand).
    [[nodiscard]] std::expected<const catalog::Catalog*, std::string> catalog(ms timeout = ms{25000});

    // ── read ────────────────────────────────────────────────────────────
    /// Version blob without the handshake (works on a freshly opened device).
    [[nodiscard]] std::expected<DeviceVersion, std::string> read_version(ms timeout = ms{5000});
    /// The LIVE grid, unsaved edits included; no side effects.
    [[nodiscard]] std::expected<::BinaryPreset, std::string> read_current_preset(ms timeout = ms{15000});
    /// Recall a stored slot and return the pushed preset (loads it, cuts audio).
    [[nodiscard]] std::expected<::BinaryPreset, std::string> read_preset(std::string_view folder_key, int position,
                                                                          ms timeout = ms{40000});
    /// Globale I/O-Einstellungen (Eingangspegel, Impedanz, USB-Ausgabe). Diese
    /// gelten praesetuebergreifend und erklaeren manches, was am Grid nicht zu
    /// sehen ist - etwa ein leises Eingangssignal.
    [[nodiscard]] std::expected<::cortex_protobuf_v2::PortSettings, std::string> io_settings(ms timeout = ms{15000});

    /// Globale Geraeteeinstellungen: Helligkeit, MIDI, Power, Speicherplatz,
    /// Cloud-Endpunkt. Gelten presetuebergreifend.
    [[nodiscard]] std::expected<::cortex_protobuf_v2::GeneralSettingsMessage, std::string>
    general_settings(ms timeout = ms{15000});

    [[nodiscard]] std::expected<int, std::string> active_scene(ms timeout = ms{10000});
    [[nodiscard]] std::expected<event::SetlistPosition, std::string> current_position(ms timeout = ms{10000});
    [[nodiscard]] std::expected<bool, std::string> preset_dirty(ms timeout = ms{5000});
    [[nodiscard]] std::expected<float, std::string> master_volume(ms timeout = ms{10000});
    [[nodiscard]] std::expected<int, std::string> mode(ms timeout = ms{10000});

    /// Every folder the device enumerates for File type 0/1/2 (presets/IRs/captures).
    [[nodiscard]] std::expected<std::vector<Folder>, std::string> list_folders(int type = 0, ms window = ms{15000});
    /// Occupied slots of one folder (any key the device reports).
    [[nodiscard]] std::expected<std::vector<PresetEntry>, std::string> list_presets(std::string_view folder_key,
                                                                                    bool include_empty = false,
                                                                                    ms timeout = ms{25000});
    [[nodiscard]] std::expected<PresetEntry, std::string> find_preset(std::string_view name, std::string_view folder_key,
                                                                      ms timeout = ms{25000});

    // ── navigation ──────────────────────────────────────────────────────
    [[nodiscard]] std::expected<void, std::string> recall_preset(std::string_view folder_key, int position,
                                                                 std::optional<std::uint64_t> request_id = {});
    [[nodiscard]] std::expected<void, std::string> switch_scene(int scene);
    /// Tuner-Modus ein/aus (ShowTuner). Das Geraet sendet dann TunerMessage mit Frequenz/Meter.
    [[nodiscard]] std::expected<void, std::string> set_tuner(bool show);
    /// Pegelmeldungen des Geraets bestellen (on) oder abbestellen (off):
    /// IOMeter mit action=CREATE bzw. DELETE und frischer request_id - dieselbe
    /// Form wie Cortex Control. Das Geraet sendet danach event::IoMeter, aber
    /// NUR solange am Eingang Signal anliegt; endet es, kommt eine letzte
    /// Meldung mit Pegel 0 (Beleg 182). Zum Auffrischen erneut mit on=true
    /// aufrufen, siehe iometer::kResubscribeInterval.
    [[nodiscard]] std::expected<void, std::string> subscribe_io_meter(bool on);
    /// Der zuletzt gemeldete Pegelstand (alle Felder zusammengefuehrt).
    [[nodiscard]] iometer::Levels io_levels() const;
    [[nodiscard]] std::expected<void, std::string> set_scene_label(int scene, std::string_view label);
    [[nodiscard]] std::expected<void, std::string> set_scene_color(int scene, std::uint32_t argb);
    [[nodiscard]] std::expected<void, std::string> copy_scene(int from, int to, bool swap = false);
    [[nodiscard]] std::expected<void, std::string> set_mode(int mode);

    // ── grid edits (row/column keyed, zero-based; save afterwards) ───────
    /// Set a block parameter. Unless verify=false, read the cell back afterwards
    /// and confirm the value arrived — "ok" used to mean "sent", and a write to
    /// an empty cell vanishes without a word. Values come back bit-exact
    /// (measured), so the check is strict. Interactive callers that write on
    /// every knob movement should pass verify=false.
    [[nodiscard]] std::expected<void, std::string> set_param(int row, int column, int param_index, float value,
                                                             std::optional<int> scene = {}, bool verify = true,
                                                             ms timeout = ms{15000});
    [[nodiscard]] std::expected<void, std::string> set_param_text(int row, int column, int param_index,
                                                                  std::string_view text);
    [[nodiscard]] std::expected<void, std::string> set_param_scene_mode(int row, int column, int param_index,
                                                                        bool enabled);
    [[nodiscard]] std::expected<void, std::string> set_bypass(int row, int column, bool bypassed,
                                                              std::optional<int> scene = {});
    /// Place a block. Unless verify=false, confirm it afterwards: first by the
    /// unit's Grid echo, and if that stays away, by reading the preset back.
    ///
    /// The unit rebuilds its DSP chain before echoing, which took 10,4 s in the
    /// measured run and leaves a silence of over 5 s in the middle — hence the
    /// generous budget. And the echo can fail to arrive while the block sits
    /// perfectly well, so a missing echo alone must not be reported as refusal.
    [[nodiscard]] std::expected<void, std::string> set_block(int row, int column, std::uint32_t model_id,
                                                             bool verify = true, ms timeout = ms{15000});
    [[nodiscard]] std::expected<void, std::string> remove_block(int row, int column);
    [[nodiscard]] std::expected<void, std::string> move_block(int from_row, int from_col, int to_row, int to_col);
    [[nodiscard]] std::expected<void, std::string> set_chain_input(int row, int in_port);
    [[nodiscard]] std::expected<void, std::string> set_chain_output(int row, int out_port);
    [[nodiscard]] std::expected<void, std::string> set_split(int row, int split_column, int mix_column);
    [[nodiscard]] std::expected<void, std::string> set_lane_output(int row, int param_index, float value);
    [[nodiscard]] std::expected<void, std::string> set_tempo_bpm(float bpm);

    /// Uhr des Geraets stellen (SystemTimeSync, Feld `ms_since_epoch`).
    /// Ohne Netz hat das Geraet keine Uhr: es startet bei 1970-01-01, und ein
    /// LocalBackup traegt dann `created "1970-01-01T00:14:47Z"` (Beleg 118).
    /// Die offizielle Cortex Control schickt diese Nachricht beim Verbinden.
    /// GRENZEN, beide gemessen am 2026-09-17 (Beleg 118):
    ///  * Das Geraet ANTWORTET nicht - ein READ bleibt stumm. Die Wirkung ist
    ///    nur mittelbar zu pruefen (das `created` des naechsten Backups).
    ///  * Die Nachricht wirkte, solange die Uhr des Geraets UNGESTELLT war
    ///    (Sprung von 1970 auf die richtige Zeit). Danach blieben zwei weitere
    ///    Versuche ohne Wirkung - ein Wert von 1970 und ein Versatz von einer
    ///    Stunde. Leitende Vermutung: Das Geraet nimmt die Zeit nur an, solange
    ///    es keine hat. Nicht bewiesen; der trennende Versuch braucht einen
    ///    Neustart. Daher wie die offizielle Cortex Control FRUEH aufrufen,
    ///    direkt nach dem Verbinden.
    [[nodiscard]] std::expected<void, std::string> sync_time(std::uint64_t ms_since_epoch);

    // ── files ───────────────────────────────────────────────────────────
    /// "Save As": snapshots the GRID into `folder_key` at `position` with `name`.
    [[nodiscard]] std::expected<void, std::string> save_current_preset(std::string_view folder_key, int position,
                                                                       std::string_view name, int instrument = 0);
    [[nodiscard]] std::expected<void, std::string> delete_preset(std::string_view folder_key, std::string_view name);
    [[nodiscard]] std::expected<void, std::string> move_preset(std::string_view folder_key, std::string_view name,
                                                               int to_position);

    // ── global ──────────────────────────────────────────────────────────
    [[nodiscard]] std::expected<void, std::string> set_master_volume(float volume);

    // ── backup / restore (LocalBackup) ──────────────────────────────────
    /// Read the on-device LocalBackup: sends a CREATE ("leg ein Backup an" -
    /// READ stays silent, evidence/100) and concatenates the `backup_json`
    /// chunks (until is_last_chunk) into one JSON string. The unit shows a
    /// modal dialog while it works and needs well over 20 s for a full unit,
    /// hence the wide default window.
    [[nodiscard]] std::expected<std::string, std::string> read_backup(ms window = ms{90000});
    /// Restore a LocalBackup: streams `backup_json` back as UPDATE chunks and
    /// waits for `applied_backup`. DESTRUCTIVE (overwrites presets/captures/IRs).
    [[nodiscard]] std::expected<void, std::string> restore_backup(std::string_view backup_json,
                                                                  std::size_t chunk = 60000, ms timeout = ms{60000});

    /// Wait for the next event of a kind (used by the CLI monitor and tests).
    [[nodiscard]] std::expected<Inbound, std::string> await_raw(MessageType type, ms timeout);

private:
    void on_inbound(const Inbound& in);
    void emit_event(const Event& e);
    [[nodiscard]] std::expected<void, std::string> send_grid(const ::BinaryPreset& sparse, Action action = Action::Update);
    [[nodiscard]] std::expected<void, std::string> check_cell(int row, int column) const;

    Session session_;
    std::mutex catalog_mutex_;
    std::optional<catalog::Catalog> catalog_;
    mutable std::mutex io_mutex_;
    iometer::Levels io_levels_;             ///< guarded by io_mutex_
    std::mutex sink_mutex_;
    /// Gehalten, solange emit_event laeuft; der Setter fuer den Sink nimmt ihn
    /// auch. Nach seiner Rueckkehr laeuft also kein Aufruf des alten Ziels mehr
    /// (rekursiv: ein Sink darf sich selbst abmelden).
    std::recursive_mutex callback_mutex_;
    EventSink sink_;
};

}  // namespace qc
