// tools.cpp — MCP-Werkzeuge (siehe tools.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "tools.hpp"

#include "qc/hid_transport.hpp"
#include "qc/log.hpp"
#include "qc/version.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <variant>

#include <sstream>

namespace qcmcp {

using mcp::json;
using mcp::ToolResult;

namespace {

json schema(std::initializer_list<std::pair<const char*, json>> props, std::initializer_list<const char*> required = {}) {
    json s{{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
    for (const auto& [k, v] : props) s["properties"][k] = v;
    json req = json::array();
    for (auto r : required) req.push_back(r);
    s["required"] = req;
    return s;
}

json str(const char* desc) { return {{"type", "string"}, {"description", desc}}; }
json integer(const char* desc, int lo, int hi) { return {{"type", "integer"}, {"description", desc}, {"minimum", lo}, {"maximum", hi}}; }
json number(const char* desc, double lo, double hi) { return {{"type", "number"}, {"description", desc}, {"minimum", lo}, {"maximum", hi}}; }
json boolean(const char* desc) { return {{"type", "boolean"}, {"description", desc}}; }

// Gepruefter Zugriff auf Tool-Argumente. operator[] auf einem const json ist
// bei fehlendem Schluessel undefiniert (Review M3); diese Helfer melden einen
// fehlenden Schluessel oder einen falschen JSON-Typ als klare Ausnahme, die der
// Server (mcp_stdio_server.hpp) im try um den Handler als Tool-Fehler ausgibt.
// Der Wertebereich bleibt bei den Fachfunktionen (qc::view::row_from_user u.a.).
[[noreturn]] void arg_fail(std::string_view key, std::string_view want) {
    throw std::invalid_argument(std::format("argument '{}' must be {}", key, want));
}
const json& arg(const json& args, std::string_view key) {
    const auto it = args.find(key);
    if (it == args.end()) throw std::invalid_argument(std::format("missing argument '{}'", key));
    return *it;
}
int req_int(const json& a, std::string_view k) {
    const auto& v = arg(a, k);
    if (!v.is_number_integer()) arg_fail(k, "an integer");
    return v.get<int>();
}
std::uint32_t req_u32(const json& a, std::string_view k) {
    const auto& v = arg(a, k);
    if (!v.is_number_integer()) arg_fail(k, "an integer");
    const auto n = v.get<std::int64_t>();
    if (n < 0 || n > 0xFFFF'FFFFLL) arg_fail(k, "a 0..4294967295 integer");
    return static_cast<std::uint32_t>(n);
}
float req_float(const json& a, std::string_view k) {
    const auto& v = arg(a, k);
    if (!v.is_number()) arg_fail(k, "a number");
    return v.get<float>();
}
bool req_bool(const json& a, std::string_view k) {
    const auto& v = arg(a, k);
    if (!v.is_boolean()) arg_fail(k, "true or false");
    return v.get<bool>();
}
std::string req_str(const json& a, std::string_view k) {
    const auto& v = arg(a, k);
    if (!v.is_string()) arg_fail(k, "a string");
    return v.get<std::string>();
}

std::optional<int> scene_arg(const json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) return std::nullopt;
    if (args[key].is_number_integer()) return args[key].get<int>();
    if (args[key].is_string()) {
        const auto s = args[key].get<std::string>();
        if (s.size() == 1) {
            const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
            if (c >= 'A' && c <= 'H') return c - 'A';
        }
    }
    return std::nullopt;
}

std::string folder_arg(const json& args) {
    const std::string f = args.value("folder", "");
    if (f.empty() || f == "user") return std::string(qc::view::kUserSetlist);
    if (f == "factory") return std::string(qc::view::kFactorySetlist);
    return f;
}

std::expected<int, std::string> position_arg(const json& args) {
    if (args.contains("position") && args["position"].is_number_integer()) {
        const int p = args["position"].get<int>();
        if (p < 0 || p > 255) return std::unexpected("position must be 0..255");
        return p;
    }
    if (args.contains("slot") && args["slot"].is_string()) return qc::view::slot_to_position(args["slot"].get<std::string>());
    return std::unexpected("give `slot` (e.g. \"28C\") or `position` (0..255)");
}

/// Row and column arrive the way the unit labels them: 1 to 4, 1 to 8. The
/// conversion to library indices lives in qc::view, so CLI, MCP and GUI cannot
/// drift apart — mixing the two counts silently writes to the next cell.
std::expected<std::pair<int, int>, std::string> cell_of(const json& args) {
    auto row = qc::view::row_from_user(req_int(args, "row"));
    if (!row) return std::unexpected(row.error());
    auto column = qc::view::column_from_user(req_int(args, "column"));
    if (!column) return std::unexpected(column.error());
    return std::pair{*row, *column};
}

}  // namespace

Tools::Tools(Config cfg)
    : cfg_(std::move(cfg)), backups_(cfg_.backup_dir.empty() ? BackupStore::default_dir() : cfg_.backup_dir) {}

std::expected<qc::QuadCortex*, std::string> Tools::device() {
    if (qc_ && qc_->lost()) qc_.reset();
    if (!qc_) {
        qc::Session::Options o;
        o.cortex_control_version = cfg_.cortex_control_version;
        auto d = qc::QuadCortex::open(o);
        if (!d) return std::unexpected(d.error());
        qc_ = std::move(*d);
    }
    if (!qc_->connected()) {
        if (auto c = qc_->connect(); !c) return std::unexpected(c.error());
    }
    return qc_.get();
}

void Tools::drop_device() {
    if (qc_) qc_->disconnect();
    qc_.reset();
}

void Tools::register_all(mcp::Server& server) {
    auto guarded = [this](auto fn) {
        return [this, fn](const json& args) -> ToolResult {
            std::lock_guard lk{mutex_};
            return fn(args);
        };
    };
    auto with_device = [this, guarded](auto fn) {
        return guarded([this, fn](const json& args) -> ToolResult {
            auto d = device();
            if (!d) return ToolResult::error(d.error());
            return fn(**d, args);
        });
    };
    auto writable = [this](auto fn) {
        return [this, fn](qc::QuadCortex& d, const json& args) -> ToolResult {
            if (!cfg_.allow_writes) return ToolResult::error("writes are disabled (start qc-mcp with --allow-writes)");
            return fn(d, args);
        };
    };
    auto ok_or_err = [](const std::expected<void, std::string>& r, std::string ok = "ok") {
        return r ? ToolResult::ok(std::move(ok)) : ToolResult::error(r.error());
    };

    // ── discovery / lifecycle ───────────────────────────────────────────
    server.add_tool({"qc_devices", "List Quad Cortex USB HID device nodes and whether they are accessible.",
                     schema({}), guarded([](const json&) {
                         json j = json::array();
                         for (const auto& d : qc::hid::enumerate())
                             j.push_back({{"path", d.path}, {"name", d.name}, {"phys", d.phys}, {"accessible", d.accessible}});
                         return ToolResult::ok_json(json{{"devices", j}, {"tool_version", std::string(qc::version())}});
                     })});

    server.add_tool({"qc_status",
                     "Connect (handshake) if needed and report firmware, serial, current preset position, active scene, "
                     "mode, master volume and dirty flag.",
                     schema({}), with_device([](qc::QuadCortex& d, const json&) {
                         const auto v = d.version();
                         json j{{"connected", d.connected()}, {"transport", d.session().transport_description()},
                                {"app_fw_version", v.app_fw_version}, {"serial", v.serial_number}, {"mac", v.mac_address},
                                {"name", v.custom_name}};
                         if (auto p = d.current_position(std::chrono::milliseconds{5000}))
                             j["position"] = {{"folder", p->folder_key}, {"position", p->position}, {"slot", qc::view::position_to_slot(p->position)}, {"factory", p->is_factory}};
                         if (auto s = d.active_scene(std::chrono::milliseconds{5000})) j["scene"] = qc::view::scene_letter(*s);
                         if (auto m = d.mode(std::chrono::milliseconds{3000})) j["mode"] = *m;
                         if (auto mv = d.master_volume(std::chrono::milliseconds{3000})) j["master_volume"] = *mv;
                         if (auto dirty = d.preset_dirty(std::chrono::milliseconds{3000})) j["dirty"] = *dirty;
                         const auto st = d.session().stats();
                         j["stats"] = {{"rx_messages", st.received_messages}, {"tx_messages", st.sent_messages}, {"undelivered", st.undelivered}};
                         return ToolResult::ok_json(j);
                     })});

    server.add_tool({"qc_disconnect", "Say goodbye to the device and release the USB interface.", schema({}),
                     guarded([this](const json&) {
                         drop_device();
                         return ToolResult::ok("disconnected");
                     })});

    // ── library ─────────────────────────────────────────────────────────
    server.add_tool({"qc_list_folders",
                     "List every folder the device enumerates. type: 0 presets/setlists (default), 1 IRs, 2 Neural Captures.",
                     schema({{"type", integer("File category: 0 presets, 1 IRs, 2 captures", 0, 2)},
                             {"window_ms", integer("how long to gather pushes (default 12000)", 1000, 60000)}}),
                     with_device([](qc::QuadCortex& d, const json& args) {
                         auto f = d.list_folders(args.value("type", 0), std::chrono::milliseconds{args.value("window_ms", 12000)});
                         if (!f) return ToolResult::error(f.error());
                         json j = json::array();
                         for (const auto& x : *f)
                             j.push_back({{"key", x.key}, {"name", x.name}, {"parent", x.parent_key}, {"factory", x.is_factory}, {"files", x.file_count}});
                         return ToolResult::ok_json(json{{"folders", j}});
                     })});

    server.add_tool({"qc_list_presets",
                     "List the occupied slots of a folder. folder: \"user\" (My Presets, default), \"factory\", or any folder key.",
                     schema({{"folder", str("folder key, \"user\" or \"factory\"")}, {"include_empty", boolean("also list empty slots")}}),
                     with_device([](qc::QuadCortex& d, const json& args) {
                         auto l = d.list_presets(folder_arg(args), args.value("include_empty", false));
                         if (!l) return ToolResult::error(l.error());
                         json j = json::array();
                         for (const auto& e : *l)
                             j.push_back({{"index", e.index}, {"slot", e.slot}, {"name", e.name}, {"author", e.author}, {"instrument", e.instrument}, {"key", e.key}});
                         return ToolResult::ok_json(json{{"folder", folder_arg(args)}, {"presets", j}});
                     })});

    server.add_tool({"qc_recall_preset",
                     "Recall (load) a preset by slot (\"28C\"), position (0..255) or name. LOADS it onto the grid and interrupts "
                     "audio. Returns the pushed preset as a grid view.",
                     schema({{"folder", str("folder key, \"user\" or \"factory\"")}, {"slot", str("slot like 28C")},
                             {"position", integer("linear position", 0, 255)}, {"name", str("preset name (looked up in folder)")}}),
                     with_device(writable([](qc::QuadCortex& d, const json& args) {
                         const auto folder = folder_arg(args);
                         int position = -1;
                         if (args.contains("name") && args["name"].is_string()) {
                             auto f = d.find_preset(req_str(args, "name"), folder);
                             if (!f) return ToolResult::error(f.error());
                             position = f->index;
                         } else {
                             auto p = position_arg(args);
                             if (!p) return ToolResult::error(p.error());
                             position = *p;
                         }
                         auto preset = d.read_preset(folder, position);
                         if (!preset) return ToolResult::error(preset.error());
                         auto cat = d.catalog();
                         auto v = qc::view::build(*preset, cat ? *cat : nullptr, static_cast<int>(preset->default_scene()));
                         json j = qc::view::to_json(v);
                         j["folder"] = folder;
                         j["position"] = position;
                         j["slot"] = qc::view::position_to_slot(position);
                         return ToolResult::ok_json(j);
                     }))});

    server.add_tool({"qc_current_preset",
                     "Read the LIVE grid (unsaved edits included) without side effects. Rows (1-4) and columns (1-8) count from 1 as labelled on the unit; parameter "
                     "values are normalized 0..1 with `real` in the parameter's own units when known.",
                     schema({{"scene", str("scene A-H to show values for (default: active scene)")}}),
                     with_device([](qc::QuadCortex& d, const json& args) {
                         auto preset = d.read_current_preset();
                         if (!preset) return ToolResult::error(preset.error());
                         auto scene = scene_arg(args, "scene");
                         if (!scene) {
                             auto s = d.active_scene(std::chrono::milliseconds{5000});
                             scene = s ? *s : 0;
                         }
                         auto cat = d.catalog();
                         return ToolResult::ok_json(qc::view::to_json(qc::view::build(*preset, cat ? *cat : nullptr, *scene)));
                     })});

    server.add_tool({"qc_catalog_search",
                     "Search the device's block catalog (amps, cabs, pedals, captures) by name/category. Returns model ids and "
                     "parameter layouts (index -> name/range).",
                     schema({{"query", str("substring, case-insensitive; empty lists everything")}, {"limit", integer("max results", 1, 500)}}),
                     with_device([](qc::QuadCortex& d, const json& args) {
                         auto cat = d.catalog();
                         if (!cat) return ToolResult::error(cat.error());
                         auto models = (*cat)->search(args.value("query", ""));
                         const auto limit = static_cast<std::size_t>(args.value("limit", 50));
                         json j = json::array();
                         for (const auto* m : models) {
                             if (j.size() >= limit) break;
                             j.push_back(qc::view::to_json(*m));
                         }
                         return ToolResult::ok_json(json{{"total", (*cat)->size()}, {"matches", models.size()}, {"models", j}});
                     })});

    // ── navigation ──────────────────────────────────────────────────────
    server.add_tool({"qc_switch_scene", "Switch the active scene (A-H).",
                     schema({{"scene", str("A-H or 0-7")}}, {"scene"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto s = scene_arg(args, "scene");
                         if (!s) return ToolResult::error("scene must be A-H or 0-7");
                         return ok_or_err(d.switch_scene(*s), "scene " + qc::view::scene_letter(*s));
                     }))});

    server.add_tool({"qc_set_scene_label", "Rename a scene.",
                     schema({{"scene", str("A-H")}, {"label", str("new label")}}, {"scene", "label"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto s = scene_arg(args, "scene");
                         if (!s) return ToolResult::error("scene must be A-H or 0-7");
                         return ok_or_err(d.set_scene_label(*s, args.value("label", "")));
                     }))});

