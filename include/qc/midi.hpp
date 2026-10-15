// midi.hpp — der zweite Steuerkanal des Quad Cortex: MIDI
//
// Wer:      Martin.Lonkwitz
// Was:      Findet den rawmidi-Ausgang des Geraets und sendet Control-Change-
//           Nachrichten darauf. Damit laesst sich steuern, was das
//           Protobuf-Protokoll nicht hergibt - allen voran das Stimmgeraet.
// Wann:     2026-09-11
// Warum:    ShowTuner (Nachricht 27) wird vom Geraet in jeder Form ignoriert
//           (docs/evidence/82). Das Handbuch nennt stattdessen CC#45, und am
//           Geraet ist das in beide Richtungen belegt (docs/evidence/84).
//           Zugegriffen wird direkt auf /dev/snd/midiC<N>D0 - ohne libasound,
//           wie auch der HID-Transport bewusst ohne hidapi auskommt.
// Projekt:  Ampersand
// Abhaengig: Linux, ALSA-rawmidi-Geraeteknoten, Schreibrecht darauf
// Nutzung:  auto port = qc::midi::Port::open_quad_cortex();
//           if (port) (void)port->tuner(true);
// Historie: git log
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace qc::midi {

// Control-Change-Nummern aus dem Handbuch (Abschnitt "MIDI Commands").
// Belegt am Geraet ist bisher nur kTuner; die uebrigen stehen hier, weil sie
// aus derselben Quelle stammen und die Tabelle sonst auseinanderfaellt.
inline constexpr int kBankMsb = 0;       ///< Preset-Gruppe (mit Program Change)
inline constexpr int kExpression1 = 1;
inline constexpr int kExpression2 = 2;
inline constexpr int kBankLsb = 32;      ///< Setlist
inline constexpr int kFootswitchA = 35;  ///< A..H liegen fortlaufend bis 42
inline constexpr int kScenes = 43;
inline constexpr int kTapTempo = 44;
inline constexpr int kTuner = 45;        ///< 0-63 schliesst, 64-127 oeffnet
inline constexpr int kGigView = 46;
inline constexpr int kModes = 47;

/// Ein offener rawmidi-Ausgang: beweglich, nicht kopierbar, schliesst sich
/// selbst. Ein Port haelt nur einen Dateideskriptor, deshalb reicht RAII ohne
/// weitere Besitzverwaltung.
class Port {
public:
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    Port(Port&& other) noexcept : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {}
    Port& operator=(Port&& other) noexcept;
    ~Port();

    /// Sucht die ALSA-Karte, deren Name `card_name` enthaelt, und oeffnet deren
    /// rawmidi-Ausgang.
    [[nodiscard]] static std::expected<Port, std::string> open_quad_cortex(std::string_view card_name = "Cortex");
    [[nodiscard]] static std::expected<Port, std::string> open(std::string_view device_path);

    /// Control Change senden: `channel` 0..15, `cc` und `value` je 0..127.
    [[nodiscard]] std::expected<void, std::string> control_change(int cc, int value, int channel = 0);

    /// Stimmgeraet des Geraets einblenden (true) oder schliessen (false).
    /// `channel` ist 0-basiert; das Geraet zaehlt in seinen Einstellungen ab 1.
    [[nodiscard]] std::expected<void, std::string> tuner(bool show, int channel = 0) {
        return control_change(kTuner, show ? 127 : 0, channel);
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    Port(int fd, std::string path) noexcept : fd_(fd), path_(std::move(path)) {}
    int fd_{-1};
    std::string path_;
};

/// Pfad des rawmidi-Ausgangs der genannten Karte, ohne ihn zu oeffnen.
[[nodiscard]] std::expected<std::string, std::string> find_device(std::string_view card_name = "Cortex");

}  // namespace qc::midi
