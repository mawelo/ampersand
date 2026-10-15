// audio.hpp — den USB-Audiostrom des Geraets mitlesen
//
// Wer:      Martin.Lonkwitz
// Was:      Aufnahmestrom von der ALSA-Karte des Quad Cortex, kanalweise als
//           float. Kanal 1/2 fuehrt das trockene Eingangssignal, 3/4 den
//           Preset-Ausgang (gemessen, docs/evidence/85).
// Wann:     2026-09-11
// Warum:    Das Geraet meldet dem Host weder Pegel noch Tonhoehe ueber das
//           Protokoll (docs/evidence/83). Beides steht aber im Audiostrom - so
//           entstehen Pegelanzeige und Stimmgeraet, ohne das Geraet zu fragen.
//
//           ACHTUNG, der teuerste Fallstrick des Geraets: Sein Aufnahme-
//           Endpunkt ist zugleich der Taktgeber der Wiedergabe
//           ("Implicit Feedback Mode: Yes" in /proc/asound/cardN/stream0).
//           Laeuft nur die Aufnahme, liefert es sauber getaktete DIGITALE
//           STILLE - alle Kanaele exakt null. Deshalb haelt diese Klasse
//           zusaetzlich einen Wiedergabestrom offen, der Stille ausgibt.
// Projekt:  Ampersand
// Abhaengig: libasound (ALSA), exklusiver Zugriff auf die Karte
// Nutzung:  auto in = qc::audio::Capture::open_quad_cortex();
//           std::vector<float> block(2048);
//           if (in) (void)in->read_channel(0, block);
// Historie: git log
#pragma once

#include <expected>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace qc::audio {

inline constexpr int kChannels = 8;
inline constexpr int kSampleRate = 48000;

/// Kanalbelegung, wie am Geraet gemessen (docs/evidence/85).
inline constexpr int kDryLeft = 0;   ///< Eingang 1, unbearbeitet
inline constexpr int kDryRight = 1;  ///< Eingang 2, unbearbeitet
inline constexpr int kWetLeft = 2;   ///< Preset-Ausgang, sofern dorthin geroutet
inline constexpr int kWetRight = 3;

/// Klartext zu einem Kanal - fuer Anzeigen, damit nirgends eine zweite,
/// abweichende Liste entsteht. Die Kanaele 1..4 sind am Geraet gemessen
/// (docs/evidence/85); was auf 5..8 liegt, bestimmt die USB-Zuordnung des
/// Geraets, deshalb tragen sie nur ihre Nummer und keine Behauptung.
[[nodiscard]] inline constexpr std::string_view channel_name(int channel) noexcept {
    switch (channel) {
        case kDryLeft:  return "input 1 (dry)";
        case kDryRight: return "input 2 (dry)";
        case kWetLeft:  return "usb out 3 (preset)";
        case kWetRight: return "usb out 4 (preset)";
        case 4:         return "usb 5";
        case 5:         return "usb 6";
        case 6:         return "usb 7";
        case 7:         return "usb 8";
        default:        return "?";
    }
}

/// Laufender Aufnahmestrom. Beweglich, nicht kopierbar; beide ALSA-Stroeme
/// werden im Destruktor geschlossen und der Stille-Ausgeber beendet.
class Capture {
public:
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) noexcept;
    Capture& operator=(Capture&&) noexcept;
    ~Capture();

    /// Oeffnet die Karte, deren Name `card_name` enthaelt, und startet den
    /// Wiedergabestrom, ohne den nichts ankommt.
    [[nodiscard]] static std::expected<Capture, std::string> open_quad_cortex(std::string_view card_name = "Cortex");

    /// Legt die zuletzt eingetroffenen Rahmen eines Kanals ab — so viele, wie in
    /// `out` passen. Blockiert, bis genug beisammen sind. Ein eigener Lesefaden
    /// holt die Rahmen fortlaufend ab; deshalb kostet eine lange Rechnung des
    /// Aufrufers keine Abtastwerte.
    [[nodiscard]] std::expected<std::size_t, std::string> read_channel(int channel, std::span<float> out);

    /// Zahl der Ueberlaeufe seit dem Oeffnen. Jeder davon reisst ein Loch ins
    /// Signal; steigt der Wert, ist die gemessene Tonhoehe nicht zu gebrauchen.
    [[nodiscard]] std::uint64_t dropouts() const noexcept;

    /// Effektivwert des zuletzt gelesenen Blocks je Kanal — fuer eine
    /// Pegelanzeige, ohne den Block zweimal zu holen.
    [[nodiscard]] double last_rms(int channel) const noexcept;

    [[nodiscard]] static constexpr int channels() noexcept { return kChannels; }
    [[nodiscard]] static constexpr int sample_rate() noexcept { return kSampleRate; }

private:
    struct Impl;
    explicit Capture(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qc::audio