    // ── grid edits ──────────────────────────────────────────────────────
    server.add_tool({"qc_set_param",
                     "Set one block parameter (row 1-4, column 1-8 — the counting the unit shows and qc_current_preset reports; "
                     "param index from qc_current_preset/qc_catalog_search). "
                     "`value` is normalized 0..1; alternatively give `real` in the parameter's units (needs the catalog). "
                     "Optional `scene` writes a per-scene value. Save afterwards with qc_save_preset to keep it.",
                     schema({{"row", integer("row 1-4, as labelled on the unit", 1, 4)}, {"column", integer("column 1-8, as labelled on the unit", 1, 8)},
                             {"param", integer("parameter index", 0, 255)}, {"value", number("normalized 0..1", 0.0, 1.0)},
                             {"real", {{"type", "number"}, {"description", "value in the parameter's own units"}}},
                             {"text", str("string value (mic/IR/capture selectors)")}, {"scene", str("A-H for a per-scene value")}},
                            {"row", "column", "param"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto at = cell_of(args);
                         if (!at) return ToolResult::error(at.error());
                         const int row = at->first, col = at->second, idx = req_int(args, "param");
                         if (args.contains("text") && args["text"].is_string())
                             return ok_or_err(d.set_param_text(row, col, idx, args["text"].get<std::string>()));
                         float value = 0.0f;
                         if (args.contains("real") && args["real"].is_number()) {
                             auto preset = d.read_current_preset();
                             auto cat = d.catalog();
                             if (!preset || !cat) return ToolResult::error("`real` needs the live preset and catalog");
                             auto v = qc::view::build(*preset, *cat, 0);
                             const auto* b = v.rows[static_cast<std::size_t>(row)].block_at(col);
                             if (!b) return ToolResult::error("no block at that cell");
                             const auto* m = (*cat)->find(static_cast<int>(b->model_id));
                             const auto* p = m ? m->parameter(idx) : nullptr;
                             if (!p) return ToolResult::error("parameter not in catalog");
                             if (p->range_is_placeholder()) return ToolResult::error("this parameter publishes a placeholder range; pass `value` 0..1");
                             value = p->to_normalized(args["real"].get<float>());
                         } else if (args.contains("value") && args["value"].is_number()) {
                             value = args["value"].get<float>();
                         } else {
                             return ToolResult::error("give `value`, `real` or `text`");
                         }
                         return ok_or_err(d.set_param(row, col, idx, value, scene_arg(args, "scene")), std::format("set row {} col {} param {} = {:.4f}", qc::view::row_to_user(row), qc::view::column_to_user(col), idx, value));
                     }))});

