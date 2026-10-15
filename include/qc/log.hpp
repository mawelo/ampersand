// log.hpp — kleines Logging mit austauschbarer Senke
//
// Wer:      Martin.Lonkwitz
// Was:      Vier Stufen (debug/info/warn/error), std::format-Aufrufe, eine
//           prozessweite Senke (Default: stderr ab "info"). GUI/MCP haengen
//           ihre eigene Senke ein (Log-Panel bzw. stderr, weil stdout dem
//           JSON-RPC gehoert).
// Wann:     2026-09-04
// Warum:    Session/Transport muessen Diagnosen ausgeben, ohne an ein UI
//           gebunden zu sein.
// Projekt:  Ampersand
// Abhaengig: C++23 (<format>)
// Nutzung:  qc::log::warn("write failed: {}", err);
// Historie: git log
//
// Bewusste Ausnahme von "kein globaler veraenderlicher Zustand": eine
// Logging-Senke ist ein Prozess-Singleton; Zugriff nur ueber Locked<>.
#pragma once

#include "qc/locked.hpp"

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace qc::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

using Sink = std::function<void(Level, std::string_view)>;

[[nodiscard]] std::string_view to_string(Level l) noexcept;

/// Replace the sink (thread-safe). An empty sink silences everything.
void set_sink(Sink sink);
/// Messages below `level` are dropped before formatting reaches the sink.
void set_level(Level level);
[[nodiscard]] Level level();

void write(Level level, std::string_view message);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Debug) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Info) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Warn) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (level() <= Level::Error) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace qc::log
