// version.hpp — Versionskennung der Bibliothek/Binaries
//
// Wer:      Martin.Lonkwitz
// Was:      qc::version() liefert genau eine Zeile (<= 80 Zeichen) mit
//           Projektname, Versionsnummer, Commit-ID und Build-Datum.
//           Implementiert in der zur Build-Zeit erzeugten Datei
//           build/<preset>/generated/qc_version.cpp (cmake/gen_version.cmake).
// Wann:     2026-09-04
// Warum:    Regel "Libraries & Versionierung": jede Library braucht version().
// Projekt:  Ampersand
// Abhaengig: keine
// Nutzung:  std::println("{}", qc::version());
// Historie: git log
#pragma once

#include <string_view>

namespace qc {

/// One line, at most 80 characters: "<name> <semver>+<commit>[-dirty] (<date>)".
[[nodiscard]] std::string_view version() noexcept;

/// The short commit id (with "-dirty" suffix when the tree had local changes).
[[nodiscard]] std::string_view commit_id() noexcept;

}  // namespace qc