    server.add_tool({"qc_set_bypass", "Bypass (true) or engage (false) a block in the active scene (or `scene`).",
                     schema({{"row", integer("row 1-4, as labelled on the unit", 1, 4)}, {"column", integer("column 1-8, as labelled on the unit", 1, 8)},
                             {"bypassed", boolean("true = bypassed")}, {"scene", str("A-H")}},
                            {"row", "column", "bypassed"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto at = cell_of(args);
                         if (!at) return ToolResult::error(at.error());
                         return ok_or_err(d.set_bypass(at->first, at->second, req_bool(args, "bypassed"), scene_arg(args, "scene")));
                     }))});

    server.add_tool({"qc_set_block", "Place a block (model id from qc_catalog_search) into a cell; replaces an existing one. Verified against the device's echo (a refusal means no DSP headroom).",
                     schema({{"row", integer("row 1-4, as labelled on the unit", 1, 4)}, {"column", integer("column 1-8, as labelled on the unit", 1, 8)}, {"model_id", integer("catalog model id", 1, 1000000)}},
                            {"row", "column", "model_id"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto at = cell_of(args);
                         if (!at) return ToolResult::error(at.error());
                         return ok_or_err(d.set_block(at->first, at->second, req_u32(args, "model_id")));
                     }))});

    server.add_tool({"qc_remove_block", "Remove the block at a cell.",
                     schema({{"row", integer("row 1-4, as labelled on the unit", 1, 4)}, {"column", integer("column 1-8, as labelled on the unit", 1, 8)}}, {"row", "column"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto at = cell_of(args);
                         if (!at) return ToolResult::error(at.error());
                         return ok_or_err(d.remove_block(at->first, at->second));
                     }))});

    server.add_tool({"qc_set_routing", "Set a row's input and/or output port (ids: see qc_ports).",
                     schema({{"row", integer("row 1-4, as labelled on the unit", 1, 4)}, {"in_port", integer("input port id", 0, 14)}, {"out_port", integer("output port id", 0, 22)}}, {"row"}),
                     with_device(writable([](qc::QuadCortex& d, const json& args) {
                         auto row_index = qc::view::row_from_user(req_int(args, "row"));
                         if (!row_index) return ToolResult::error(row_index.error());
                         const int row = *row_index;
                         if (args.contains("in_port")) { if (auto r = d.set_chain_input(row, args["in_port"].get<int>()); !r) return ToolResult::error(r.error()); }
                         if (args.contains("out_port")) { if (auto r = d.set_chain_output(row, args["out_port"].get<int>()); !r) return ToolResult::error(r.error()); }
                         return ToolResult::ok("ok");
                     }))});

    server.add_tool({"qc_ports", "List input/output port ids and names.", schema({}), guarded([](const json&) {
                         json in = json::array(), out = json::array();
                         for (const auto& [id, n] : qc::view::input_ports()) in.push_back({{"id", id}, {"name", n}});
                         for (const auto& [id, n] : qc::view::output_ports()) out.push_back({{"id", id}, {"name", n}});
                         return ToolResult::ok_json(json{{"inputs", in}, {"outputs", out}});
                     })});

    server.add_tool({"qc_save_preset", "Save the grid as it is now (\"Save As\") into a user folder slot. The device may rename on name collision.",
                     schema({{"folder", str("user folder key (default My Presets)")}, {"slot", str("slot like 30A")}, {"position", integer("position", 0, 255)},
                             {"name", str("preset name")}, {"instrument", integer("0 none,1 guitar,2 bass,3 synth,4 vocal,5 other", 0, 5)}},
                            {"name"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         auto p = position_arg(args);
                         if (!p) return ToolResult::error(p.error());
                         return ok_or_err(d.save_current_preset(folder_arg(args), *p, req_str(args, "name"), args.value("instrument", 0)),
                                          std::format("saved to {} (re-list to see the final name)", qc::view::position_to_slot(*p)));
                     }))});

    // Backups liegen ausschliesslich in backups_ (Review 2026-10-15, H1/M1):
    // das Tool-Argument ist ein blanker Dateiname, nie ein Pfad.
    const std::string backup_where = "Files live only in " + backups_.dir().string() + "; give a plain file name, not a path.";
    server.add_tool({"qc_backup_save",
                     "Read the on-device LocalBackup (presets/captures/IRs/config) and write it to a NEW local file "
                     "(never overwrites). " + backup_where,
                     schema({{"name", str("plain file name, e.g. qc-2026-10-15.json")}}, {"name"}),
                     with_device(writable([this](qc::QuadCortex& d, const json& args) -> ToolResult {
                         const auto name = req_str(args, "name");
                         if (auto c = BackupStore::check_name(name); !c) return ToolResult::error(c.error());
                         auto b = d.read_backup();
                         if (!b) return ToolResult::error(b.error());
                         auto path = backups_.save(name, *b);
                         if (!path) return ToolResult::error(path.error());
                         return ToolResult::ok_json(json{{"file", path->string()}, {"bytes", b->size()}});
                     }))});

    server.add_tool({"qc_backup_restore",
                     "Restore a LocalBackup from a file saved by qc_backup_save. DESTRUCTIVE: overwrites the unit's "
                     "presets/captures/IRs. Requires confirm=true. " + backup_where,
                     schema({{"name", str("plain file name of the backup")}, {"confirm", boolean("must be true")}}, {"name"}),
                     with_device(writable([this](qc::QuadCortex& d, const json& args) -> ToolResult {
                         if (!args.value("confirm", false)) return ToolResult::error("refusing: pass confirm=true (this overwrites device content)");
                         auto data = backups_.load(req_str(args, "name"));
                         if (!data) return ToolResult::error(data.error());
                         if (!json::accept(*data)) return ToolResult::error("not a valid backup: the file is not JSON");
                         auto r = d.restore_backup(*data);
                         return r ? ToolResult::ok("backup applied") : ToolResult::error(r.error());
                     }))});

    server.add_tool({"qc_set_master_volume", "Set the master volume (0..1; the unit shows round(v*100)).",
                     schema({{"volume", number("0..1", 0.0, 1.0)}}, {"volume"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         return ok_or_err(d.set_master_volume(req_float(args, "volume")));
                     }))});

    server.add_tool({"qc_tuner",
                     "Show or hide the unit's built-in tuner. Goes out as MIDI CC#45, not over the protobuf "
                     "protocol - ShowTuner (27) is ignored by the unit (docs/evidence/82, 84). The unit does not "
                     "report its tuner state back, so this sets it rather than toggling it.",
                     schema({{"show", boolean("true = show the tuner")}}, {"show"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         const bool show = req_bool(args, "show");
                         return ok_or_err(d.set_tuner(show), show ? "tuner shown (MIDI CC#45)" : "tuner hidden (MIDI CC#45)");
                     }))});

    server.add_tool({"qc_io_meter",
                     "Measure the levels the unit reports itself (IOMeter): inputs, returns, XLR, Out 3/4, sends, "
                     "headphones, USB in/out, grid outputs. Subscribes for `seconds`, unsubscribes, and returns "
                     "last/max/mean for every channel that saw a level. The unit sends levels ONLY while a signal is "
                     "present: no messages means silence, not an error (docs/evidence/182). The first call after the "
                     "USB connection opens waits ~8 s for the first messages (the unit's start-up push, "
                     "docs/evidence/183). Values are 0..1; `db` reads them as linear amplitude, which is "
                     "UNCONFIRMED. The *_limiter fields and hp_limiter_active are reported separately: measured "
                     "constant (1.0 / true) even without clipping, meaning unknown. Read-only.",
                     schema({{"seconds", integer("measuring window in seconds (default 15)", 1, 120)}}),
                     with_device([](qc::QuadCortex& d, const json& args) -> ToolResult {
                         const int seconds = args.value("seconds", 15);
                         // Der Zustand lebt in einem shared_ptr, den der Sink per Wert haelt:
                         // emit_event ruft eine KOPIE des Sinks ausserhalb der Sperre auf, ein
                         // spaeter Aufruf darf also nichts vom Stapel dieser Funktion anfassen.
                         struct Messung {
                             std::mutex m;
                             qc::iometer::Accumulator acc;
                             std::optional<double> erste_s;
                             std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
                         };
                         auto st = std::make_shared<Messung>();
                         d.set_event_sink([st](const qc::Event& e) {
                             const auto* io = std::get_if<qc::event::IoMeter>(&e);
                             if (!io) return;
                             std::lock_guard lk{st->m};
                             if (!st->erste_s) {
                                 st->erste_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - st->start).count();
                             }
                             st->acc.add(io->levels);
                         });
                         if (auto r = d.subscribe_io_meter(true); !r) {
                             d.set_event_sink({});
                             return ToolResult::error(r.error());
                         }
                         auto auffrischen = st->start + qc::iometer::kResubscribeInterval;
                         const auto ende = st->start + std::chrono::seconds(seconds);
                         while (std::chrono::steady_clock::now() < ende && !d.lost()) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(100));
                             if (std::chrono::steady_clock::now() >= auffrischen) {
                                 (void)d.subscribe_io_meter(true);
                                 auffrischen += qc::iometer::kResubscribeInterval;
                             }
                         }
                         (void)d.subscribe_io_meter(false);
                         d.set_event_sink({});

                         std::lock_guard lk{st->m};
                         const auto& acc = st->acc;
                         json pegel = json::array();
                         json limiter = json::object();
                         for (const auto& c : qc::iometer::channels()) {
                             const auto& k = acc.stats(c.index);
                             if (c.group == qc::iometer::Group::Limiter) {
                                 limiter[std::string(c.key)] = k.last;
                                 continue;
                             }
                             if (k.max <= 0.0f) continue;
                             pegel.push_back({{"key", c.key}, {"name", c.name}, {"last", k.last}, {"max", k.max},
                                              {"mean", acc.mean(c.index)},
                                              {"max_db", qc::iometer::to_db(k.max)}});
                         }
                         if (acc.messages() > 0) {
                             limiter["hp_limiter_active_count"] = acc.hp_limiter_true();
                         }
                         json out{{"seconds", seconds}, {"messages", acc.messages()}, {"channels", pegel},
                                  {"scale", "0..1; max_db assumes linear amplitude (unconfirmed)"}};
                         if (st->erste_s) out["first_message_after_s"] = *st->erste_s;
                         if (acc.messages() > 0) out["unconfirmed_limiter_fields"] = limiter;
                         if (acc.messages() == 0) {
                             out["hint"] = "no IOMeter: the unit reports levels only while a signal is present";
                         } else if (pegel.empty()) {
                             out["hint"] = "messages arrived, but every level was 0 (the last message when a signal ends)";
                         }
                         return ToolResult::ok_json(out);
                     })});

    server.add_tool({"qc_set_tempo", "Set the current preset's tempo (40..240 bpm).",
                     schema({{"bpm", number("40..240", 40.0, 240.0)}}, {"bpm"}),
                     with_device(writable([ok_or_err](qc::QuadCortex& d, const json& args) {
                         return ok_or_err(d.set_tempo_bpm(req_float(args, "bpm")));
                     }))});
}

}  // namespace qcmcp
