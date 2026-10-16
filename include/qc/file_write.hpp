// file_write.hpp — Nutzerdaten privat und atomar auf die Platte schreiben
//
// Wer:      Martin.Lonkwitz
// Was:      write_private_atomic(): schreibt Bytes in eine Datei, nur fuer den
//           Besitzer lesbar (0600), atomar per Temp-Datei daneben + rename.
//           Kein Geraete-I/O, keine Krypto.
// Wann:     2026-10-16
// Warum:    SSOT fuer das Sichern frei waehlbarer Dateien (qcctl backup-save
//           und die GUI-Sicherung). Ein Schreibfehler darf eine schon
//           vorhandene Sicherung nicht zerstoeren (Re-Review 2026-10-15:
//           O_TRUNC tat genau das).
// Projekt:  Ampersand (libqccore)
// Abhaengig: POSIX (open/write/fsync/rename), C++23 (<expected>, <span>)
// Nutzung:  auto r = qc::io::write_private_atomic("/pfad/backup.json", bytes);
//           if (!r) std::println(stderr, "{}", r.error());
// Korrektheit/Sicherheit: siehe docs/file-write.md
// Historie: git log
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qc::io {

/// Schreibt `bytes` nach `path`, Modus 0600, atomar. Erst in
/// `<path>.tmp.<pid>.<n>` (O_EXCL|O_NOFOLLOW) schreiben, fsync, dann per rename
/// an den Platz ziehen. Bei jedem Fehler bleibt `path` unberuehrt und die
/// Temp-Datei wird entfernt.
[[nodiscard]] inline std::expected<void, std::string> write_private_atomic(const std::string& path,
                                                                           std::span<const std::uint8_t> bytes) {
    static unsigned counter = 0;  // nur fuer eindeutige Temp-Namen innerhalb eines Prozesses
    const std::string tmp = std::format("{}.tmp.{}.{}", path, static_cast<long>(::getpid()), counter++);

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return std::unexpected(std::format("cannot write {}: {}", tmp, std::strerror(errno)));

    bool ok = ::fchmod(fd, 0600) == 0;  // umask-unabhaengig sicherstellen
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) done += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    const int werr = errno;
    const bool closed = ::close(fd) == 0;
    if (!ok || !closed) {
        ::unlink(tmp.c_str());
        return std::unexpected(std::format("cannot write {}: {}", path, std::strerror(werr)));
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int rerr = errno;
        ::unlink(tmp.c_str());
        return std::unexpected(std::format("cannot place {}: {}", path, std::strerror(rerr)));
    }
    return {};
}

/// Bequemlichkeit fuer Text-/JSON-Nutzlasten.
[[nodiscard]] inline std::expected<void, std::string> write_private_atomic(const std::string& path,
                                                                           std::string_view bytes) {
    return write_private_atomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                                                    bytes.size()));
}

}  // namespace qc::io
