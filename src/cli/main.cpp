// main.cpp — qcctl: Kommandozeilenwerkzeug fuer den Quad Cortex
//
// Wer:      Martin.Lonkwitz
// Was:      Geraetesuche, Version, Ordner/Presets listen, Preset recallen,
//           Szene, Live-Grid anzeigen (Text/JSON), Parameter/Bypass setzen,
//           speichern, Katalog durchsuchen, Traffic-Monitor.
// Wann:     2026-09-04
// Warum:    Schnellster Weg, jede Protokollfunktion gegen das echte Geraet zu
//           pruefen.
// Projekt:  Ampersand
// Abhaengig: qccore
// Nutzung:  qcctl --help
// Historie: git log
#include "qc/audio.hpp"
#include "qc/device.hpp"
#include "qc/hid_transport.hpp"
#include "qc/log.hpp"
#include "qc/pitch.hpp"
#include "qc/version.hpp"
#include "qc/wav.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <print>
#include <condition_variable>
#include <cstring>
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

struct Args {
    std::string command;
    std::vector<std::string> positional;
    bool json{false};
    bool verbose{false};
    bool no_connect{false};
    bool raw{false};
    bool confirm{false};
    bool sync{false};  ///< time: set the unit's clock from this host
    std::string folder{qc::view::kUserSetlist};
    int scene{-1};
    std::string cc_version{"auto"};
    int delay_ms{0};                  ///< raw: erst nach dieser Stille senden
    std::string gesture;              ///< tap: tap|press|release|move|drag
    float to_x{-1.0F};                ///< tap: Ziel einer Wischgeste
    float to_y{-1.0F};
};

void usage() {
    std::println(R"(qcctl - Neural DSP Quad Cortex control (USB HID)

Usage: qcctl [options] <command> [args]

Commands:
  devices                         list Quad Cortex hidraw nodes
  version                         read the device version (no handshake)
  info                            handshake, then version + current position + scene
  folders [type]                  list folders (type 0 presets, 1 IRs, 2 captures)
  presets                         list presets in --folder
  recall <slot|name>              recall a preset in --folder ("28C", "218" or a name)
  current                         show the live grid (unsaved edits included)
  export [file]                   signal-flow JSON report of the live grid (all settings)
  scene [A-H|0-7]                 read or switch the active scene
  param <row> <col> <idx> <val>   set a block parameter (val 0..1)
  bypass <row> <col> <on|off>     bypass / engage a block in the active scene
  block <row> <col> <model_id>    place a block (verified against the Grid echo)
  remove <row> <col>              remove a block
  route <row> <in> <out>          set a row's input and output port (0 = empty)
  param-text <row> <col> <idx> <text>   set a text parameter (capture/IR/mic selection)
                                  rows 1-4 and columns 1-8, counted as the unit
                                  labels them and as export/current report them
  save <slot> <name> [instr]      Save As into --folder (snapshots the grid)
  delete <name>                   delete a preset by name from --folder (user folders only)
  volume [0..1]                   read or set the master volume
  tempo <bpm>                     set the preset tempo
  time [--sync]                   show this host's clock; --sync sets the unit's clock
                                  (the unit has none without a network and starts at
                                  1970, which dates backups wrongly). Run it early:
                                  measured, the unit took it while its clock was unset
  tuner <on|off>                  show/hide the unit's built-in tuner (MIDI CC#45)
  tune [seconds] [channel]        live tuner from the USB audio stream (channel 1 = dry)
  meter [seconds]                 level of all eight USB channels, from the audio stream
  iometer [seconds]               levels the unit reports itself (inputs, returns, outputs,
                                  headphones, USB in/out; --json: all raw values per line).
                                  The unit sends them ONLY while a signal is present, and
                                  only after its start-up push (~8 s after connecting)
  tune <file.wav>                 same analysis on a recording (mono float32)
  catalog [search]                list the device's block catalog
  monitor [seconds]               print every push the device sends
  screenshot [out.png]            fetch the unit's display as PNG (800x480, RemoteControl)
  uitree                          print the unit's UI tree as text
  tap <x> <y>                     touch the unit's screen at a PIXEL position (800x480);
                                  --gesture tap|press|release|move|drag, --to <x> <y> for a drag
  replay <file> [seconds]         replay a recorded host sequence in ONE session,
                                  then count what comes back (lines: <type> <hex>)
  raw <Type> <action> [seconds]   send an action (CREATE/UPDATE/DELETE/READ or hex);
                                  --delay <ms> waits before sending (the unit is busy
                                  with its seed push for the first ~11 s)
  io [--json]                     global I/O settings (input level, impedance, USB output)
  settings                        global device settings (display, MIDI, power, storage)
  coros                           show the installed CorOS version (from the version handshake; no network, nothing sent to Neural)
  backup-save <file.json>         have the unit build a LocalBackup and save it (CREATE, not READ; takes ~90 s)
  backup-restore <file.json>      restore a LocalBackup (DESTRUCTIVE; needs --confirm)

Options:
  --folder <key>   folder key (default: "/media/p4/Presets/My Presets";
                   factory: "/opt/neuraldsp/Factory Library/")
  --scene <A-H>    scene for `current` / per-scene `param`
  --json           machine-readable output
  --raw            (current) dump the BinaryPreset as protobuf text
  --verbose        debug log (every report)
  --cc <ver>       Cortex Control version to announce (default auto = the unit's zenos version)
  --version        print the tool version
)");
}

std::optional<int> parse_int(std::string_view s) {
    int v = 0;
    if (s.empty() || std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{}) return std::nullopt;
    return v;
}

// First "N.N" / "N.N.N" token in a string (used to lift the CorOS release out of
// the device's zenos version field). No regex dependency; empty if none.
std::string find_semver(std::string_view s) {
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!is_digit(s[i]) || (i > 0 && (is_digit(s[i - 1]) || s[i - 1] == '.'))) continue;
        std::size_t j = i;
        int dots = 0;
        while (j < s.size() && (is_digit(s[j]) || s[j] == '.')) {
            if (s[j] == '.') {
                if (j + 1 >= s.size() || !is_digit(s[j + 1])) break;
                ++dots;
            }
            ++j;
        }
        if (dots >= 1) return std::string(s.substr(i, j - i));
    }
    return {};
}

std::optional<float> parse_float(std::string_view s) {
    float v = 0;
    if (s.empty() || std::from_chars(s.data(), s.data() + s.size(), v).ec != std::errc{}) return std::nullopt;
    return v;
}

std::optional<int> parse_scene(std::string_view s) {
    if (s.size() == 1) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
        if (c >= 'A' && c <= 'H') return c - 'A';
    }
    auto n = parse_int(s);
    if (n && *n >= 0 && *n < 8) return n;
    return std::nullopt;
}

