// framing.hpp — HID-Report-Framing des Quad-Cortex-Protokolls
//
// Wer:      Martin.Lonkwitz
// Was:      Reiner Byte-Codec: logische Nachricht (Typ, Payload) <-> 129-Byte
//           HID-Reports, Fragmentierung/Reassemblierung, 8-Byte-Trailer.
//           Kein I/O, kein Protobuf, keine Threads.
// Wann:     2026-09-04
// Warum:    Das Geraet spricht Protobuf ueber HID-Reports mit eigenem Rahmen;
//           dieser Codec ist die Grundlage von Session/Transport.
// Projekt:  Ampersand
// Abhaengig: C++23 (<expected>, <span>)
// Nutzung:  auto reports = qc::framing::encode(type, payload);
//           qc::framing::Reassembler r; if (auto m = r.push(report); m && *m) ...
// Referenz: docs/protocol.md (Abschnitt 2), Belege aus Wire-Captures der
//           Referenzprojekte pyquadcortex (Python) und CortexUSB (C#).
// Historie: git log
//
// Wire-Format (bestaetigt durch Captures von Cortex Control 4.0.1):
//
//   Report (129 Bytes = Report-ID + 128 Byte Body)
//     [0]     report id: 0x02 host->device (OUTPUT), 0x01 device->host (INPUT)
//     [1..2]  u16 little-endian header:  bits 0..13 = Anzahl gueltiger Datenbytes
//                                        bit 14 (0x4000) = FIRST, bit 15 (0x8000) = LAST
//             (in der Praxis: [1] = len (<=126), [2] = flags 0x40/0x80)
//     [3..]   Daten, Rest mit Nullen aufgefuellt (vom Geraet: alter Pufferinhalt!)
//
//   Reassemblierter Body = protobuf ++ trailer(8)
//     [n+0..1] message type, u16 LE
//     [n+2..3] 0
//     [n+4]    encrypted flag (verschluesselte Nachrichten werden nicht unterstuetzt)
//     [n+5]    compressed flag (gzip)
//     [n+6..7] 0 vom Host; vom Geraet gefuellt, Bedeutung unbekannt, ignoriert
//
//   Es gibt KEIN Gesamtlaengenfeld: die Reassemblierung ist rein flag-gesteuert.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace qc::framing {

inline constexpr std::size_t kReportSize = 129;   ///< incl. report-ID byte
inline constexpr std::size_t kBodySize = 128;     ///< bytes after the report-ID
inline constexpr std::size_t kChunkSize = 126;    ///< data bytes per report
inline constexpr std::size_t kTrailerSize = 8;
inline constexpr std::uint8_t kOutReportId = 0x02;
inline constexpr std::uint8_t kInReportId = 0x01;
inline constexpr std::uint16_t kFlagFirst = 0x4000;
inline constexpr std::uint16_t kFlagLast = 0x8000;
inline constexpr std::uint16_t kLengthMask = 0x3FFF;
/// Policy cap for a single reassembled body. The largest observed message (the
/// ModelRepo catalog) is ~47 KB; 1 MiB leaves room and still unwedges a stream
/// whose LAST report was lost.
inline constexpr std::size_t kMaxBodyBytes = std::size_t{1} << 20;

using Report = std::array<std::uint8_t, kReportSize>;

/// A decoded logical message: type tag plus the raw (still possibly encrypted /
/// compressed) protobuf payload.
struct Envelope {
    std::uint16_t type{0};
    bool encrypted{false};
    bool compressed{false};
    std::vector<std::uint8_t> payload;
};

struct ReportHeader {
    std::size_t length{0};  ///< valid data bytes in this report (<= kChunkSize)
    bool first{false};
    bool last{false};
};

/// Frame one logical message into one or more OUTPUT reports.
/// `payload` is the serialized protobuf; the 8-byte trailer is appended here.
[[nodiscard]] std::vector<Report> encode(std::uint16_t type,
                                         std::span<const std::uint8_t> payload,
                                         bool encrypted = false,
                                         bool compressed = false);

/// Build the 8-byte trailer for `type`.
[[nodiscard]] std::array<std::uint8_t, kTrailerSize> make_trailer(std::uint16_t type,
                                                                  bool encrypted,
                                                                  bool compressed) noexcept;

/// Parse the len/flags header of one report (report-ID byte included in `report`).
[[nodiscard]] std::expected<ReportHeader, std::string>
parse_header(std::span<const std::uint8_t> report) noexcept;

/// Split a reassembled body into its envelope (strips the trailer).
[[nodiscard]] std::expected<Envelope, std::string>
decode_body(std::span<const std::uint8_t> body);

/// Flag-driven reassembly of INPUT reports into logical bodies.
///
/// Behaviour (mirrors both reference implementations):
///  * a FIRST|LAST report is complete on its own and is returned immediately,
///    even while a multi-report message is still being assembled (the device
///    interleaves small pushes, e.g. tempo ticks, into a large transfer);
///  * a FIRST report while assembling discards the stale partial;
///  * a continuation without an active assembly is ignored;
///  * exceeding kMaxBodyBytes resets the buffer (a lost LAST cannot wedge it).
class Reassembler {
public:
    struct Stats {
        std::uint64_t reports{0};
        std::uint64_t messages{0};
        std::uint64_t dropped_partials{0};
        std::uint64_t stray_continuations{0};
        std::uint64_t overflows{0};
    };

    /// Feed one report. Returns a complete body when one is ready.
    [[nodiscard]] std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
    push(std::span<const std::uint8_t> report);

    void reset() noexcept;
    [[nodiscard]] bool assembling() const noexcept { return assembling_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    std::vector<std::uint8_t> buffer_;
    bool assembling_{false};
    Stats stats_;
};

/// Hex dump helper ("08 03 0a 00 ...") for logs and tests.
[[nodiscard]] std::string hex(std::span<const std::uint8_t> bytes, std::size_t max_bytes = 0);

}  // namespace qc::framing
