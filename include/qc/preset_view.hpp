// preset_view.hpp — lesbare Sicht auf ein BinaryPreset (Grid, Szenen, Ports)
//
// Wer:      Martin.Lonkwitz
// Was:      Reine Funktionen: BinaryPreset (+Katalog, +aktive Szene) -> View-
//           Strukturen fuer GUI/CLI/MCP; Slot-/Port-Namen; JSON-Export.
// Wann:     2026-09-04
// Warum:    Das Wire-Format ist positionsbasiert (Zeile = Index in chains,
//           Spalte = Index in models, Parameter-Index = Position) und kennt
//           keine Namen; diese Schicht macht daraus etwas Anzeigbares.
// Projekt:  Ampersand
// Abhaengig: catalog, generierte Protobuf-Klassen, nlohmann_json
// Nutzung:  auto v = qc::view::build(preset, &catalog, scene);
// Historie: git log
#pragma once

#include "qc/catalog.hpp"

#include <Preset.pb.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qc::view {

inline constexpr int kRows = 4;
inline constexpr int kColumns = 8;
inline constexpr int kScenes = 8;
inline constexpr int kSlotsPerSetlist = 256;

/// Rows and columns are 1-based wherever a person sees them: on the unit's own
/// display, in `qcctl export`, in the GUI, in the JSON. Inside the library they
/// are 0-based indices. These convert at that boundary — and say plainly which
/// counting is meant when a value falls outside, because mixing the two silently
/// writes to the neighbouring cell.
[[nodiscard]] std::expected<int, std::string> row_from_user(int row);
[[nodiscard]] std::expected<int, std::string> column_from_user(int column);
[[nodiscard]] constexpr int row_to_user(int index) noexcept { return index + 1; }
[[nodiscard]] constexpr int column_to_user(int index) noexcept { return index + 1; }

inline constexpr std::string_view kUserSetlist = "/media/p4/Presets/My Presets";
/// Trailing slash: Cortex Control sends it verbatim on a factory RECALL; the
/// device reports the folder's LISTING key without it.
inline constexpr std::string_view kFactorySetlist = "/opt/neuraldsp/Factory Library/";

struct ParamView {
    int index{0};
    std::string name;
    std::optional<float> value;        ///< normalized 0..1 (for the requested scene)
    std::optional<std::string> text;   ///< string-valued parameters (mic selection, IR, capture)
    float minimum{0.0f};
    float maximum{1.0f};
    std::string units;
    std::string type;
    bool scene_mode{false};
    bool placeholder_range{false};
    std::vector<std::string> options;  ///< Param.dynamic_steps (comboBox names live in the preset)
    std::optional<int> option_count;
    std::optional<int> expression;     ///< pedal 1/2 if assigned
    /// Value in the parameter's own units (dB, ms, ...), when the range is real.
    [[nodiscard]] std::optional<float> real_value() const noexcept;
    [[nodiscard]] std::string display_value() const;
};

struct BlockView {
    int row{0};
    int column{0};
    std::uint32_t model_id{0};
    std::string name;
    std::string category;
    bool bypassed{false};
    bool scene_mode_bypass{false};
    bool is_capture{false};
    std::vector<ParamView> params;
};

struct SplitView {
    int split_column{-1};
    int mix_column{-1};
    [[nodiscard]] bool rejoins() const noexcept { return mix_column >= 0; }
};

struct RowView {
    int row{0};
    int in_port{-1};
    int out_port{-1};
    std::string in_name;
    std::string out_name;
    std::vector<BlockView> blocks;   ///< occupied cells only, ordered by column
    std::optional<SplitView> split;  ///< active branch (even rows only)
    /// Another row routes into this one via out_portid 16-18 ("Row 3"/"Row 4");
    /// the unit then labels this row's input "Prev. Row".
    std::optional<int> fed_from_row;
    /// The input label as the unit shows it: port name, "Prev. Row", or "" (none).
    [[nodiscard]] std::string input_label() const;
    std::vector<ParamView> output_control;  ///< Lane Output Control (model 23000)
    std::vector<ParamView> input_control;   ///< Input Gate (model 28000)
    [[nodiscard]] const BlockView* block_at(int column) const noexcept;
};

struct PresetView {
    std::string name;
    std::string author;
    int default_scene{0};
    int scene{0};  ///< the scene this view was built for
    std::vector<std::string> scene_labels;   ///< 8 entries
    std::vector<std::uint32_t> scene_colors; ///< 8 entries, ARGB
    std::vector<RowView> rows;               ///< 4 entries
    std::optional<float> tempo_bpm;
    std::string created_version;
    std::string modified_version;
    std::vector<std::string> tags;
};

/// Build the view. `cat` may be null (names fall back to "#id").
[[nodiscard]] PresetView build(const ::BinaryPreset& preset, const catalog::Catalog* cat, int scene);

[[nodiscard]] std::string input_port_name(int id);
[[nodiscard]] std::string output_port_name(int id);
[[nodiscard]] std::vector<std::pair<int, std::string>> input_ports();
[[nodiscard]] std::vector<std::pair<int, std::string>> output_ports();

/// "28C" -> 218 (zero-based linear position); also accepts a plain number.
[[nodiscard]] std::expected<int, std::string> slot_to_position(std::string_view slot);
/// 218 -> "28C"
[[nodiscard]] std::string position_to_slot(int position);
[[nodiscard]] std::string scene_letter(int scene);
[[nodiscard]] bool is_factory_setlist(std::string_view folder_key) noexcept;
/// Compare folder keys ignoring a trailing slash.
[[nodiscard]] bool same_folder(std::string_view a, std::string_view b) noexcept;

/// Extract the display name of a capture block from its file_name parameter
/// (`<64-char hash><name>`).
[[nodiscard]] std::string capture_display_name(std::string_view file_name);

[[nodiscard]] nlohmann::json to_json(const PresetView& v);

/// Signal-flow report: for every row with a physical input, the items in
/// signal order (blocks; a `parallel` item for a branch with path_a = the row
/// itself and path_b = the lane row below), every parameter with its value in
/// the parameter's own units. Meant for rebuilding the rig elsewhere.
/// Assumption (documented, not hardware-verified): a branch covers columns
/// split..mix inclusive on both rows.
[[nodiscard]] nlohmann::json chain_report(const PresetView& v);
[[nodiscard]] nlohmann::json to_json(const catalog::Model& m);

}  // namespace qc::view