std::expected<Args, std::string> parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string_view s(argv[i]);
        auto next = [&](std::string_view opt) -> std::expected<std::string, std::string> {
            if (i + 1 >= argc) return std::unexpected(std::format("{} needs a value", opt));
            return std::string(argv[++i]);
        };
        if (s == "--json") a.json = true;
        else if (s == "--raw") a.raw = true;
        else if (s == "--confirm") a.confirm = true;
        else if (s == "--sync") a.sync = true;
        else if (s == "--verbose" || s == "-v") a.verbose = true;
        else if (s == "--gesture") { auto v = next(s); if (!v) return std::unexpected(v.error()); a.gesture = *v; }
        else if (s == "--to") { auto vx = next(s); if (!vx) return std::unexpected(vx.error());
                                auto vy = next(s); if (!vy) return std::unexpected(vy.error());
                                try { a.to_x = std::stof(*vx); a.to_y = std::stof(*vy); }
                                catch (...) { return std::unexpected(std::string("--to expects two numbers")); } }
        else if (s == "--delay") { auto v = next(s); if (!v) return std::unexpected(v.error());
                                   auto n = parse_int(*v); if (!n) return std::unexpected(std::string("--delay expects milliseconds"));
                                   a.delay_ms = *n; }
        else if (s == "--folder") { auto v = next(s); if (!v) return std::unexpected(v.error()); a.folder = *v; }
        else if (s == "--cc") { auto v = next(s); if (!v) return std::unexpected(v.error()); a.cc_version = *v; }
        else if (s == "--scene") {
            auto v = next(s);
            if (!v) return std::unexpected(v.error());
            auto sc = parse_scene(*v);
            if (!sc) return std::unexpected("--scene expects A-H or 0-7");
            a.scene = *sc;
        } else if (s == "--factory") a.folder = std::string(qc::view::kFactorySetlist);
        else if (s == "--help" || s == "-h") { a.command = "help"; return a; }
        else if (s == "--version") { a.command = "--version"; return a; }
        else if (s == "--") {  // Options-Ende: alles danach woertlich
            for (++i; i < argc; ++i) {
                if (a.command.empty()) a.command = std::string(argv[i]);
                else a.positional.emplace_back(argv[i]);
            }
            break;
        }
        else if (s.starts_with("--")) return std::unexpected(std::format("unknown option {}", s));
        else if (a.command.empty()) a.command = std::string(s);
        else a.positional.emplace_back(s);
    }
    if (a.command.empty()) a.command = "help";
    return a;
}

std::expected<std::vector<std::uint8_t>, std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot open {}", path));
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void print_preset(const qc::view::PresetView& v) {
    std::println("Preset: {}  (author: {}, default scene {}, shown scene {}{})", v.name, v.author,
                 qc::view::scene_letter(v.default_scene), qc::view::scene_letter(v.scene),
                 v.tempo_bpm ? std::format(", {:.0f} bpm", *v.tempo_bpm) : std::string{});
    std::print("Scenes:");
    for (int i = 0; i < qc::view::kScenes; ++i) {
        std::print(" {}={:?}", qc::view::scene_letter(i), v.scene_labels[static_cast<std::size_t>(i)]);
    }
    std::println("");
    for (const auto& r : v.rows) {
        std::println("Row {}: in={} out={}{}", qc::view::row_to_user(r.row), r.in_name.empty() ? "-" : r.in_name,
                     r.out_name.empty() ? "-" : r.out_name,
                     r.split ? std::format("  split@{} mix@{}", qc::view::column_to_user(r.split->split_column),
                                           qc::view::column_to_user(r.split->mix_column))
                             : "");
        for (const auto& b : r.blocks) {
            std::println("  [{}] {}{} ({}, id {})", qc::view::column_to_user(b.column), b.bypassed ? "~" : "", b.name,
                         b.category, b.model_id);
            for (const auto& p : b.params) {
                if (p.type == "grMeter" || p.type == "meter") continue;
                std::println("      {:>2} {:<22} {}{}", p.index, p.name, p.display_value(), p.scene_mode ? "  [scene]" : "");
            }
        }
    }
}

