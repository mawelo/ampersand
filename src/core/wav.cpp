// wav.cpp — RIFF/WAVE mit IEEE-float32, mono (siehe wav.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      Implementierung des gemeinsamen WAV-Codecs.
// Wann:     2026-09-08
// Warum:    s. wav.hpp
// Projekt:  Ampersand
// Abhaengig: C++23/26 Standardbibliothek (kein OpenSSL, kein I/O)
// Historie: git log
#include "qc/wav.hpp"

#include <bit>
#include <format>

namespace qc::wav {

namespace {

[[nodiscard]] std::uint16_t get_u16le(std::span<const std::uint8_t> b, std::size_t off) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[off]) |
                                      static_cast<std::uint16_t>(b[off + 1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32le(std::span<const std::uint8_t> b, std::size_t off) noexcept {
    return static_cast<std::uint32_t>(b[off]) | static_cast<std::uint32_t>(b[off + 1]) << 8 |
           static_cast<std::uint32_t>(b[off + 2]) << 16 | static_cast<std::uint32_t>(b[off + 3]) << 24;
}

void put_u16le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void put_tag(std::vector<std::uint8_t>& out, const char (&tag)[5]) {
    out.insert(out.end(), tag, tag + 4);
}

[[nodiscard]] bool tag_is(std::span<const std::uint8_t> b, std::size_t off, const char (&tag)[5]) noexcept {
    return b[off] == static_cast<std::uint8_t>(tag[0]) && b[off + 1] == static_cast<std::uint8_t>(tag[1]) &&
           b[off + 2] == static_cast<std::uint8_t>(tag[2]) && b[off + 3] == static_cast<std::uint8_t>(tag[3]);
}

constexpr std::uint16_t kFormatIeeeFloat = 3;
constexpr std::uint16_t kChannels = 1;
constexpr std::uint16_t kBlockAlign = 4;
constexpr std::uint16_t kBitsPerSample = 32;

}  // namespace

std::expected<std::vector<std::uint8_t>, std::string>
write_f32le(std::span<const std::uint8_t> le_samples, std::uint32_t sample_rate) {
    if (le_samples.size() % 4 != 0) {
        return std::unexpected(
            std::format("data length {} is not a multiple of 4 bytes", le_samples.size()));
    }
    if (sample_rate == 0) return std::unexpected("sample rate must not be zero");
    if (le_samples.size() > 0xFFFF'FF00U) return std::unexpected("data does not fit a RIFF chunk");

    std::vector<std::uint8_t> chunks;
    chunks.reserve(le_samples.size() + kHeaderBytes);
    put_tag(chunks, "fmt ");
    put_u32le(chunks, 16);
    put_u16le(chunks, kFormatIeeeFloat);
    put_u16le(chunks, kChannels);
    put_u32le(chunks, sample_rate);
    put_u32le(chunks, sample_rate * kBlockAlign);
    put_u16le(chunks, kBlockAlign);
    put_u16le(chunks, kBitsPerSample);
    // fact (Sample-Anzahl; von IEEE-float-WAVs erwartet)
    put_tag(chunks, "fact");
    put_u32le(chunks, 4);
    put_u32le(chunks, static_cast<std::uint32_t>(le_samples.size() / 4));
    put_tag(chunks, "data");
    put_u32le(chunks, static_cast<std::uint32_t>(le_samples.size()));
    chunks.insert(chunks.end(), le_samples.begin(), le_samples.end());

    std::vector<std::uint8_t> out;
    out.reserve(chunks.size() + 12);
    put_tag(out, "RIFF");
    put_u32le(out, static_cast<std::uint32_t>(chunks.size() + 4));
    put_tag(out, "WAVE");
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

std::expected<std::vector<std::uint8_t>, std::string>
write(std::span<const float> samples, std::uint32_t sample_rate) {
    std::vector<std::uint8_t> le;
    le.reserve(samples.size() * 4);
    for (const float s : samples) {
        const auto bits = std::bit_cast<std::uint32_t>(s);
        le.push_back(static_cast<std::uint8_t>(bits & 0xFF));
        le.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        le.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
        le.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
    }
    return write_f32le(le, sample_rate);
}

std::expected<std::pair<std::uint32_t, std::vector<std::uint8_t>>, std::string>
read_f32le(std::span<const std::uint8_t> wav) {
    if (wav.size() < 12 || !tag_is(wav, 0, "RIFF") || !tag_is(wav, 8, "WAVE")) {
        return std::unexpected("input is not a RIFF/WAVE file");
    }
    bool have_fmt = false;
    std::uint32_t rate = 0;
    std::span<const std::uint8_t> data;
    std::size_t pos = 12;
    while (pos + 8 <= wav.size()) {
        const std::uint32_t size = get_u32le(wav, pos + 4);
        const std::size_t start = pos + 8;
        if (size > wav.size() - start) return std::unexpected("truncated WAVE chunk");
        const auto chunk = wav.subspan(start, size);
        if (tag_is(wav, pos, "fmt ")) {
            if (chunk.size() < 16) return std::unexpected("truncated WAVE format chunk");
            if (get_u16le(chunk, 0) != kFormatIeeeFloat || get_u16le(chunk, 2) != kChannels ||
                get_u16le(chunk, 12) != kBlockAlign || get_u16le(chunk, 14) != kBitsPerSample) {
                return std::unexpected("WAVE must be mono, IEEE float32");
            }
            rate = get_u32le(chunk, 4);
            if (rate == 0) return std::unexpected("WAVE sample rate is zero");
            have_fmt = true;
        } else if (tag_is(wav, pos, "data")) {
            data = chunk;
        }
        pos = start + size + (size & 1U);  // Chunks sind auf gerade Groesse gepaddet
    }
    if (!have_fmt) return std::unexpected("WAVE has no format chunk");
    if (data.empty()) return std::unexpected("WAVE has no data chunk");
    if (data.size() % 4 != 0) return std::unexpected("WAVE data is not a whole number of samples");
    return std::pair{rate, std::vector<std::uint8_t>(data.begin(), data.end())};
}

std::expected<Audio, std::string> read(std::span<const std::uint8_t> wav) {
    auto raw = read_f32le(wav);
    if (!raw) return std::unexpected(raw.error());
    Audio a;
    a.sample_rate = raw->first;
    a.samples.reserve(raw->second.size() / 4);
    for (std::size_t i = 0; i + 3 < raw->second.size(); i += 4) {
        a.samples.push_back(std::bit_cast<float>(get_u32le(raw->second, i)));
    }
    return a;
}

}  // namespace qc::wav
