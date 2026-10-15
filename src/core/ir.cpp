// ir.cpp — IR-Nutzdaten <-> RIFF/WAVE (siehe ir.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      Implementierung des IR-Codecs: Validierung, WAV schreiben/lesen.
// Wann:     2026-09-08
// Warum:    s. ir.hpp
// Projekt:  Ampersand
// Abhaengig: C++23/26 Standardbibliothek, qc::wav (kein OpenSSL, kein I/O)
// Historie: git log
#include "qc/ir.hpp"

#include "qc/wav.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>

namespace qc::ir {

namespace {

// Little-Endian-Lesen ohne Aliasing-Tricks: Bytes einzeln, dann bit_cast.
[[nodiscard]] std::uint32_t get_u32le(std::span<const std::uint8_t> b, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(b[off]) | static_cast<std::uint32_t>(b[off + 1]) << 8 |
           static_cast<std::uint32_t>(b[off + 2]) << 16 | static_cast<std::uint32_t>(b[off + 3]) << 24;
}

}  // namespace

std::expected<void, std::string> validate_payload(std::span<const std::uint8_t> payload) {
    if (payload.size() != kPayloadBytes) {
        return std::unexpected(
            std::format("IR payload is {} bytes, expected {}", payload.size(), kPayloadBytes));
    }
    std::size_t audible = 0;
    for (std::size_t i = 0; i < kSampleCount; ++i) {
        const float s = std::bit_cast<float>(get_u32le(payload, i * 4));
        if (!std::isfinite(s) || std::abs(s) > 8.0F) {
            return std::unexpected(std::format(
                "sample {} is {} — not bounded float32 IR data (wrong serial or not an IR?)", i, s));
        }
        if (std::abs(s) > 1e-12F) ++audible;
    }
    if (audible < 32) {
        return std::unexpected("payload is implausibly empty (fewer than 32 non-zero samples)");
    }
    return {};
}

std::expected<std::vector<std::uint8_t>, std::string>
payload_to_wav(std::span<const std::uint8_t> payload) {
    if (auto ok = validate_payload(payload); !ok) return std::unexpected(ok.error());
    // Die Bytes wandern unveraendert in den data-Chunk -> bit-identischer Rundlauf.
    return qc::wav::write_f32le(payload, kSampleRate);
}

std::expected<std::vector<std::uint8_t>, std::string>
wav_to_payload(std::span<const std::uint8_t> wav) {
    auto raw = qc::wav::read_f32le(wav);
    if (!raw) return std::unexpected(raw.error());
    if (raw->first != kSampleRate) {
        return std::unexpected(std::format("WAVE is {} Hz, an IR must be {} Hz", raw->first, kSampleRate));
    }
    if (auto ok = validate_payload(raw->second); !ok) return std::unexpected(ok.error());
    return std::move(raw->second);
}

}  // namespace qc::ir
