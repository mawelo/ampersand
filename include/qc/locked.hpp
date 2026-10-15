// __REUSABLE__ locked.hpp — Mutex-gekapselter Wert (Rust-Mutex<T>-Analogon)
//
// Wer:      Martin.Lonkwitz
// Was:      qc::Locked<T>: der geschuetzte Wert ist NUR ueber with(lambda)
//           erreichbar; der Lock wird per RAII gehalten. Kein vergessenes
//           Locken moeglich (Memory-Safety-Regel 8.1).
// Wann:     2026-09-04
// Warum:    Session/Device teilen Zustand zwischen RX-Thread, Keepalive-Thread
//           und Aufrufern.
// Projekt:  Ampersand (reusable)
// Abhaengig: C++17 (<mutex>)
// Nutzung:  qc::Locked<std::vector<int>> q;
//           q.with([](auto& v){ v.push_back(1); });
//           auto n = q.with([](const auto& v){ return v.size(); });
// Historie: git log
#pragma once

#include <mutex>
#include <utility>

namespace qc {

template <class T>
class Locked {
public:
    Locked() = default;
    explicit Locked(T value) : value_(std::move(value)) {}
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

    template <class F>
    auto with(F&& f) {
        std::lock_guard lk{mutex_};
        return std::forward<F>(f)(value_);
    }
    template <class F>
    auto with(F&& f) const {
        std::lock_guard lk{mutex_};
        return std::forward<F>(f)(value_);
    }

    /// Copy out (T must be copyable).
    T snapshot() const {
        std::lock_guard lk{mutex_};
        return value_;
    }

private:
    mutable std::mutex mutex_;
    T value_{};
};

}  // namespace qc
