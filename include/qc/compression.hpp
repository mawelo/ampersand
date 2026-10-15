// compression.hpp — gzip/zlib-Dekompression von Geraete-Payloads
//
// Wer:      Martin.Lonkwitz
// Was:      Duenne, sichere Huelle um zlib: erkennt gzip-Magic (1f 8b) und
//           dekomprimiert mit Groessen-Obergrenze; alles andere geht unveraendert
//           durch.
// Wann:     2026-09-04
// Warum:    RecallPreset-Pushes, Factory-Listings und der ModelRepo-Katalog
//           kommen gzip-komprimiert vom Geraet.
// Projekt:  Ampersand
// Abhaengig: zlib
// Nutzung:  auto plain = qc::compression::decompress_if_gzip(payload);
// Historie: git log
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace qc::compression {

/// Upper bound for a decompressed payload (the catalog expands to ~560 KB).
inline constexpr std::size_t kMaxInflated = std::size_t{64} << 20;

[[nodiscard]] bool is_gzip(std::span<const std::uint8_t> data) noexcept;

/// Inflate a gzip or zlib stream (auto-detected by zlib).
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
inflate(std::span<const std::uint8_t> data, std::size_t max_out = kMaxInflated);

/// gunzip when the magic bytes say so; otherwise return a copy of the input.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
decompress_if_gzip(std::span<const std::uint8_t> data, std::size_t max_out = kMaxInflated);

/// gzip-compress (used by tests and by outbound payloads that need it).
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
gzip(std::span<const std::uint8_t> data);

}  // namespace qc::compression
