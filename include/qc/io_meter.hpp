// io_meter.hpp — Pegel, die das Geraet selbst meldet (IOMeter, Typ 5)
//
// Wer:      Martin.Lonkwitz
// Was:      Die 38 Pegelfelder und den Kopfhoerer-Limiter-Merker der
//           IOMeterMessage als benannte Kanaltabelle; Zusammenfuehren
//           eingehender Nachrichten in einen Zustand; Umrechnung in dB.
// Wann:     2026-10-01
// Warum:    Die Pegel des Geraets sind gemessen verfuegbar (Beleg 182): Nach
//           einer Bestellung (IOMeter, action=CREATE, request_id) sendet das
//           Geraet rund 8,6 Nachrichten je Sekunde, SOLANGE am Eingang Signal
//           anliegt. Endet das Signal, folgt eine letzte Nachricht mit Pegel 0,
//           danach Stille. Keine Nachricht heisst also Pegel 0, nicht
//           "Verbindung weg". Ergaenzt den Weg ueber den Audiostrom
//           (qc::audio), der nur die acht USB-Kanaele sieht: IOMeter nennt auch
//           Returns, XLR-, Klinken-, Send- und Kopfhoererausgaenge.
// Projekt:  Ampersand
// Abhaengig: ProductionAutomation.pb.h (nur in io_meter.cpp)
// Nutzung:  qc::iometer::Levels l;  qc::iometer::merge(l, msg);
//           for (const auto& ch : qc::iometer::channels()) l.value(ch.index) ...
// GRENZE    Die Skala ist NICHT bestaetigt. Die Werte liegen in 0..1;
//           CortexUSB nennt sie "presumed linear amplitude". Gemessen lag
//           Eingang 1 im Median bei 0,14..0,50 (als linear gelesen −17..−6 dB),
//           waehrend der Audiostrom fuer dasselbe Spiel um −58 dBFS zeigte.
//           to_db() rechnet deshalb "als ob linear" und heisst nicht dBFS.
// Historie: git log
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cortex_protobuf_v2 { class IOMeterMessage; }

namespace qc::iometer {

/// Abo auffrischen. Ob ein einzelnes Abo lange Stille uebersteht, ist nicht
/// geprueft; wiederholtes Bestellen waehrend laufenden Stroms war gemessen
/// unschaedlich (Beleg 182, alle 30 s, Strom ungestoert).
inline constexpr std::chrono::seconds kResubscribeInterval{10};

enum class Group : std::uint8_t { Input, Output, Limiter, UsbOutput, UsbInput, Grid };

struct Channel {
    std::size_t index;       ///< Platz in Levels::values
    int field;               ///< Protobuf-Feldnummer
    std::string_view name;   ///< Anzeigename
    std::string_view key;    ///< kurz, fuer Spalten und JSON
    Group group;
};

inline constexpr std::size_t kChannelCount = 38;

/// Alle Pegelfelder in Feldreihenfolge (3..41 ohne 19).
[[nodiscard]] std::span<const Channel> channels() noexcept;

/// Der zusammengefuehrte Zustand. Felder, die eine Nachricht nicht traegt,
/// behalten ihren alten Wert.
struct Levels {
    std::array<float, kChannelCount> values{};
    bool hp_limiter_active{false};
    std::uint64_t updates{0};  ///< Zahl der eingearbeiteten Nachrichten

    [[nodiscard]] float value(std::size_t index) const noexcept {
        return index < values.size() ? values[index] : 0.0f;
    }
    friend bool operator==(const Levels&, const Levels&) = default;
};

/// Kennzahlen eines Kanals ueber einen Messzeitraum.
struct ChannelStats {
    float last{0.0f};
    float max{0.0f};
    double sum{0.0};  ///< fuer den Mittelwert ueber alle Meldungen
};

/// Sammelt Pegelstaende ueber einen Zeitraum (MCP-Werkzeug, Auswertungen).
/// Nicht threadsicher; der Aufrufer schuetzt es.
class Accumulator {
public:
    void add(const Levels& levels) noexcept;
    [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }
    [[nodiscard]] const ChannelStats& stats(std::size_t index) const noexcept { return stats_.at(index); }
    [[nodiscard]] double mean(std::size_t index) const noexcept {
        return messages_ ? stats_.at(index).sum / static_cast<double>(messages_) : 0.0;
    }
    /// Wie oft hp_limiter_active wahr war.
    [[nodiscard]] std::uint64_t hp_limiter_true() const noexcept { return hp_limiter_true_; }

private:
    std::array<ChannelStats, kChannelCount> stats_{};
    std::uint64_t messages_{0};
    std::uint64_t hp_limiter_true_{0};
};

/// Eine eingegangene Nachricht in `levels` einarbeiten.
void merge(Levels& levels, const cortex_protobuf_v2::IOMeterMessage& msg);

/// Wert in dB unter der Annahme linearer Amplitude; 0 und darunter ergeben
/// kFloorDb. Siehe GRENZE oben.
inline constexpr double kFloorDb = -144.0;
[[nodiscard]] double to_db(float value) noexcept;

}  // namespace qc::iometer
