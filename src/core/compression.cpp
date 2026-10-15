// compression.cpp — zlib-Huelle (siehe compression.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "qc/compression.hpp"

#include <zlib.h>

#include <format>
#include <memory>

namespace qc::compression {

bool is_gzip(std::span<const std::uint8_t> data) noexcept {
    return data.size() >= 2 && data[0] == 0x1F && data[1] == 0x8B;
}

namespace {

struct InflateCloser {
    void operator()(z_stream* s) const noexcept {
        if (s) {
            inflateEnd(s);
            delete s;  // unsafe{} paired with the `new` in inflate(); see below
        }
    }
};
struct DeflateCloser {
    void operator()(z_stream* s) const noexcept {
        if (s) {
            deflateEnd(s);
            delete s;  // unsafe{}
        }
    }
};

}  // namespace

std::expected<std::vector<std::uint8_t>, std::string> inflate(std::span<const std::uint8_t> data,
                                                               std::size_t max_out) {
    if (data.empty()) return std::unexpected("inflate: empty input");
    // unsafe{
    // zlib is a C API driven through raw pointers; the z_stream is owned by a
    // unique_ptr with a custom deleter so every exit path calls inflateEnd.
    std::unique_ptr<z_stream, InflateCloser> strm(new z_stream{});
    // 15 + 32: zlib auto-detects gzip and zlib headers.
    if (inflateInit2(strm.get(), 15 + 32) != Z_OK) return std::unexpected("inflateInit2 failed");

    std::vector<std::uint8_t> out;
    out.resize(std::min(max_out, std::max<std::size_t>(data.size() * 4, 4096)));
    strm->next_in = const_cast<Bytef*>(data.data());  // zlib's API is not const-correct
    strm->avail_in = static_cast<uInt>(data.size());

    for (;;) {
        if (strm->total_out >= out.size()) {
            if (out.size() >= max_out) return std::unexpected(std::format("inflate: output exceeds {} bytes", max_out));
            out.resize(std::min(max_out, out.size() * 2));
        }
        strm->next_out = out.data() + strm->total_out;
        strm->avail_out = static_cast<uInt>(out.size() - strm->total_out);
        const int rc = ::inflate(strm.get(), Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && strm->avail_in == 0) return std::unexpected("inflate: truncated stream");
        if (rc != Z_OK) return std::unexpected(std::format("inflate: zlib error {} ({})", rc, strm->msg ? strm->msg : "?"));
    }
    out.resize(strm->total_out);
    // }unsafe
    return out;
}

std::expected<std::vector<std::uint8_t>, std::string> decompress_if_gzip(std::span<const std::uint8_t> data,
                                                                          std::size_t max_out) {
    if (is_gzip(data)) return inflate(data, max_out);
    return std::vector<std::uint8_t>(data.begin(), data.end());
}

std::expected<std::vector<std::uint8_t>, std::string> gzip(std::span<const std::uint8_t> data) {
    // unsafe{
    std::unique_ptr<z_stream, DeflateCloser> strm(new z_stream{});
    // 15 + 16 = gzip container.
    if (deflateInit2(strm.get(), Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::unexpected("deflateInit2 failed");
    }
    std::vector<std::uint8_t> out(deflateBound(strm.get(), static_cast<uLong>(data.size())));
    strm->next_in = const_cast<Bytef*>(data.data());
    strm->avail_in = static_cast<uInt>(data.size());
    strm->next_out = out.data();
    strm->avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(strm.get(), Z_FINISH);
    if (rc != Z_STREAM_END) return std::unexpected(std::format("deflate: zlib error {}", rc));
    out.resize(strm->total_out);
    // }unsafe
    return out;
}

}  // namespace qc::compression
