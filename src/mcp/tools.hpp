// tools.hpp — MCP-Werkzeuge fuer den Quad Cortex (USB)
//
// Wer:      Martin.Lonkwitz
// Was:      Registriert alle qc_*-Tools an einem mcp::Server. Haelt die USB-
//           Verbindung (QuadCortex) lazily und serialisiert Zugriffe.
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#pragma once

#include "backup_store.hpp"
#include "mcp_stdio_server.hpp"

#include "qc/device.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace qcmcp {

struct Config {
    std::string cortex_control_version{"auto"};
    /// Schreibende Tools (qc_set_*, qc_save_*, qc_backup_*). Standard AUS
    /// (Review 2026-10-15, M2); freischalten mit --allow-writes.
    bool allow_writes{false};
    /// Ablage fuer qc_backup_save/-restore; leer = BackupStore::default_dir().
    std::filesystem::path backup_dir{};
};

class Tools {
public:
    explicit Tools(Config cfg);
    void register_all(mcp::Server& server);

private:
    std::expected<qc::QuadCortex*, std::string> device();
    void drop_device();

    Config cfg_;
    BackupStore backups_;
    std::mutex mutex_;
    std::unique_ptr<qc::QuadCortex> qc_;
};

}  // namespace qcmcp
