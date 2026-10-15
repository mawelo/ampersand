// preset_view.cpp — Sicht auf ein BinaryPreset (siehe preset_view.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
//
// Positionsregeln (docs/protocol.md §7.5/§9), alle auf Hardware bestaetigt:
//   * Chain.row fehlt auf einem recallten Preset -> Zeile = Index in chains
//   * Model.column fehlt -> Spalte = Index in models (8 Eintraege pro Zeile,
//     leere Zellen haben hash 0/absent)
//   * Param.index fehlt -> Parameter-Index = Position
//   * bypass[row].colBypass[col].sceneBypass[scene] positional; nur mit
//     sceneMode ist die Szenenliste massgeblich, sonst gilt Eintrag 0
#include "qc/preset_view.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>

namespace qc::view {

namespace {

std::expected<int, std::string> from_user(std::string_view what, int value, int count) {
    if (value < 1 || value > count) {
        return std::unexpected(std::format("{} {} is out of range: {}s are numbered 1 to {}, as on the unit",
                                           what, value, what, count));
    }
    return value - 1;
}

}  // namespace

std::expected<int, std::string> row_from_user(int row) { return from_user("row", row, kRows); }
std::expected<int, std::string> column_from_user(int column) { return from_user("column", column, kColumns); }

namespace {

constexpr std::array<std::pair<int, const char*>, 15> kInputPorts{{
    {0, "Empty"},     {1, "Input 1"},  {2, "Input 2"},  {3, "Input 1/2"}, {4, "Return 1"},
    {5, "Return 2"},  {6, "Return 1/2"}, {7, "Prev. Row"}, {8, "USB 5"},   {9, "USB 6"},
    {10, "USB 7"},    {11, "USB 8"},   {12, "USB 5/6"}, {13, "USB 7/8"},  {14, "Sidechain"},
}};

constexpr std::array<std::pair<int, const char*>, 23> kOutputPorts{{
    {0, "Empty"},       {1, "Output 1/2"}, {2, "Output 3/4"}, {3, "Send 1/2"},  {4, "Output 1"},
    {5, "Output 2"},    {6, "Output 3"},   {7, "Output 4"},   {8, "Send 1"},    {9, "Send 2"},
    {10, "USB 5"},      {11, "USB 6"},     {12, "USB 7"},     {13, "USB 8"},    {14, "USB 5/6"},
    {15, "USB 7/8"},    {16, "Row 3"},     {17, "Row 4"},     {18, "Row 3/4"},  {19, "Multi Out"},
    {20, "USB 3"},      {21, "USB 4"},     {22, "USB 3/4"},
}};

constexpr std::uint32_t kTempoControlModel = 25000;
constexpr std::uint32_t kCaptureModelLow = 14000;
constexpr std::uint32_t kCaptureModelHigh = 14999;
constexpr int kCaptureFileNameParam = 5;

std::optional<float> scene_value(const ::Param& p, int scene) {
    if (p.param_values_size() == 0) return std::nullopt;
    int idx = 0;
    if (p.scene_mode() && scene >= 0 && scene < p.param_values_size()) idx = scene;
    const auto& pv = p.param_values(idx);
    if (pv.value_case() == ::ParamValue::kFloatValue) return pv.float_value();
    if (pv.value_case() == ::ParamValue::kIntValue) return static_cast<float>(pv.int_value());
    return std::nullopt;
}

std::optional<std::string> scene_text(const ::Param& p, int scene) {
    if (p.param_values_size() == 0) return std::nullopt;
    int idx = 0;
    if (p.scene_mode() && scene >= 0 && scene < p.param_values_size()) idx = scene;
    const auto& pv = p.param_values(idx);
    if (pv.value_case() == ::ParamValue::kStringValue) return pv.string_value();
    return std::nullopt;
}

std::vector<ParamView> build_params(const ::Model& model, const catalog::Model* cm, int scene) {
    std::vector<ParamView> out;
    out.reserve(static_cast<std::size_t>(model.params_size()));
    for (int i = 0; i < model.params_size(); ++i) {
        const auto& p = model.params(i);
        ParamView pv;
        pv.index = p.has_index() ? static_cast<int>(p.index()) : i;
        pv.scene_mode = p.scene_mode();
        pv.value = scene_value(p, scene);
        pv.text = scene_text(p, scene);
        if (p.has_expression() && p.expression() > 0) pv.expression = p.expression();
        for (const auto& s : p.dynamic_steps()) pv.options.push_back(s);
        if (const auto* cp = cm ? cm->parameter(pv.index) : nullptr) {
            pv.name = cp->name;
            pv.minimum = cp->minimum;
            pv.maximum = cp->maximum;
            pv.units = cp->units;
            pv.type = cp->type;
            pv.placeholder_range = cp->range_is_placeholder();
            pv.option_count = cp->option_count();
        } else {
            pv.name = std::format("param {}", pv.index);
        }
        if (!pv.options.empty()) pv.option_count = static_cast<int>(pv.options.size());
        out.push_back(std::move(pv));
    }
    return out;
}

struct BypassCell {
    bool bypassed{false};
    bool scene_mode{false};
};

BypassCell bypass_state(const ::BinaryPreset& preset, int row, int column, int scene) {
    BypassCell cell;
    for (int r = 0; r < preset.bypass_size(); ++r) {
        const auto& b = preset.bypass(r);
        const int brow = b.has_row() ? static_cast<int>(b.row()) : r;
        if (brow != row) continue;
        for (int c = 0; c < b.colbypass_size(); ++c) {
            const auto& cb = b.colbypass(c);
            const int bcol = cb.has_column() ? static_cast<int>(cb.column()) : c;
            if (bcol != column) continue;
            cell.scene_mode = cb.scenemode();
            if (cb.scenebypass_size() == 0) return cell;
            const int idx = (cell.scene_mode && scene >= 0 && scene < cb.scenebypass_size()) ? scene : 0;
            cell.bypassed = cb.scenebypass(idx).bypass();
            return cell;
        }
    }
    return cell;
}

}  // namespace

std::optional<float> ParamView::real_value() const noexcept {
    if (!value || placeholder_range) return std::nullopt;
    const float span = maximum - minimum;
    return minimum + std::clamp(*value, 0.0f, 1.0f) * span;
}

std::string ParamView::display_value() const {
    if (text) return *text;
    if (!value) return "-";
    if (option_count && *option_count > 1) {
        const int opt = static_cast<int>(std::lround(*value * static_cast<float>(*option_count - 1)));
        if (opt >= 0 && static_cast<std::size_t>(opt) < options.size()) return options[static_cast<std::size_t>(opt)];
        return std::format("option {}", opt);
    }
    if (auto r = real_value()) {
        if (units.empty()) return std::format("{:.3g}", *r);
        return std::format("{:.2f} {}", *r, units);
    }
    return std::format("{:.3f}", *value);
}

std::string RowView::input_label() const {
    if (fed_from_row) return "Prev. Row";
    if (in_port > 0) return in_name;
    return "";
}

const BlockView* RowView::block_at(int column) const noexcept {
    for (const auto& b : blocks) {
        if (b.column == column) return &b;
    }
    return nullptr;
}

PresetView build(const ::BinaryPreset& preset, const catalog::Catalog* cat, int scene) {
    PresetView v;
    v.name = preset.name();
    v.author = preset.author_name();
    v.default_scene = preset.has_default_scene() ? static_cast<int>(preset.default_scene()) : 0;
    v.scene = scene;
    v.created_version = preset.created_version_size() > 0 ? preset.created_version(0) : "";
    v.modified_version = preset.modified_version_size() > 0 ? preset.modified_version(0) : "";
    for (const auto& t : preset.tags()) v.tags.push_back(t);
    v.scene_labels.assign(kScenes, " ");
    v.scene_colors.assign(kScenes, 0xFF808080u);
    for (int i = 0; i < preset.scene_labels_size() && i < kScenes; ++i) v.scene_labels[static_cast<std::size_t>(i)] = preset.scene_labels(i);
    for (int i = 0; i < preset.scene_colors_size() && i < kScenes; ++i) v.scene_colors[static_cast<std::size_t>(i)] = preset.scene_colors(i);

    v.rows.resize(kRows);
    for (int r = 0; r < kRows; ++r) v.rows[static_cast<std::size_t>(r)].row = r;

    for (int ci = 0; ci < preset.chains_size(); ++ci) {
        const auto& chain = preset.chains(ci);
        const int row = chain.has_row() ? static_cast<int>(chain.row()) : ci;
        if (row < 0 || row >= kRows) continue;
        auto& rv = v.rows[static_cast<std::size_t>(row)];
        if (chain.has_in_portid()) {
            rv.in_port = static_cast<int>(chain.in_portid());
            rv.in_name = input_port_name(rv.in_port);
        }
        if (chain.has_out_portid()) {
            rv.out_port = static_cast<int>(chain.out_portid());
            rv.out_name = output_port_name(rv.out_port);
        }
        for (int mi = 0; mi < chain.models_size(); ++mi) {
            const auto& model = chain.models(mi);
            if (!model.has_hash() || model.hash() == 0) continue;
            BlockView b;
            b.row = row;
            b.column = model.has_column() ? static_cast<int>(model.column()) : mi;
            b.model_id = model.hash();
            const auto* cm = cat ? cat->find(static_cast<int>(model.hash())) : nullptr;
            b.name = cm ? cm->name : std::format("#{}", model.hash());
            b.category = cm ? cm->category : "";
            b.is_capture = (model.hash() >= kCaptureModelLow && model.hash() <= kCaptureModelHigh) ||
                           (cm && cm->is_capture_block());
            b.params = build_params(model, cm, scene);
            if (b.is_capture) {
                for (const auto& p : b.params) {
                    if (p.index == kCaptureFileNameParam && p.text && !p.text->empty()) {
                        b.name = capture_display_name(*p.text);
                        break;
                    }
                }
            }
            const auto bp = bypass_state(preset, row, b.column, scene);
            b.bypassed = bp.bypassed;
            b.scene_mode_bypass = bp.scene_mode;
            rv.blocks.push_back(std::move(b));
        }
        std::sort(rv.blocks.begin(), rv.blocks.end(), [](const BlockView& a, const BlockView& b2) { return a.column < b2.column; });

        for (const auto& scp : chain.split_control_points()) {
            if (scp.split() < 0 && scp.mix() < 0) continue;
            rv.split = SplitView{scp.split(), scp.mix()};
        }
        if (chain.output_control_size() > 0) {
            const auto& oc = chain.output_control(0);
            const auto* cm = cat ? cat->find(oc.has_hash() ? static_cast<int>(oc.hash()) : 23000) : nullptr;
            rv.output_control = build_params(oc, cm, scene);
        }
        if (chain.input_control_size() > 0) {
            const auto& ic = chain.input_control(0);
            const auto* cm = cat ? cat->find(ic.has_hash() ? static_cast<int>(ic.hash()) : 28000) : nullptr;
            rv.input_control = build_params(ic, cm, scene);
        }
    }

    // Internal routing: out 16 feeds row 3 (index 2), 17 feeds row 4 (index 3), 18 both.
    for (const auto& r : v.rows) {
        if (r.out_port == 16 || r.out_port == 18) v.rows[2].fed_from_row = r.row;
        if (r.out_port == 17 || r.out_port == 18) v.rows[3].fed_from_row = r.row;
    }

    if (preset.tempoprogramdata_size() > 0) {
        const auto& tempo = preset.tempoprogramdata(0);
        if (tempo.params_size() > 0) {
            if (auto val = scene_value(tempo.params(0), scene)) v.tempo_bpm = 40.0f + *val * 200.0f;
        }
    }
    (void)kTempoControlModel;
    return v;
}

std::string input_port_name(int id) {
    for (const auto& [k, n] : kInputPorts) {
        if (k == id) return n;
    }
    return std::format("in {}", id);
}

std::string output_port_name(int id) {
    for (const auto& [k, n] : kOutputPorts) {
        if (k == id) return n;
    }
    return std::format("out {}", id);
}

std::vector<std::pair<int, std::string>> input_ports() {
    std::vector<std::pair<int, std::string>> out;
    for (const auto& [k, n] : kInputPorts) out.emplace_back(k, n);
    return out;
}

std::vector<std::pair<int, std::string>> output_ports() {
    std::vector<std::pair<int, std::string>> out;
    for (const auto& [k, n] : kOutputPorts) out.emplace_back(k, n);
    return out;
}

std::expected<int, std::string> slot_to_position(std::string_view slot) {
    while (!slot.empty() && slot.front() == ' ') slot.remove_prefix(1);
    while (!slot.empty() && slot.back() == ' ') slot.remove_suffix(1);
    if (slot.empty()) return std::unexpected("empty slot");
    int number = 0;
    const auto last = slot.back();
    const bool lettered = (last >= 'A' && last <= 'H') || (last >= 'a' && last <= 'h');
    const auto digits = lettered ? slot.substr(0, slot.size() - 1) : slot;
    const auto parsed = digits.empty() ? std::from_chars_result{digits.data(), std::errc::invalid_argument}
                                       : std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
        return std::unexpected(std::format("slot must look like '28C' (bank 1-32 + letter A-H) or a position 0-255: '{}'", slot));
    }
    if (!lettered) {
        if (number < 0 || number >= kSlotsPerSetlist) return std::unexpected(std::format("position {} out of range 0-255", number));
        return number;
    }
    if (number < 1 || number > 32) return std::unexpected(std::format("bank must be 1-32: '{}'", slot));
    const int letter = (last >= 'a') ? last - 'a' : last - 'A';
    return (number - 1) * 8 + letter;
}

