// test_compression.cpp — gzip-Rundreise
#include "qc/compression.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace qc::compression;

TEST_CASE("gzip round trip and magic detection", "[compression]") {
    std::vector<std::uint8_t> plain(5000);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<std::uint8_t>(i % 13);
    auto gz = gzip(plain);
    REQUIRE(gz.has_value());
    CHECK(is_gzip(*gz));
    CHECK(gz->size() < plain.size());
    auto back = decompress_if_gzip(*gz);
    REQUIRE(back.has_value());
    CHECK(*back == plain);
}

TEST_CASE("non-gzip data passes through unchanged", "[compression]") {
    std::vector<std::uint8_t> data{0x08, 0x03, 0x10};
    CHECK_FALSE(is_gzip(data));
    auto same = decompress_if_gzip(data);
    REQUIRE(same.has_value());
    CHECK(*same == data);
}

TEST_CASE("truncated gzip is an error, not a crash", "[compression]") {
    std::vector<std::uint8_t> plain(1000, 7);
    auto gz = gzip(plain);
    REQUIRE(gz.has_value());
    gz->resize(gz->size() / 2);
    CHECK_FALSE(inflate(*gz).has_value());
}

TEST_CASE("output cap is enforced", "[compression]") {
    std::vector<std::uint8_t> plain(100000, 0);
    auto gz = gzip(plain);
    REQUIRE(gz.has_value());
    CHECK_FALSE(inflate(*gz, 1024).has_value());
}
