// log.cpp — Logging-Senke (siehe log.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "qc/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>

namespace qc::log {

namespace {

struct State {
    Sink sink = [](Level l, std::string_view m) {
        const auto now = std::chrono::system_clock::now();
        std::println(stderr, "{:%H:%M:%S} [{}] {}", std::chrono::floor<std::chrono::milliseconds>(now), to_string(l), m);
    };
};

Locked<State>& state() {
    static Locked<State> s;
    return s;
}

std::atomic<int> g_level{static_cast<int>(Level::Info)};

}  // namespace

std::string_view to_string(Level l) noexcept {
    switch (l) {
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "?";
}

void set_sink(Sink sink) {
    state().with([&](State& s) { s.sink = std::move(sink); });
}

void set_level(Level level) { g_level.store(static_cast<int>(level)); }

Level level() { return static_cast<Level>(g_level.load()); }

void write(Level lvl, std::string_view message) {
    if (static_cast<int>(lvl) < g_level.load()) return;
    Sink sink = state().with([](const State& s) { return s.sink; });
    if (sink) sink(lvl, message);
}

}  // namespace qc::log