std::string position_to_slot(int position) {
    if (position < 0 || position >= kSlotsPerSetlist) return std::format("#{}", position);
    return std::format("{}{}", position / 8 + 1, static_cast<char>('A' + position % 8));
}

std::string scene_letter(int scene) {
    if (scene < 0 || scene >= kScenes) return "?";
    return std::string(1, static_cast<char>('A' + scene));
}

bool same_folder(std::string_view a, std::string_view b) noexcept {
    while (!a.empty() && a.back() == '/') a.remove_suffix(1);
    while (!b.empty() && b.back() == '/') b.remove_suffix(1);
    return a == b;
}

bool is_factory_setlist(std::string_view folder_key) noexcept {
    return same_folder(folder_key, kFactorySetlist) || folder_key.starts_with("/opt/neuraldsp/");
}

std::string capture_display_name(std::string_view file_name) {
    constexpr std::size_t kHash = 64;
    if (file_name.size() > kHash &&
        std::all_of(file_name.begin(), file_name.begin() + kHash, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        return std::string(file_name.substr(kHash));
    }
    return std::string(file_name);
}

nlohmann::json to_json(const PresetView& v) {
    nlohmann::json j;
    j["note"] = "rows and columns are 1-based, as the unit labels them and as qcctl expects them";
    j["name"] = v.name;
    j["author"] = v.author;
    j["default_scene"] = v.default_scene;
    j["scene"] = v.scene;
    j["scene_labels"] = v.scene_labels;
    j["scene_colors"] = v.scene_colors;
    if (v.tempo_bpm) j["tempo_bpm"] = *v.tempo_bpm;
    j["created_version"] = v.created_version;
    j["modified_version"] = v.modified_version;
    j["tags"] = v.tags;
    j["rows"] = nlohmann::json::array();
    for (const auto& r : v.rows) {
        nlohmann::json jr;
        jr["row"] = row_to_user(r.row);
        jr["in_port"] = r.in_port;
        jr["in_name"] = r.in_name;
        jr["out_port"] = r.out_port;
        jr["out_name"] = r.out_name;
        if (r.split) jr["split"] = {{"split_column", column_to_user(r.split->split_column)},
                                    {"mix_column", column_to_user(r.split->mix_column)}};
        jr["blocks"] = nlohmann::json::array();
        for (const auto& b : r.blocks) {
            nlohmann::json jb;
            jb["column"] = column_to_user(b.column);
            jb["model_id"] = b.model_id;
            jb["name"] = b.name;
            jb["category"] = b.category;
            jb["bypassed"] = b.bypassed;
            jb["is_capture"] = b.is_capture;
            jb["params"] = nlohmann::json::array();
            for (const auto& p : b.params) {
                nlohmann::json jp;
                jp["index"] = p.index;
                jp["name"] = p.name;
                if (p.value) jp["value"] = *p.value;
                if (p.text) jp["text"] = *p.text;
                if (auto rv = p.real_value()) jp["real"] = *rv;
                jp["display"] = p.display_value();
                jp["units"] = p.units;
                jp["type"] = p.type;
                jp["scene_mode"] = p.scene_mode;
                if (!p.options.empty()) jp["options"] = p.options;
                if (p.expression) jp["expression"] = *p.expression;
                jb["params"].push_back(std::move(jp));
            }
            jr["blocks"].push_back(std::move(jb));
        }
        auto lane = nlohmann::json::array();
        for (const auto& p : r.output_control) lane.push_back({{"index", p.index}, {"name", p.name}, {"display", p.display_value()}});
        jr["output_control"] = std::move(lane);
        j["rows"].push_back(std::move(jr));
    }
    return j;
}

namespace {

nlohmann::json param_report(const ParamView& p) {
    nlohmann::json jp{{"index", p.index}, {"name", p.name}, {"value", p.display_value()}};
    if (p.text) jp["text"] = *p.text;
    if (auto r = p.real_value()) jp["real"] = *r;
    if (p.value) jp["normalized"] = *p.value;
    if (!p.units.empty()) jp["units"] = p.units;
    if (!p.options.empty() && p.value) {
        const int opt = static_cast<int>(std::lround(*p.value * static_cast<float>(static_cast<int>(p.options.size()) - 1)));
        jp["option"] = opt;
    }
    if (p.scene_mode) jp["scene_mode"] = true;
    if (p.expression) jp["expression_pedal"] = *p.expression;
    return jp;
}

nlohmann::json block_report(const BlockView& b) {
    nlohmann::json jb{{"item", b.name}, {"category", b.category}, {"model_id", b.model_id},
                      {"row", row_to_user(b.row)}, {"column", column_to_user(b.column)}, {"bypassed", b.bypassed}};
    if (b.is_capture) jb["neural_capture"] = true;
    jb["settings"] = nlohmann::json::array();
    for (const auto& p : b.params) {
        if (p.type == "meter" || p.type == "grMeter" || p.type == "stereoMeter" || p.type == "stereoGrMeter") continue;
        jb["settings"].push_back(param_report(p));
    }
    return jb;
}

nlohmann::json blocks_between(const RowView& r, int from, int to) {
    auto arr = nlohmann::json::array();
    for (const auto& b : r.blocks) {
        if (b.column >= from && b.column <= to) arr.push_back(block_report(b));
    }
    return arr;
}

}  // namespace

nlohmann::json chain_report(const PresetView& v) {
    nlohmann::json j;
    j["preset"] = v.name;
    j["author"] = v.author;
    j["scene"] = scene_letter(v.scene);
    j["scene_label"] = v.scene_labels[static_cast<std::size_t>(std::clamp(v.scene, 0, kScenes - 1))];
    if (v.tempo_bpm) j["tempo_bpm"] = *v.tempo_bpm;
    j["note"] = "rows/columns are 1-based as on the unit; 'parallel' items carry path_a (this row) and path_b (the lane row "
                "below), columns split..mix inclusive (assumption)";
    j["chains"] = nlohmann::json::array();
    for (const auto& r : v.rows) {
        const bool lane_of_previous = (r.row % 2 == 1) && v.rows[static_cast<std::size_t>(r.row - 1)].split.has_value();
        if (r.in_port <= 0 && !r.fed_from_row && r.blocks.empty()) continue;
        if (lane_of_previous && r.in_port <= 0 && !r.fed_from_row) continue;  // reported inside the parallel item
        nlohmann::json chain{{"row", row_to_user(r.row)}, {"input", r.input_label().empty() ? "-" : r.input_label()},
                             {"output", r.out_name.empty() ? "-" : r.out_name}, {"input_port", r.in_port},
                             {"output_port", r.out_port}};
        if (r.fed_from_row) chain["fed_from_row"] = row_to_user(*r.fed_from_row);
        auto items = nlohmann::json::array();
        const RowView* lane = (r.row % 2 == 0 && r.row + 1 < kRows) ? &v.rows[static_cast<std::size_t>(r.row + 1)] : nullptr;
        if (r.split && lane) {
            // split < 0: the lane is fed by its own input and only merges at `mix`.
            const bool forks = r.split->split_column >= 0;
            const int split = forks ? std::clamp(r.split->split_column, 0, kColumns - 1) : 0;
            const int mix = r.split->rejoins() ? std::clamp(r.split->mix_column, split, kColumns - 1) : kColumns - 1;
            for (const auto& b : blocks_between(r, 0, split - 1)) items.push_back(b);
            nlohmann::json par{{"type", "parallel"},
                               {"split_column", forks ? nlohmann::json(column_to_user(split)) : nlohmann::json(nullptr)},
                               {"mix_column", r.split->rejoins() ? nlohmann::json(column_to_user(mix)) : nlohmann::json(nullptr)},
                               {"path_a", blocks_between(r, split, mix)}, {"path_b", blocks_between(*lane, split, mix)},
                               {"lane_row", row_to_user(lane->row)}};
            if (!lane->output_control.empty()) {
                par["path_b_lane_output"] = nlohmann::json::array();
                for (const auto& p : lane->output_control) par["path_b_lane_output"].push_back(param_report(p));
            }
            items.push_back(par);
            for (const auto& b : blocks_between(r, mix + 1, kColumns - 1)) items.push_back(b);
        } else {
            for (const auto& b : blocks_between(r, 0, kColumns - 1)) items.push_back(b);
        }
        chain["items"] = std::move(items);
        if (!r.input_control.empty()) {
            chain["input_gate"] = nlohmann::json::array();
            for (const auto& p : r.input_control) chain["input_gate"].push_back(param_report(p));
        }
        if (!r.output_control.empty()) {
            chain["lane_output"] = nlohmann::json::array();
            for (const auto& p : r.output_control) chain["lane_output"].push_back(param_report(p));
        }
        j["chains"].push_back(std::move(chain));
    }
    return j;
}

nlohmann::json to_json(const catalog::Model& m) {
    nlohmann::json j;
    j["id"] = m.id;
    j["name"] = m.name;
    j["category"] = m.category;
    j["category_id"] = m.category_id;
    j["based_on"] = m.based_on;
    j["factory"] = m.is_factory();
    j["superseded"] = m.superseded;
    j["parameters"] = nlohmann::json::array();
    for (const auto& p : m.parameters) {
        nlohmann::json jp{{"index", p.index}, {"name", p.name}, {"min", p.minimum}, {"max", p.maximum},
                          {"default", p.default_value}, {"units", p.units}, {"type", p.type}};
        if (p.steps) jp["steps"] = *p.steps;
        j["parameters"].push_back(std::move(jp));
    }
    return j;
}

}  // namespace qc::view
