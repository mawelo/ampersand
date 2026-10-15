// wav.hpp — RIFF/WAVE mit IEEE-float32, mono
//
// Wer:      Martin.Lonkwitz
// Was:      Gemeinsamer WAV-Codec fuer alle Audio-Nutzlasten des Geraets
//           (Impulsantworten, FIR-Filter aus Captures). Kein I/O.
// Wann:     2026-09-08
// Warum:    IR- und Capture-Unit brauchen dasselbe WAV-Format. Eine Quelle
//           dafuer (SSOT) statt zweier Kopien.
// Projekt:  Ampersand
// Abhaengig: C++23 (<expected>, <span>)
// Nutzung:  auto wav = qc::wav::write(samples, 48000);
//           auto a   = qc::wav::read(bytes);   // a->sample_rate, a->samples
// Historie: git log
//
// Format: RIFF/WAVE, fmt-Chunk (16 B, Tag 3 = IEEE float), fact-Chunk mit der
// Sample-Anzahl, data-Chunk mit float32 little-endian. Genau ein Kanal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace qc::wav {

/// Groesse des erzeugten Kopfes vor dem data-Inhalt (RIFF + fmt + fact + data).
inline constexpr std::size_t kHeaderBytes = 56;

struct Audio {
    std::uint32_t sample_rate{};
    std::vector<float> samples;
};

/// Schreibt bereits als float32-little-endian vorliegende Bytes. Die Bytes
/// wandern **unveraendert** in den data-Chunk; der Rundlauf ist bit-identisch.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
write_f32le(std::span<const std::uint8_t> le_samples, std::uint32_t sample_rate);

/// Schreibt Samples als float32 little-endian.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
write(std::span<const float> samples, std::uint32_t sample_rate);

/// Liest eine WAV und gibt Abtastrate und den rohen data-Chunk zurueck.
/// Verlangt mono, IEEE float32; sonst ein Fehler mit Begruendung.
[[nodiscard]] std::expected<std::pair<std::uint32_t, std::vector<std::uint8_t>>, std::string>
read_f32le(std::span<const std::uint8_t> wav);

/// Wie read_f32le, aber als Samples.
[[nodiscard]] std::expected<Audio, std::string> read(std::span<const std::uint8_t> wav);

}  // namespace qc::wav
