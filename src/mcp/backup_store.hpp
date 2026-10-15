// backup_store.hpp — fester, abgeschotteter Ablageort fuer LocalBackups (qc-mcp)
//
// Wer:      Martin.Lonkwitz
// Was:      Liest und schreibt Backup-Dateien ausschliesslich in EINEM
//           Verzeichnis. Namen sind blanke Dateinamen (keine Pfade, kein "..",
//           kein fuehrender Punkt); Dateien entstehen mit 0600 und nie ueber
//           eine bestehende Datei oder einen Symlink hinweg. Gelesen wird nur
//           eine regulaere Datei bis zu einer Groessengrenze.
// Wann:     2026-10-15
// Warum:    Review 2026-10-15, H1/M1: qc_backup_save schrieb an jeden Pfad aus
//           den Tool-Argumenten (ein per Prompt-Injection gesteuertes LLM
//           haette ~/.bashrc ueberschreiben koennen), qc_backup_restore las
//           jede Datei ohne Limit (/dev/zero, FIFO, ~/.ssh/id_*).
// Projekt:  Ampersand
// Abhaengig: POSIX (openat, O_NOFOLLOW, O_EXCL), std::filesystem
// Nutzung:  BackupStore s{BackupStore::default_dir()};
//           s.save("qc-2026-10-15.json", bytes);   // -> Pfad oder Fehler
//           s.load("qc-2026-10-15.json");          // -> Inhalt oder Fehler
// Historie: git log
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace qcmcp {

/// Obergrenze fuer eine Backup-Datei beim Lesen. Gemessen: 211 016 Bytes fuer
/// ein Backup mit Presets, Captures und IRs; 64 MiB lassen weiten Spielraum
/// und entsprechen der Inflate-Grenze je Nachricht (compression.hpp).
inline constexpr std::size_t kMaxBackupBytes = std::size_t{64} << 20;

class BackupStore {
public:
    /// $XDG_DATA_HOME/ampersand/backups, sonst ~/.local/share/ampersand/backups.
    [[nodiscard]] static std::filesystem::path default_dir();

    /// Prueft einen Dateinamen: 1..128 Zeichen aus [A-Za-z0-9._-], erstes
    /// Zeichen alphanumerisch (schliesst "", ".", "..", versteckte Dateien und
    /// jeden Pfadtrenner aus).
    [[nodiscard]] static std::expected<void, std::string> check_name(std::string_view name);

    explicit BackupStore(std::filesystem::path dir, std::size_t max_bytes = kMaxBackupBytes)
        : dir_(std::move(dir)), max_bytes_(max_bytes) {}

    /// Legt das Verzeichnis bei Bedarf an (0700) und schreibt `data` in eine
    /// NEUE Datei (O_CREAT|O_EXCL|O_NOFOLLOW, 0600). Eine vorhandene Datei
    /// gleichen Namens ist ein Fehler, sie wird nie ueberschrieben.
    [[nodiscard]] std::expected<std::filesystem::path, std::string> save(std::string_view name,
                                                                         std::string_view data) const;

    /// Liest eine regulaere Datei (kein Symlink, kein Geraet, keine FIFO) bis
    /// `max_bytes`; groessere Dateien sind ein Fehler.
    [[nodiscard]] std::expected<std::string, std::string> load(std::string_view name) const;

    [[nodiscard]] const std::filesystem::path& dir() const noexcept { return dir_; }

private:
    std::filesystem::path dir_;
    std::size_t max_bytes_;
};

}  // namespace qcmcp