int run(const Args& a) {
    if (a.command == "help") { usage(); return 0; }
    if (a.command == "--version") { std::println("{}", qc::version()); return 0; }

    if (a.command == "devices") {
        auto devs = qc::hid::enumerate();
        if (a.json) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto& d : devs) j.push_back({{"path", d.path}, {"name", d.name}, {"phys", d.phys}, {"accessible", d.accessible}});
            std::println("{}", j.dump(2));
        } else {
            if (devs.empty()) std::println("no Quad Cortex found (vendor 152a product 880a)");
            for (const auto& d : devs) std::println("{}  {}  {}  {}", d.path, d.name, d.phys, d.accessible ? "rw" : "NO ACCESS (udev rule?)");
        }
        return devs.empty() ? 1 : 0;
    }

    if (a.command == "meter") {
        // Pegel aller acht USB-Kanaele, direkt aus dem Audiostrom. Braucht kein
        // HID. Das Geraet meldet Pegel ueber das Protokoll nur, solange Signal
        // anliegt (`qcctl iometer`, Beleg 182); im Audiostrom stehen sie immer
        // (Beleg 83, Beleg 147).
        // ACHTUNG: Capture::open_quad_cortex haelt zusaetzlich einen
        // Wiedergabestrom offen. Ohne den liefert das Geraet exakt Nullen -
        // sein Aufnahme-Endpunkt ist zugleich der Taktgeber der Wiedergabe.
        const int seconds = a.positional.empty() ? 20 : parse_int(a.positional[0]).value_or(20);
        auto in = qc::audio::Capture::open_quad_cortex();
        if (!in) { std::println(stderr, "error: {}", in.error()); return 1; }

        // 4800 Rahmen = 100 ms. Das ist der Takt, in dem auch das Geraet selbst
        // seine Pegel schickt, wenn es sie schickt (Beleg 92: alle 100 ms).
        constexpr std::size_t kBlock = 4800;
        std::vector<float> block(kBlock);
        const auto bis = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::println("{:>6}  {:>8}  {:>8}   {}", "ch", "rms dBFS", "peak dBFS", "name");
        while (std::chrono::steady_clock::now() < bis) {
            auto n = in->read_channel(qc::audio::kDryLeft, block);
            if (!n) { std::println(stderr, "error: {}", n.error()); return 1; }
            // Spitze nur fuer den gelesenen Kanal; fuer die uebrigen liefert
            // die Klasse den Effektivwert, und der genuegt einer Anzeige.
            double spitze = 0.0;
            for (float v : block) spitze = std::max(spitze, static_cast<double>(std::abs(v)));
            for (int c = 0; c < qc::audio::Capture::channels(); ++c) {
                const double r = in->last_rms(c);
                const auto db = [](double v) { return v > 0.0 ? 20.0 * std::log10(v) : -144.0; };
                std::println("{:>6}  {:8.2f}  {:>8}   {}", c + 1, db(r),
                             c == qc::audio::kDryLeft ? std::format("{:.2f}", db(spitze)) : "-",
                             std::string(qc::audio::channel_name(c)));
            }
            std::println("  dropouts {}", in->dropouts());
        }
        return 0;
    }

    if (a.command == "tune") {
        // Braucht kein HID: Tonhoehe und Pegel stehen im USB-Audiostrom, das
        // Geraet meldet sie nicht (docs/evidence/85). Kanal 1 fuehrt das
        // trockene Eingangssignal.
        // Endet das erste Argument auf .wav, wird die Datei ausgewertet statt
        // des Geraets. Das dient dem Vergleich mit anderen Verfahren und der
        // Fehlersuche ohne Hardware.
        if (!a.positional.empty() && a.positional[0].ends_with(".wav")) {
            auto bytes = read_file(a.positional[0]);
            if (!bytes) { std::println(stderr, "error: {}", bytes.error()); return 1; }
            auto audio = qc::wav::read(*bytes);
            if (!audio) { std::println(stderr, "error: {}", audio.error()); return 1; }
            const int rate = static_cast<int>(audio->sample_rate);
            const std::size_t window = static_cast<std::size_t>(rate / 2);
            std::println("{}: {:.1f} s at {} Hz", a.positional[0],
                         static_cast<double>(audio->samples.size()) / rate, rate);
            for (std::size_t at = 0; at + window <= audio->samples.size(); at += window) {
                const std::span<const float> block(audio->samples.data() + at, window);  // unsafe{} Ausschnitt im eigenen Puffer
                const auto reading = qc::pitch::detect(block, rate);
                const auto note = qc::pitch::nearest_note(reading.frequency_hz);
                std::println("  t={:5.1f}s  {:8.2f} Hz  {:>4} {:+6.1f} ct   quality {:.3f}  rms {:.5f}{}",
                             static_cast<double>(at) / rate, reading.frequency_hz,
                             reading.frequency_hz > 0 ? note.name : "-", note.cents, reading.aperiodicity,
                             reading.rms, reading.reliable() ? "  ok" : "");
            }
            return 0;
        }

        const int seconds = a.positional.empty() ? 30 : parse_int(a.positional[0]).value_or(30);
        const int channel = a.positional.size() > 1 ? parse_int(a.positional[1]).value_or(1) - 1 : qc::audio::kDryLeft;
        auto in = qc::audio::Capture::open_quad_cortex();
        if (!in) { std::println(stderr, "error: {}", in.error()); return 1; }

        // Ein halbe-Sekunde-Block. Kuerzer ging schief: Bei 55 Hz stecken in
        // einer Viertelsekunde nur rund 14 Perioden, und die YIN-Guete reicht
        // dann meist nicht ueber die Schwelle - die Anzeige sagte staendig
        // "no clear pitch", obwohl ein sauberer Ton anlag.
        std::vector<float> block(static_cast<std::size_t>(qc::audio::kSampleRate / 2));
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::println("listening on channel {} — play a note", channel + 1);
        while (std::chrono::steady_clock::now() < until) {
            if (auto got = in->read_channel(channel, block); !got) {
                std::println(stderr, "error: {}", got.error());
                return 1;
            }
            const auto reading = qc::pitch::detect(block, qc::audio::kSampleRate);
            const auto lost = in->dropouts();
            const double rms = in->last_rms(channel);
            const double level_db = rms > 0.0 ? 20.0 * std::log10(rms) : -120.0;
            if (!reading.reliable()) {
                // Die Guete mit anzeigen: Sie sagt, ob es am Signal liegt oder
                // knapp war - ohne sie raet man beim Justieren.
                std::print("\r  {:>6.1f} dBFS   {:<40}\033[K", level_db,
                           reading.rms > 0.0005 ? std::format("(no clear pitch, quality {:.3f})", reading.aperiodicity)
                                                : std::string("(silence)"));
            } else {
                const auto note = qc::pitch::nearest_note(reading.frequency_hz);
                // Balken von -50 bis +50 Cent; die Mitte ist die reine Stimmung.
                const int slot = std::clamp(static_cast<int>(std::lround(note.cents / 5.0)) + 10, 0, 20);
                std::string bar(21, '-');
                bar[10] = '|';
                bar[static_cast<std::size_t>(slot)] = '#';
                std::print("\r  {:>6.1f} dBFS   {:>4} {:+6.1f} ct  [{}] {:7.2f} Hz{}\033[K", level_db, note.name,
                           note.cents, bar, reading.frequency_hz,
                           lost > 0 ? std::format("  ({} dropouts)", lost) : std::string{});
            }
            std::fflush(stdout);
        }
        std::println("");
        return 0;
    }

    qc::Session::Options opts;
    opts.cortex_control_version = a.cc_version;
    auto dev = qc::QuadCortex::open(opts);
    if (!dev) { std::println(stderr, "error: {}", dev.error()); return 1; }
    auto& qc = **dev;

    if (a.command == "version") {
        auto v = qc.read_version();
        if (!v) { std::println(stderr, "error: {}", v.error()); return 1; }
        if (a.json) {
            nlohmann::json j{{"app_fw_version", v->app_fw_version}, {"serial", v->serial_number}, {"mac", v->mac_address},
                             {"zenos", v->zenos_git_hash}, {"kernel", v->linux_kernel_version}, {"uboot", v->uboot_version},
                             {"bootloader", v->bootloader_fw_version}, {"name", v->custom_name}, {"comms_version_hex", v->comms_version_hex}};
            std::println("{}", j.dump(2));
        } else {
            std::println("app_fw_version : {}", v->app_fw_version);
            std::println("serial         : {}", v->serial_number);
            std::println("mac            : {}", v->mac_address);
            std::println("zenos          : {}", v->zenos_git_hash);
            std::println("kernel         : {}", v->linux_kernel_version);
            std::println("u-boot         : {}", v->uboot_version);
            std::println("bootloader     : {}", v->bootloader_fw_version);
            std::println("name           : {}", v->custom_name);
        }
        return 0;
    }

    if (a.command == "monitor") {
        // Ohne Capture: der Sink lebt bis zum Abbau von `qc` und darf nichts
        // vom Stapel von main() anfassen.
        qc.set_event_sink([](const qc::Event& e) {
            std::visit(
                [](const auto& ev) {
                    using T = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<T, qc::event::PresetRecalled>) std::println("RecallPreset: {} (reason {})", ev.preset.name(), ev.reason);
                    else if constexpr (std::is_same_v<T, qc::event::GridDelta>) std::println("Grid delta: action {} chains {} bypass {}", ev.action, ev.sparse.chains_size(), ev.sparse.bypass_size());
                    else if constexpr (std::is_same_v<T, qc::event::SceneSelected>) std::println("Scene: {}", qc::view::scene_letter(ev.scene));
                    else if constexpr (std::is_same_v<T, qc::event::SceneLabel>) std::println("SceneLabel {}: {:?}", ev.index, ev.label);
                    else if constexpr (std::is_same_v<T, qc::event::SceneColor>) std::println("SceneColor {}: {:08x}", ev.index, ev.color);
                    else if constexpr (std::is_same_v<T, qc::event::SetlistPosition>) std::println("SetlistPosition: {} {} ({})", ev.folder_key, qc::view::position_to_slot(ev.position), ev.is_factory ? "factory" : "user");
                    else if constexpr (std::is_same_v<T, qc::event::ModeChanged>) std::println("Mode: {}", ev.mode);
                    else if constexpr (std::is_same_v<T, qc::event::MasterVolume>) std::println("MasterVolume: {:.3f}", ev.volume);
                    else if constexpr (std::is_same_v<T, qc::event::PresetDirty>) std::println("PresetDirty: {}", ev.dirty);
                    else if constexpr (std::is_same_v<T, qc::event::FolderListing>) std::println("Folder[{}]: {} ({} files)", ev.type, ev.folder.key(), ev.folder.files_size());
                    else if constexpr (std::is_same_v<T, qc::event::Tempo>) { if (ev.bpm) std::println("Tempo: {:.1f} bpm", *ev.bpm); }
                    else if constexpr (std::is_same_v<T, qc::event::IoMeter>) std::println("IOMeter: input 1 {:.3f}, input 2 {:.3f}", ev.levels.value(0), ev.levels.value(1));
                    else if constexpr (std::is_same_v<T, qc::event::Raw>) std::println("{} ({} bytes)", qc::to_string(ev.type), ev.bytes);
                    else if constexpr (std::is_same_v<T, qc::event::Lost>) std::println("LOST: {}", ev.reason);
                },
                e);
        });
    }

    if (!a.no_connect) {
        if (auto c = qc.connect(); !c) { std::println(stderr, "error: {}", c.error()); return 1; }
    }

    auto fail = [](const std::string& e) { std::println(stderr, "error: {}", e); return 1; };

    // Row and column are typed the way the unit labels them: 1 to 4, 1 to 8.
    // The conversion lives in qc::view so CLI, MCP and GUI cannot drift apart.
    auto cell = [](const std::string& row, const std::string& column) -> std::expected<std::pair<int, int>, std::string> {
        auto r = parse_int(row), c = parse_int(column);
        if (!r || !c) return std::unexpected("row and column must be whole numbers");
        auto ri = qc::view::row_from_user(*r);
        if (!ri) return std::unexpected(ri.error());
        auto ci = qc::view::column_from_user(*c);
        if (!ci) return std::unexpected(ci.error());
        return std::pair{*ri, *ci};
    };

    if (a.command == "info") {
        const auto v = qc.version();
        std::println("device   : {} (fw {}, serial {}, mac {})", v.custom_name.empty() ? "Quad Cortex" : v.custom_name,
                     v.app_fw_version, v.serial_number, v.mac_address);
        if (auto pos = qc.current_position(); pos) std::println("position : {} {}", pos->folder_key, qc::view::position_to_slot(pos->position));
        else std::println("position : ? ({})", pos.error());
        if (auto sc = qc.active_scene(); sc) std::println("scene    : {}", qc::view::scene_letter(*sc));
        if (auto m = qc.mode(); m) std::println("mode     : {}", *m);
        if (auto mv = qc.master_volume(); mv) std::println("volume   : {}", static_cast<int>(std::lround(*mv * 100)));
        if (auto d = qc.preset_dirty(); d) std::println("dirty    : {}", *d);
        const auto st = qc.session().stats();
        std::println("stats    : rx {} msgs / {} reports, tx {} msgs, undelivered {}", st.received_messages, st.received_reports, st.sent_messages, st.undelivered);
        return 0;
    }
    if (a.command == "folders") {
        const int type = a.positional.empty() ? 0 : parse_int(a.positional[0]).value_or(0);
        auto folders = qc.list_folders(type);
        if (!folders) return fail(folders.error());
        if (a.json) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto& f : *folders) j.push_back({{"key", f.key}, {"name", f.name}, {"parent", f.parent_key}, {"factory", f.is_factory}, {"files", f.file_count}});
            std::println("{}", j.dump(2));
        } else {
            for (const auto& f : *folders) std::println("{:>4}  {}  {}{}", f.file_count, f.key, f.name, f.is_factory ? "  [factory]" : "");
        }
        return 0;
    }
    if (a.command == "presets") {
        auto list = qc.list_presets(a.folder);
        if (!list) return fail(list.error());
        if (a.json) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto& e : *list) j.push_back({{"index", e.index}, {"slot", e.slot}, {"name", e.name}, {"author", e.author}, {"instrument", e.instrument}, {"key", e.key}});
            std::println("{}", j.dump(2));
        } else {
            for (const auto& e : *list) std::println("{:>4} {:<4} {:<24} {}", e.index, e.slot, e.name, e.author);
        }
        return 0;
    }
    if (a.command == "recall") {
        if (a.positional.empty()) return fail("recall needs a slot or a name");
        int position = -1;
        if (auto p = qc::view::slot_to_position(a.positional[0])) position = *p;
        else {
            auto found = qc.find_preset(a.positional[0], a.folder);
            if (!found) return fail(found.error());
            position = found->index;
        }
        auto preset = qc.read_preset(a.folder, position);
        if (!preset) return fail(preset.error());
        auto cat = qc.catalog();
        auto v = qc::view::build(*preset, cat ? *cat : nullptr, a.scene >= 0 ? a.scene : static_cast<int>(preset->default_scene()));
        if (a.json) std::println("{}", qc::view::to_json(v).dump(2));
        else print_preset(v);
        return 0;
    }
    if (a.command == "current") {
        auto preset = qc.read_current_preset();
        if (!preset) return fail(preset.error());
        if (a.raw) {
            std::println("{}", preset->DebugString());
            return 0;
        }
        int scene = a.scene;
        if (scene < 0) {
            auto sc = qc.active_scene();
            scene = sc ? *sc : 0;
        }
        auto cat = qc.catalog();
        if (!cat) std::println(stderr, "warning: no catalog ({}); names unavailable", cat.error());
        auto v = qc::view::build(*preset, cat ? *cat : nullptr, scene);
        if (a.json) std::println("{}", qc::view::to_json(v).dump(2));
        else print_preset(v);
        return 0;
    }
    if (a.command == "export") {
        auto preset = qc.read_current_preset();
        if (!preset) return fail(preset.error());
        int scene = a.scene;
        if (scene < 0) {
            auto sc = qc.active_scene();
            scene = sc ? *sc : 0;
        }
        auto cat = qc.catalog();
        if (!cat) std::println(stderr, "warning: no catalog ({}); names unavailable", cat.error());
        const auto report = qc::view::chain_report(qc::view::build(*preset, cat ? *cat : nullptr, scene)).dump(2);
        if (a.positional.empty()) {
            std::println("{}", report);
        } else {
            std::ofstream out(a.positional[0]);
            if (!out) return fail("cannot write " + a.positional[0]);
            out << report << '\n';
            std::println("wrote {}", a.positional[0]);
        }
        return 0;
    }
    if (a.command == "scene") {
        if (a.positional.empty()) {
            auto sc = qc.active_scene();
            if (!sc) return fail(sc.error());
            std::println("{}", qc::view::scene_letter(*sc));
            return 0;
        }
        auto sc = parse_scene(a.positional[0]);
        if (!sc) return fail("scene expects A-H or 0-7");
        if (auto r = qc.switch_scene(*sc); !r) return fail(r.error());
        std::println("scene {}", qc::view::scene_letter(*sc));
        return 0;
    }
    if (a.command == "tuner") {
        if (a.positional.empty()) return fail("tuner <on|off>");
        const auto& v = a.positional[0];
        const bool on = (v == "on" || v == "1" || v == "true");
        if (!on && v != "off" && v != "0" && v != "false") return fail("tuner expects on|off");
        if (auto r = qc.set_tuner(on); !r) return fail(r.error());
        // Goes out over MIDI CC#45, not the protobuf protocol (docs/evidence/84).
        // The unit does not report its tuner state back, so this says what was
        // asked for, not what the screen shows.
        std::println("tuner {} (MIDI CC#45 — the unit does not report its tuner state back)", on ? "on" : "off");
        return 0;
    }
    if (a.command == "param") {
        if (a.positional.size() != 4) return fail("param <row 1-4> <col 1-8> <idx> <value>");
        auto at = cell(a.positional[0], a.positional[1]);
        if (!at) return fail(at.error());
        auto idx = parse_int(a.positional[2]);
        auto val = parse_float(a.positional[3]);
        if (!idx || !val) return fail("param expects a whole-number index and a float value");
        std::optional<int> scene;
        if (a.scene >= 0) scene = a.scene;
        if (auto r = qc.set_param(at->first, at->second, *idx, *val, scene); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "bypass") {
        if (a.positional.size() != 3) return fail("bypass <row 1-4> <col 1-8> <on|off>");
        auto at = cell(a.positional[0], a.positional[1]);
        if (!at) return fail(at.error());
        const bool on = a.positional[2] == "on" || a.positional[2] == "1" || a.positional[2] == "true";
        std::optional<int> scene;
        if (a.scene >= 0) scene = a.scene;
        if (auto r = qc.set_bypass(at->first, at->second, on, scene); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "block") {
        if (a.positional.size() != 3) return fail("block <row 1-4> <col 1-8> <model_id>");
        auto at = cell(a.positional[0], a.positional[1]);
        if (!at) return fail(at.error());
        auto id = parse_int(a.positional[2]);
        if (!id) return fail("block expects a whole-number model id");
        if (auto r = qc.set_block(at->first, at->second, static_cast<std::uint32_t>(*id)); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "route") {
        if (a.positional.size() != 3) return fail("route <row 1-4> <in-port> <out-port>  (0 = empty)");
        auto row = parse_int(a.positional[0]);
        auto in = parse_int(a.positional[1]);
        auto out = parse_int(a.positional[2]);
        if (!row || !in || !out) return fail("route expects whole numbers");
        auto ri = qc::view::row_from_user(*row);
        if (!ri) return fail(ri.error());
        if (auto r = qc.set_chain_input(*ri, *in); !r) return fail(r.error());
        if (auto r = qc.set_chain_output(*ri, *out); !r) return fail(r.error());
        std::println("row {} : in {} out {}", *row, *in, *out);
        return 0;
    }
    if (a.command == "param-text") {
        // Fuer Parameter, die einen Namen tragen statt einer Zahl - etwa die
        // Auswahl einer Capture-Datei oder eines Mikrofons.
        if (a.positional.size() != 4) return fail("param-text <row 1-4> <col 1-8> <idx> <text>");
        auto at = cell(a.positional[0], a.positional[1]);
        if (!at) return fail(at.error());
        auto idx = parse_int(a.positional[2]);
        if (!idx) return fail("param-text expects a whole-number index");
        if (auto r = qc.set_param_text(at->first, at->second, *idx, a.positional[3]); !r) return fail(r.error());
        std::println("ok (the unit does not confirm string parameters — read back with `current`)");
        return 0;
    }
    if (a.command == "remove") {
        if (a.positional.size() != 2) return fail("remove <row 1-4> <col 1-8>");
        auto at = cell(a.positional[0], a.positional[1]);
        if (!at) return fail(at.error());
        if (auto r = qc.remove_block(at->first, at->second); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "save") {
        if (a.positional.size() < 2) return fail("save <slot> <name> [instrument]");
        auto pos = qc::view::slot_to_position(a.positional[0]);
        if (!pos) return fail(pos.error());
        const int instrument = a.positional.size() > 2 ? parse_int(a.positional[2]).value_or(0) : 0;
        if (auto r = qc.save_current_preset(a.folder, *pos, a.positional[1], instrument); !r) return fail(r.error());
        std::println("saved to {} {} (the device may rename on collision)", a.folder, qc::view::position_to_slot(*pos));
        return 0;
    }
    if (a.command == "delete") {
        if (a.positional.empty()) return fail("delete <name>");
        if (auto r = qc.delete_preset(a.folder, a.positional[0]); !r) return fail(r.error());
        std::println("delete sent for \"{}\" in {} (listings lag a few seconds)", a.positional[0], a.folder);
        return 0;
    }
    if (a.command == "backup-save") {
        if (a.positional.empty()) return fail("backup-save <file.json>");
        auto b = qc.read_backup();
        if (!b) return fail(b.error());
        // Nutzerdaten: Datei nur fuer den Besitzer lesbar (0600), auch wenn sie
        // schon existierte (Re-Review 2026-10-15). Der Pfad ist bei der CLI
        // bewusst frei waehlbar.
        const std::string& file = a.positional[0];
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return fail(std::format("cannot write {}: {}", file, std::strerror(errno)));
        const bool mode_ok = ::fchmod(fd, 0600) == 0;
        std::size_t done = 0;
        bool write_ok = mode_ok;
        while (write_ok && done < b->size()) {
            const ssize_t n = ::write(fd, b->data() + done, b->size() - done);
            if (n < 0 && errno == EINTR) continue;
            write_ok = n > 0;
            if (write_ok) done += static_cast<std::size_t>(n);
        }
        const int saved_errno = errno;
        const bool close_ok = ::close(fd) == 0;
        if (!write_ok || !close_ok) return fail(std::format("cannot write {}: {}", file, std::strerror(saved_errno)));
        std::println("saved LocalBackup ({} bytes, mode 0600) to {}", b->size(), file);
        return 0;
    }
    if (a.command == "backup-restore") {
        if (a.positional.empty()) return fail("backup-restore <file.json> [--confirm]  (DESTRUCTIVE: overwrites presets/captures/IRs)");
        if (!a.confirm) return fail("refusing without --confirm: this overwrites the unit's presets/captures/IRs from the backup file");
        auto data = read_file(a.positional[0]);
        if (!data) return fail(data.error());
        const std::string_view json(reinterpret_cast<const char*>(data->data()), data->size());  // unsafe{}
        std::println("restoring {} bytes — do not disconnect …", data->size());
        if (auto r = qc.restore_backup(json); !r) return fail(r.error());
        std::println("device reported the backup was applied");
        return 0;
    }
    if (a.command == "volume") {
        if (a.positional.empty()) {
            auto v = qc.master_volume();
            if (!v) return fail(v.error());
            std::println("{:.3f}", *v);
            return 0;
        }
        auto v = parse_float(a.positional[0]);
        if (!v) return fail("volume expects 0..1");
        if (auto r = qc.set_master_volume(*v); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "tempo") {
        if (a.positional.empty()) return fail("tempo <bpm>");
        auto v = parse_float(a.positional[0]);
        if (!v) return fail("tempo expects a number");
        if (auto r = qc.set_tempo_bpm(*v); !r) return fail(r.error());
        std::println("ok");
        return 0;
    }
    if (a.command == "time") {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        const auto sec = std::chrono::time_point_cast<std::chrono::seconds>(now);
        const std::string iso = std::format("{:%Y-%m-%dT%H:%M:%SZ}", sec);
        if (!a.sync) {
            std::println("host  : {}  ({} ms since epoch)", iso, ms);
            std::println("unit  : not readable - the unit answers no SystemTimeSync READ.");
            std::println("");
            std::println("Without a network the unit has no clock: it starts at 1970 and dates");
            std::println("backups wrongly. `qcctl time --sync` sends the host clock, as the");
            std::println("official Cortex Control does on connect.");
            std::println("Measured 2026-09-17 (docs/evidence/118): it was applied while the");
            std::println("unit's clock was UNSET; two later attempts changed nothing. So run it");
            std::println("early, right after the unit booted.");
            return 0;
        }
        if (auto r = qc.sync_time(static_cast<std::uint64_t>(ms)); !r) return fail(r.error());
        std::println("sent SystemTimeSync {}  ({} ms since epoch)", iso, ms);
        std::println("The unit does not acknowledge it, and it may ignore the value once its");
        std::println("clock is set (see docs/evidence/118).");
        return 0;
    }
    if (a.command == "catalog") {
        auto cat = qc.catalog();
        if (!cat) return fail(cat.error());
        const std::string needle = a.positional.empty() ? "" : a.positional[0];
        auto models = (*cat)->search(needle);
        if (a.json) {
            nlohmann::json j = nlohmann::json::array();
            for (const auto* m : models) j.push_back(qc::view::to_json(*m));
            std::println("{}", j.dump(2));
        } else {
            std::println("{} models ({} matching)", (*cat)->size(), models.size());
            for (const auto* m : models) {
                std::println("{:>6}  {:<32} {:<24} {}{}", m->id, m->name, m->category, m->based_on, m->superseded ? "  [superseded]" : "");
                if (!needle.empty()) {
                    for (const auto& p : m->parameters) std::println("          {:>2} {:<22} {} .. {} {} {}", p.index, p.name, p.minimum, p.maximum, p.units, p.type);
                }
            }
        }
        return 0;
    }
    if (a.command == "iometer") {
        // Pegel, die das Geraet selbst meldet (IOMeter). Gemessen (Beleg 182):
        // rund 8,6 Meldungen je Sekunde, solange am Eingang Signal anliegt;
        // endet es, kommt eine letzte Meldung mit Pegel 0, dann Stille. Eine
        // stille Zeile ist also kein Fehler. Die dB-Spalten rechnen "als ob
        // linear" - die Skala ist nicht bestaetigt (include/qc/io_meter.hpp).
        // Die ersten Meldungen kommen erst nach dem Seed-Push des Geraets,
        // gemessen rund 8 s nach dem Verbinden, dann gebuendelt (Beleg 183).
        const int seconds = a.positional.empty() ? 20 : parse_int(a.positional[0]).value_or(20);
        // Spalte = {Kopf, Kanal, zweiter Kanal eines Paars (Maximum) oder -1}.
        struct Spalte { const char* kopf; std::size_t l; int r; };
        static constexpr std::array<Spalte, 18> kSpalten{{
            {"in1", 0, -1}, {"in2", 1, -1}, {"ret1", 2, -1}, {"ret2", 3, -1}, {"xlr1", 4, -1}, {"xlr2", 6, -1},
            {"out3", 8, -1}, {"out4", 10, -1}, {"send1", 12, -1}, {"send2", 13, -1}, {"hp", 14, 15},
            {"usbi1", 24, 25}, {"usbi2", 26, 27}, {"usbi3", 28, 29}, {"usbi4", 30, 31},
            {"usbo1", 16, 17}, {"usbo2", 18, 19}, {"usbo3", 20, 21}}};
        const auto kanaele = qc::iometer::channels();
        const auto start = std::chrono::steady_clock::now();
        // Der Zustand lebt in einem shared_ptr, den der Sink per Wert haelt:
        // emit_event ruft eine KOPIE des Sinks ausserhalb der Sperre auf, ein
        // spaeter Aufruf nach set_event_sink({}) darf nichts vom Stapel anfassen.
        struct Ausgabe {
            std::mutex m;
            std::uint64_t meldungen{0};
            std::uint64_t unlesbar{0};  ///< IOMeter, das nicht als IOMeterMessage aufging
        };
        auto st = std::make_shared<Ausgabe>();
        const bool als_json = a.json;
        qc.set_event_sink([st, start, kanaele, als_json](const qc::Event& e) {
            if (const auto* roh = std::get_if<qc::event::Raw>(&e); roh && roh->type == qc::MessageType::IOMeter) {
                std::lock_guard lk{st->m};
                ++st->unlesbar;
                return;
            }
            const auto* io = std::get_if<qc::event::IoMeter>(&e);
            if (!io) return;
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard lk{st->m};
            ++st->meldungen;
            if (als_json) {
                std::string zeile = std::format(R"({{"t":{:.3f},"hp_limiter_active":{})", t, io->levels.hp_limiter_active);
                for (const auto& k : kanaele) zeile += std::format(R"(,"{}":{:.6g})", k.key, io->levels.value(k.index));
                std::println("{}}}", zeile);
                return;
            }
            std::string zeile = std::format("{:7.2f}", t);
            for (const auto& sp : kSpalten) {
                float v = io->levels.value(sp.l);
                if (sp.r >= 0) v = std::max(v, io->levels.value(static_cast<std::size_t>(sp.r)));
                zeile += v > 0.0f ? std::format(" {:6.1f}", qc::iometer::to_db(v)) : std::string("      -");
            }
            std::println("{}", zeile);
        });
        if (!a.json) {
            std::string kopf = std::format("{:>7}", "t/s");
            for (const auto& sp : kSpalten) kopf += std::format(" {:>6}", sp.kopf);
            std::println("{}   (dB, presumed linear scale)", kopf);
        }
        if (auto r = qc.subscribe_io_meter(true); !r) return fail(r.error());
        auto auffrischen = start + qc::iometer::kResubscribeInterval;
        const auto ende = start + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < ende && !qc.lost()) {
            std::this_thread::sleep_for(100ms);
            if (std::chrono::steady_clock::now() >= auffrischen) {
                if (auto r = qc.subscribe_io_meter(true); !r) return fail(r.error());
                auffrischen += qc::iometer::kResubscribeInterval;
            }
        }
        (void)qc.subscribe_io_meter(false);
        qc.set_event_sink({});
        std::lock_guard lk{st->m};
        if (st->unlesbar > 0) std::println(stderr, "warning: {} IOMeter message(s) did not parse", st->unlesbar);
        if (als_json) return 0;
        if (st->meldungen == 0) {
            std::println("no IOMeter in {} s - the unit reports levels only while a signal is present", seconds);
        } else {
            std::println("{} IOMeter messages in {} s", st->meldungen, seconds);
        }
        return 0;
    }

    if (a.command == "raw") {
        if (a.positional.size() < 2) return fail("raw <Type> <CREATE|UPDATE|DELETE|READ|hex-bytes> [seconds]");
        auto type = qc::parse_message_type(a.positional[0]);
        if (!type) return fail("unknown message type " + a.positional[0]);
        std::vector<std::uint8_t> payload;
        const std::string& act = a.positional[1];
        if (act == "CREATE") payload = {};
        else if (act == "UPDATE") payload = {0x08, 0x01};
        else if (act == "DELETE") payload = {0x08, 0x02};
        else if (act == "READ") payload = {0x08, 0x03};
        else {
            for (std::size_t i = 0; i + 1 < act.size(); i += 2) payload.push_back(static_cast<std::uint8_t>(std::stoul(act.substr(i, 2), nullptr, 16)));
        }
        const int seconds = a.positional.size() > 2 ? parse_int(a.positional[2]).value_or(10) : 10;
        qc.session().set_listener([](const qc::Inbound& in) {
            std::println("{:>7.3f}s  rx {} ({} bytes{}{}): {}", 0.0, qc::to_string(in.type), in.payload.size(),
                         in.encrypted ? ", enc" : "", in.decrypt_failed ? ", UNDECRYPTED" : "",
                         qc::framing::hex(in.payload, in.payload.size() <= 160 ? 0 : 48));
        });
        if (a.delay_ms > 0) {
            // Das Original schickt seine Abonnements erst, wenn der Seed-Push
            // durch ist (gemessen: 10,4 s nach dem Verbinden, docs/evidence/92).
            // Wer sofort sendet, sendet mitten in den Push hinein.
            std::println("waiting {} ms before sending", a.delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(a.delay_ms));
        }
        if (auto r = qc.session().send_raw(*type, payload); !r) return fail(r.error());
        std::println("sent {} {} - listening {} s", a.positional[0], act, seconds);
        for (int i = 0; i < seconds * 10 && !qc.lost(); ++i) std::this_thread::sleep_for(100ms);
        return 0;
    }
    if (a.command == "screenshot" || a.command == "uitree") {
        // RemoteControl (72) aus CorOS 4.1.0, zurueckgewonnen in
        // docs/evidence/93. Gefragt wird mit action=READ und einem LEEREN
        // Unterfeld - `screenshot` (4) oder `graphics_tree` (5). Genau diese
        // Form antwortet; CREATE mit request_id, wie bei den Messwerten, tut
        // es hier nicht.
        const bool bild = a.command == "screenshot";
        const std::uint8_t feld = bild ? 0x22 : 0x2a;
        const std::vector<std::uint8_t> payload{0x08, 0x03, feld, 0x00};

        std::vector<std::uint8_t> antwort;
        std::mutex m;
        std::condition_variable da;
        qc.session().set_listener([&](const qc::Inbound& in) {
            if (in.type != qc::MessageType::RemoteControl) return;
            {
                std::lock_guard lk{m};
                if (!antwort.empty()) return;
                antwort = in.payload;
            }
            da.notify_one();
        });
        if (auto r = qc.session().send_raw(qc::MessageType::RemoteControl, payload); !r) return fail(r.error());

        {
            std::unique_lock lk{m};
            // Grosszuegig: das Geraet schiebt nach dem Verbinden erst seinen Seed
            // durch und antwortet darauf erst danach (docs/evidence/78).
            const bool da_ist = da.wait_for(lk, 30s, [&] { return !antwort.empty(); });
            lk.unlock();
            // Abmelden, BEVOR antwort/m/da sterben; set_listener wartet einen
            // laufenden Aufruf ab (der braucht m - deshalb erst entsperren).
            qc.session().set_listener({});
            if (!da_ist) return fail("the unit did not answer RemoteControl within 30 s");
        }

        // Aus der Antwort das innere Feld schaelen: aussen RemoteControlMessage,
        // darin screenshot/graphics_tree, darin payload (Feld 1). Von Hand,
        // weil unser erzeugtes Protobuf noch die Fassung 4.0.1 ist und diese
        // Nachricht dort nicht kennt.
        auto varint = [](std::span<const std::uint8_t> b, std::size_t& i) -> std::uint64_t {
            std::uint64_t v = 0;
            int s = 0;
            while (i < b.size() && s < 64) {
                const std::uint8_t c = b[i++];
                v |= static_cast<std::uint64_t>(c & 0x7F) << s;
                if ((c & 0x80) == 0) break;
                s += 7;
            }
            return v;
        };
        auto feld_suchen = [&](std::span<const std::uint8_t> b, int gesucht)
            -> std::span<const std::uint8_t> {
            std::size_t i = 0;
            while (i < b.size()) {
                const std::uint64_t kopf = varint(b, i);
                const int nr = static_cast<int>(kopf >> 3);
                const int art = static_cast<int>(kopf & 7);
                if (art == 2) {
                    const std::uint64_t len = varint(b, i);
                    if (i + len > b.size()) break;
                    if (nr == gesucht) return b.subspan(i, len);
                    i += len;
                } else if (art == 0) {
                    (void)varint(b, i);
                } else {
                    break;
                }
            }
            return {};
        };

        const auto innen = feld_suchen(antwort, bild ? 4 : 5);
        if (innen.empty()) return fail("no " + std::string(bild ? "screenshot" : "graphics_tree") + " in the reply");
        const auto inhalt = feld_suchen(innen, 1);
        if (inhalt.empty()) return fail("the reply carried an empty payload");

        if (!bild) {
            std::print("{}", std::string_view(reinterpret_cast<const char*>(inhalt.data()), inhalt.size()));
            return 0;
        }
        const std::string ziel = a.positional.empty() ? "quad-cortex-screen.png" : a.positional[0];
        std::ofstream out(ziel, std::ios::binary);
        if (!out) return fail("cannot write " + ziel);
        out.write(reinterpret_cast<const char*>(inhalt.data()), static_cast<std::streamsize>(inhalt.size()));
        std::println("{} ({} bytes)", ziel, inhalt.size());
        return 0;
    }
    // Aufbau einer RemoteControlMouse-Nachricht fuer `tap`; nur eine Stelle
    // kennt das Format.
    auto maus_payload = [](float x, float y, int geste, float to_x, float to_y) {
        std::vector<std::uint8_t> maus;
        auto f32 = [&maus](std::uint8_t tag, float v) {
            maus.push_back(tag);
            std::uint32_t bits{};
            std::memcpy(&bits, &v, sizeof bits);
            for (int i = 0; i < 4; ++i) maus.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
        };
        f32(0x0D, x);
        f32(0x15, y);
        maus.push_back(0x18);
        maus.push_back(static_cast<std::uint8_t>(geste));
        if (to_x >= 0.0F) f32(0x25, to_x);
        if (to_y >= 0.0F) f32(0x2D, to_y);
        std::vector<std::uint8_t> payload{0x08, 0x01, 0x1A, static_cast<std::uint8_t>(maus.size())};
        payload.insert(payload.end(), maus.begin(), maus.end());
        return payload;
    };

    if (a.command == "tap") {
        // RemoteControlMouse (docs/evidence/95). Gemessen: die Koordinaten sind
        // BILDPUNKTE des 800x480-Displays, keine normierten Werte, und nur
        // `action=UPDATE` wirkt - READ und CREATE laesst das Geraet fallen.
        // Eine Antwort kommt nie; ob etwas geschah, sagt `screenshot`.
        if (a.positional.size() < 2) return fail("tap <x> <y> [--to <x> <y>] [--gesture tap|press|release|move|drag]");
        auto zahl = [](const std::string& s) -> std::optional<float> {
            try { return std::stof(s); } catch (...) { return std::nullopt; }
        };
        const auto x = zahl(a.positional[0]);
        const auto y = zahl(a.positional[1]);
        if (!x || !y) return fail("tap expects numbers");

        static const std::array<std::pair<std::string_view, int>, 5> kGesten{{
            {"press", 0}, {"release", 1}, {"move", 2}, {"tap", 3}, {"drag", 4}}};
        int geste = 3;
        if (!a.gesture.empty()) {
            const auto it = std::find_if(kGesten.begin(), kGesten.end(),
                                         [&](const auto& g) { return g.first == a.gesture; });
            if (it == kGesten.end()) return fail("unknown gesture " + a.gesture);
            geste = it->second;
        }

        const auto payload = maus_payload(*x, *y, geste, a.to_x, a.to_y);
        if (auto r = qc.session().send_raw(qc::MessageType::RemoteControl, payload); !r) return fail(r.error());
        std::println("sent {} at {} {} (the unit does not acknowledge — check with `screenshot`)",
                     a.gesture.empty() ? "tap" : a.gesture, *x, *y);
        std::this_thread::sleep_for(300ms);
        return 0;
    }
    if (a.command == "replay") {
        // Eine aufgezeichnete Host-Sequenz in EINER Sitzung nachspielen. Gedacht
        // fuer Referenzmessungen gegen das Original: Manche Antworten des
        // Geraets haengen offenbar nicht an einer einzelnen Nachricht, sondern
        // daran, was der Host vorher getan hat (docs/evidence/92).
        if (a.positional.empty()) return fail("replay <datei> [seconds]  (Zeilen: <typ> <hex>)");
        std::ifstream in(a.positional[0]);
        if (!in) return fail("cannot open " + a.positional[0]);
        struct Schritt { qc::MessageType type; std::vector<std::uint8_t> payload; int abstand_ms; };
        std::vector<Schritt> schritte;
        std::string zeile;
        while (std::getline(in, zeile)) {
            if (zeile.empty() || zeile.front() == '#') continue;
            std::istringstream zs(zeile);
            int nummer{};
            std::string hex;
            int abstand_ms = 0;
            if (!(zs >> nummer >> hex)) continue;
            zs >> abstand_ms;   // dritte Spalte ist optional
            std::vector<std::uint8_t> payload;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
                payload.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            schritte.push_back({static_cast<qc::MessageType>(nummer), std::move(payload), abstand_ms});
        }
        const int seconds = a.positional.size() > 1 ? parse_int(a.positional[1]).value_or(20) : 20;
        std::map<qc::MessageType, int> gezaehlt;
        qc.session().set_listener([&gezaehlt](const qc::Inbound& in2) { ++gezaehlt[in2.type]; });
        std::println("replaying {} messages, then listening {} s", schritte.size(), seconds);
        for (const auto& s : schritte) {
            // Eine dritte Spalte gibt den Abstand zur vorigen Nachricht wieder.
            // Ohne sie bleibt es bei 20 ms; das Original haelt teils Sekunden
            // Pause, und ob das eine Rolle spielt, ist genau die Frage.
            std::this_thread::sleep_for(std::chrono::milliseconds(s.abstand_ms > 0 ? s.abstand_ms : 20));
            if (auto r = qc.session().send_raw(s.type, s.payload); !r)
                std::println("  {} : {}", qc::to_string(s.type), r.error());
        }
        for (int i = 0; i < seconds * 10 && !qc.lost(); ++i) std::this_thread::sleep_for(100ms);
        // Erst abmelden, dann lesen: der Listener zaehlt im RX-Thread ohne
        // Sperre; nach set_listener({}) laeuft er garantiert nicht mehr.
        qc.session().set_listener({});
        std::println("received:");
        for (const auto& [typ, n] : gezaehlt)
            std::println("  {:<24} {:>6}", qc::to_string(typ), n);
        return 0;
    }
    if (a.command == "io") {
        // Die globalen Einstellungen gelten presetuebergreifend und erklaeren
        // manches, was am Grid nicht zu sehen ist - etwa ein leises Signal, weil
        // der Eingangspegel niedrig steht.
        auto s = qc.io_settings();
        if (!s) return fail(s.error());
        if (a.json) {
            nlohmann::json j;
            auto& ins = j["inputs"] = nlohmann::json::array();
            for (const auto& p : s->in_port()) {
                ins.push_back({{"port", p.input_port_id()},
                               {"level", p.has_level() ? nlohmann::json(p.level()) : nlohmann::json(nullptr)},
                               {"z_mode", p.has_input_zmode() ? nlohmann::json(p.input_zmode()) : nlohmann::json(nullptr)},
                               {"plugged", p.has_plugged() ? nlohmann::json(p.plugged()) : nlohmann::json(nullptr)}});
            }
            if (s->has_usb_port()) {
                const auto& u = s->usb_port();
                j["usb"] = {{"level", u.has_level() ? nlohmann::json(u.level()) : nlohmann::json(nullptr)},
                            {"dry_wet", u.has_dry_wet() ? nlohmann::json(u.dry_wet()) : nlohmann::json(nullptr)},
                            {"plugged", u.has_plugged() ? nlohmann::json(u.plugged()) : nlohmann::json(nullptr)}};
            }
            std::println("{}", j.dump(2));
            return 0;
        }
        std::println("inputs:");
        for (const auto& p : s->in_port()) {
            std::println("  {} {:<10} level {:>6}  z-mode {:>6}  {}", p.input_port_id(),
                         qc::view::input_port_name(static_cast<int>(p.input_port_id())),
                         p.has_level() ? std::format("{:.3f}", p.level()) : "-",
                         p.has_input_zmode() ? std::format("{:.3f}", p.input_zmode()) : "-",
                         p.has_plugged() ? (p.plugged() ? "plugged" : "not plugged") : "");
        }
        if (s->has_usb_port()) {
            const auto& u = s->usb_port();
            std::println("usb: level {}  dry/wet {}  {}",
                         u.has_level() ? std::format("{:.3f}", u.level()) : "-",
                         u.has_dry_wet() ? std::format("{:.3f}", u.dry_wet()) : "-",
                         u.has_plugged() ? (u.plugged() ? "plugged" : "not plugged") : "");
        }
        return 0;
    }
    if (a.command == "settings") {
        auto g = qc.general_settings();
        if (!g) return fail(g.error());
        const auto& s = *g;
        auto ja = [](bool v) { return v ? "on" : "off"; };
        std::println("display:  screen {}%  led {}%  dimmed led {}%  locked {}",
                     s.has_screen_brightness() ? std::to_string(s.screen_brightness()) : "-",
                     s.has_led_brightness() ? std::to_string(s.led_brightness()) : "-",
                     s.has_dimmed_led_brightness() ? std::to_string(s.dimmed_led_brightness()) : "-",
                     s.has_lock_screen_and_volume_knob() ? ja(s.lock_screen_and_volume_knob()) : "-");
        std::println("midi:     channel {}  over usb {}  internal clock {}  ignore duplicate PC {}",
                     s.has_midi_channel() ? std::to_string(s.midi_channel()) : "-",
                     s.has_midi_over_usb() ? ja(s.midi_over_usb()) : "-",
                     s.has_internal_midi_clock_enabled() ? ja(s.internal_midi_clock_enabled()) : "-",
                     s.has_ignore_duplicate_pc() ? ja(s.ignore_duplicate_pc()) : "-");
        std::println("handling: stomp auto-assign {}  swap tempo/tuner {}  dynamic delay compensation {}",
                     s.has_stomp_mode_auto_assign() ? ja(s.stomp_mode_auto_assign()) : "-",
                     s.has_swap_tempo_tuner_access() ? ja(s.swap_tempo_tuner_access()) : "-",
                     s.has_enable_dynamic_delay_compensation() ? ja(s.enable_dynamic_delay_compensation()) : "-");
        if (s.has_total_disk_space() && s.has_available_disk_space()) {
            const double total = static_cast<double>(s.total_disk_space());
            const double free = static_cast<double>(s.available_disk_space());
            std::println("storage:  {:.1f} of {:.1f} GB free ({:.0f}% used)", free, total,
                         total > 0 ? (1.0 - free / total) * 100.0 : 0.0);
        }
        if (s.has_cloud_endpoint()) std::println("cloud:    {}", s.cloud_endpoint());
        if (s.has_disable_internet_connection_check())
            std::println("          internet check {}", ja(!s.disable_internet_connection_check()));
        return 0;
    }
    if (a.command == "coros") {
        // The device reports its ZenOS/CorOS release in the version handshake
        // (the zenos field; the unit itself carries e.g. "4.1.0"). No update
        // check, no network, nothing leaves the host — it is just read off the
        // handshake the session already performed.
        auto v = qc.read_version();
        if (!v) return fail(v.error());
        const std::string& z = v->zenos_git_hash;
        const std::string ver = find_semver(z);  // dotted-number release, "" if it is a git hash
        if (a.json) {
            nlohmann::json j{{"coros_version", ver}, {"zenos", z}, {"app_fw_version", v->app_fw_version},
                             {"serial", v->serial_number}};
            std::println("{}", j.dump(2));
        } else if (!ver.empty()) {
            std::println("CorOS {}", ver);
        } else {
            std::println("CorOS version not reported as a release number; zenos = {}", z.empty() ? "(unknown)" : z);
        }
        return 0;
    }
    if (a.command == "monitor") {
        const int seconds = a.positional.empty() ? 30 : parse_int(a.positional[0]).value_or(30);
        std::println("monitoring for {} s ...", seconds);
        for (int i = 0; i < seconds * 10 && !qc.lost(); ++i) std::this_thread::sleep_for(100ms);
        return 0;
    }
    std::println(stderr, "unknown command '{}'; see --help", a.command);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);
    if (!args) {
        std::println(stderr, "error: {}", args.error());
        return 2;
    }
    qc::log::set_level(args->verbose ? qc::log::Level::Debug : qc::log::Level::Info);
    int rc = 0;
    try {
        rc = run(*args);
    } catch (const std::exception& e) {
        std::println(stderr, "fatal: {}", e.what());
        rc = 1;
    }
    return rc;
}
