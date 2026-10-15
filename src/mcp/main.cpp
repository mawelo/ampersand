// main.cpp — qc-mcp: MCP-Server (stdio) zur Steuerung des Quad Cortex
//
// Wer:      Martin.Lonkwitz
// Was:      Startet den JSON-RPC-Server auf stdin/stdout; Log geht auf stderr.
// Wann:     2026-09-04
// Warum:    Ein gesonderter MCP-Server fuer die USB-Steuerung.
// Projekt:  Ampersand
// Nutzung:  mcp add qc -- ~/.local/bin/qc-mcp [--allow-writes] [--backup-dir <dir>]
//           Ohne --allow-writes nur lesend (Review 2026-10-15, M2).
// Historie: git log
#include "mcp_stdio_server.hpp"
#include "tools.hpp"

#include "qc/log.hpp"
#include "qc/version.hpp"

#include <print>
#include <string_view>

int main(int argc, char** argv) {
    qcmcp::Config cfg;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--cc" && i + 1 < argc) cfg.cortex_control_version = argv[++i];
        else if (a == "--read-only") cfg.allow_writes = false;  // Standard; bleibt fuer alte Aufrufe
        else if (a == "--allow-writes") cfg.allow_writes = true;
        else if (a == "--backup-dir" && i + 1 < argc) {
            cfg.backup_dir = argv[++i];
            if (!cfg.backup_dir.is_absolute()) {
                std::println(stderr, "qc-mcp: --backup-dir needs an absolute path");
                return 2;
            }
        }
        else if (a == "--verbose") verbose = true;
        else if (a == "--version") { std::println("{}", qc::version()); return 0; }
        else if (a == "--help" || a == "-h") {
            std::println("qc-mcp [--cc <version>] [--allow-writes] [--backup-dir <dir>] [--verbose]\n"
                         "MCP server over stdio for the Neural DSP Quad Cortex.\n"
                         "Read-only unless --allow-writes is given. Backups: {} (or --backup-dir).\n{}",
                         qcmcp::BackupStore::default_dir().string(), qc::version());
            return 0;
        }
    }
    qc::log::set_level(verbose ? qc::log::Level::Debug : qc::log::Level::Warn);
    mcp::Server server("qc-mcp", std::string(qc::version()),
                       "Tools to inspect and control a Neural DSP Quad Cortex over USB (qc_*). "
                       "Rows (1-4) and columns (1-8) count from 1, as labelled on the unit; "
                       "parameter values are normalized 0..1. Grid edits are live and must be saved with qc_save_preset.");
    qcmcp::Tools tools(cfg);
    tools.register_all(server);
    return server.run();
}
