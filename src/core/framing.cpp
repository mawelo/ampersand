// framing.cpp — Implementierung des HID-Report-Codecs (siehe framing.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      encode/decode/Reassembler fuer das Quad-Cortex-HID-Framing.
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "qc/framing.hpp"

#include <algorithm>
#include <format>

namespace qc::framing {

std::array<std::uint8_t, kTrailerSize> make_trailer(std::uint16_t type, bool encrypted,
                                                    bool compressed) noexcept {
    return {static_cast<std::uint8_t>(type & 0xFF),
            static_cast<std::uint8_t>((type >> 8) & 0xFF),
            0, 0,
            static_cast<std::uint8_t>(encrypted ? 1 : 0),
            static_cast<std::uint8_t>(compressed ? 1 : 0),
            0, 0};
}

std::vector<Report> encode(std::uint16_t type, std::span<const std::uint8_t> payload,
                           bool encrypted, bool compressed) {
    std::vector<std::uint8_t> body;
    body.reserve(payload.size() + kTrailerSize);
    body.insert(body.end(), payload.begin(), payload.end());
    const auto trailer = make_trailer(type, encrypted, compressed);
    body.insert(body.end(), trailer.begin(), trailer.end());

    const std::size_t chunks = (body.size() + kChunkSize - 1) / kChunkSize;
    std::vector<Report> reports;
    reports.reserve(chunks);
    for (std::size_t i = 0; i < chunks; ++i) {
        const std::size_t offset = i * kChunkSize;
        const std::size_t len = std::min(kChunkSize, body.size() - offset);
        std::uint16_t header = static_cast<std::uint16_t>(len & kLengthMask);
        if (i == 0) header = static_cast<std::uint16_t>(header | kFlagFirst);
        if (i + 1 == chunks) header = static_cast<std::uint16_t>(header | kFlagLast);

        Report r{};
        r.at(0) = kOutReportId;
        r.at(1) = static_cast<std::uint8_t>(header & 0xFF);
        r.at(2) = static_cast<std::uint8_t>((header >> 8) & 0xFF);
        std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(offset), len, r.begin() + 3);
        reports.push_back(r);
    }
    return reports;
}

std::expected<ReportHeader, std::string> parse_header(std::span<const std::uint8_t> report) noexcept {
    if (report.size() < 3) {
        return std::unexpected(std::format("report is only {} bytes; need id+header", report.size()));
    }
    const std::uint16_t header =
        static_cast<std::uint16_t>(report[1] | (static_cast<std::uint16_t>(report[2]) << 8));
    ReportHeader h;
    h.length = header & kLengthMask;
    h.first = (header & kFlagFirst) != 0;
    h.last = (header & kFlagLast) != 0;
    // `length` is authoritative but never trusted beyond the bytes present.
    h.length = std::min({h.length, kChunkSize, report.size() - 3});
    return h;
}

std::expected<Envelope, std::string> decode_body(std::span<const std::uint8_t> body) {
    if (body.size() < kTrailerSize) {
        return std::unexpected(std::format("body is {} bytes, shorter than the {}-byte trailer",
                                           body.size(), kTrailerSize));
    }
    const std::size_t n = body.size() - kTrailerSize;
    Envelope e;
    e.type = static_cast<std::uint16_t>(body[n] | (static_cast<std::uint16_t>(body[n + 1]) << 8));
    e.encrypted = body[n + 4] != 0;
    e.compressed = body[n + 5] != 0;
    e.payload.assign(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(n));
    return e;
}

std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
Reassembler::push(std::span<const std::uint8_t> report) {
    ++stats_.reports;
    auto h = parse_header(report);
    if (!h) {
        reset();
        return std::unexpected(h.error());
    }
    const auto data = report.subspan(3, h->length);

    if (h->first && h->last) {
        // Complete in one report. Do NOT disturb an in-progress multi-report
        // assembly: the device interleaves small pushes into large transfers.
        ++stats_.messages;
        return std::vector<std::uint8_t>(data.begin(), data.end());
    }
    if (h->first) {
        if (assembling_) ++stats_.dropped_partials;
        buffer_.assign(data.begin(), data.end());
        assembling_ = true;
        return std::nullopt;
    }
    if (!assembling_) {
        ++stats_.stray_continuations;
        return std::nullopt;
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    if (buffer_.size() > kMaxBodyBytes) {
        ++stats_.overflows;
        reset();
        return std::unexpected(
            std::format("reassembly buffer exceeded {} bytes without LAST; reset", kMaxBodyBytes));
    }
    if (h->last) {
        ++stats_.messages;
        std::vector<std::uint8_t> out;
        out.swap(buffer_);
        assembling_ = false;
        return out;
    }
    return std::nullopt;
}

void Reassembler::reset() noexcept {
    buffer_.clear();
    assembling_ = false;
}

std::string hex(std::span<const std::uint8_t> bytes, std::size_t max_bytes) {
    std::string out;
    const std::size_t n = (max_bytes == 0) ? bytes.size() : std::min(max_bytes, bytes.size());
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out.push_back(' ');
        out += std::format("{:02x}", bytes[i]);
    }
    if (n < bytes.size()) out += std::format(" ...(+{})", bytes.size() - n);
    return out;
}

}  // namespace qc::framing
