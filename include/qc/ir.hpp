// ir.hpp — Impulsantworten (IR) des Quad Cortex: Nutzdaten <-> WAV
//
// Wer:      Martin.Lonkwitz
// Was:      Reiner Byte-Codec zwischen der entschluesselten IR-Nutzlast des
//           Geraets und einer RIFF/WAVE-Datei. Kein I/O, keine Krypto.
// Wann:     2026-09-08
// Warum:    Factory- und eigene IRs lesbar/schreibbar machen (Ziel aus todo.md:
//           "eigene IRs installieren"). Die Entschluesselung liefert nur rohe
//           Bytes; erst dieses Format macht daraus hoerbares Audio.
// Projekt:  Ampersand
// Abhaengig: C++23 (<expected>, <span>)
// Nutzung:  auto wav     = qc::ir::payload_to_wav(decrypted_payload);
//           auto payload = qc::ir::wav_to_payload(wav);  // zurueck
// Historie: git log
//
// Format (Beleg: docs/units/ir.md):
//   Entschluesselte IR-Nutzlast = genau 4096 Bytes
//                               = 1024 Samples float32, little-endian
//                               = 48 kHz, mono.
//   Die WAV ist IEEE-float32 (fmt-Tag 3) mit fact-Chunk; die Nutzlast wandert
//   unveraendert in den data-Chunk (verlustfrei, bit-identischer Rundlauf).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace qc::ir {

inline constexpr std::size_t kPayloadBytes = 4096;   ///< Groesse der IR-Nutzlast
inline constexpr std::size_t kSampleCount = 1024;    ///< float32-Samples darin
inline constexpr std::uint32_t kSampleRate = 48000;  ///< Hz, mono
inline constexpr std::size_t kWavBytes = 4152;       ///< Groesse der erzeugten WAV

/// Plausibilitaetspruefung der Nutzlast: exakt kPayloadBytes, alle Samples
/// endlich und |s| <= 8.0, und mindestens 32 Samples ungleich (nahe) null.
/// Schuetzt davor, eine falsch entschluesselte Datei als IR zu behandeln.
[[nodiscard]] std::expected<void, std::string> validate_payload(std::span<const std::uint8_t> payload);

/// Nutzlast -> vollstaendige RIFF/WAVE-Datei (48 kHz, mono, IEEE float32).
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
payload_to_wav(std::span<const std::uint8_t> payload);

/// RIFF/WAVE -> Nutzlast. Verlangt exakt 48 kHz, mono, IEEE float32 und einen
/// data-Chunk von kPayloadBytes; sonst ein Fehler mit Begruendung.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
wav_to_payload(std::span<const std::uint8_t> wav);

}  // namespace qc::ir
