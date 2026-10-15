// test_ir.cpp — IR-Nutzlast <-> WAV: Rundlauf, Format, Plausibilitaetspruefung
//
// Bewusst nur SYNTHETISCHE Daten (abklingender Sinus): keine Geraete- oder
// Factory-Inhalte im Repo. Das Format selbst ist am Geraet verifiziert
// (bit-identischer Rundlauf und Byte-Gleichheit gegen eine unabhaengige
// Implementierung, s. docs/units/ir.md).
#include "qc/ir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace {

/// Ein plausibler Mini-IR: abklingender Sinus, |s| < 1.
std::vector<std::uint8_t> make_payload() {
    std::vector<std::uint8_t> p;
    p.reserve(qc::ir::kPayloadBytes);
    for (std::size_t i = 0; i < qc::ir::kSampleCount; ++i) {
        const auto t = static_cast<float>(i) / static_cast<float>(qc::ir::kSampleRate);
        const float s = std::sin(2.0F * std::numbers::pi_v<float> * 220.0F * t) *
                        std::exp(-static_cast<float>(i) / 200.0F);
        const auto bits = std::bit_cast<std::uint32_t>(s);
        p.push_back(static_cast<std::uint8_t>(bits & 0xFF));
        p.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
    }
    return p;
}

void put_f32(std::vector<std::uint8_t>& p, float v) {
    const auto bits = std::bit_cast<std::uint32_t>(v);
    p.push_back(static_cast<std::uint8_t>(bits & 0xFF));
    p.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFF));
    p.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFF));
    p.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFF));
}

}  // namespace

TEST_CASE("IR payload round-trips through WAV unchanged", "[ir]") {
    const auto payload = make_payload();
    REQUIRE(payload.size() == qc::ir::kPayloadBytes);

    const auto wav = qc::ir::payload_to_wav(payload);
    REQUIRE(wav.has_value());
    CHECK(wav->size() == qc::ir::kWavBytes);

    const auto back = qc::ir::wav_to_payload(*wav);
    REQUIRE(back.has_value());
    CHECK(*back == payload);
}

TEST_CASE("WAV header describes 48 kHz mono IEEE float32", "[ir]") {
    const auto wav = qc::ir::payload_to_wav(make_payload());
    REQUIRE(wav.has_value());
    const auto& w = *wav;
    CHECK(w[0] == 'R');
    CHECK(w[1] == 'I');
    CHECK(w[2] == 'F');
    CHECK(w[3] == 'F');
    CHECK(w[8] == 'W');
    CHECK(w[11] == 'E');
    // fmt-Chunk ab Offset 12: "fmt " + size(16) + Format-Tag 3, 1 Kanal, 48000 Hz
    CHECK(w[12] == 'f');
    CHECK(w[20] == 3);   // IEEE float
    CHECK(w[22] == 1);   // mono
    CHECK(w[24] == 0x80);  // 48000 = 0x0000BB80, little endian
    CHECK(w[25] == 0xBB);
}

TEST_CASE("payload validation rejects implausible input", "[ir]") {
    SECTION("wrong size") {
        const std::vector<std::uint8_t> short_one(100, 0);
        CHECK_FALSE(qc::ir::validate_payload(short_one).has_value());
    }
    SECTION("all zero is implausible") {
        const std::vector<std::uint8_t> zeros(qc::ir::kPayloadBytes, 0);
        CHECK_FALSE(qc::ir::validate_payload(zeros).has_value());
    }
    SECTION("out-of-range sample (typical for a wrong key)") {
        auto p = make_payload();
        p.resize(0);
        put_f32(p, 5.4e37F);  // wie bei falscher Seriennummer beobachtet
        while (p.size() < qc::ir::kPayloadBytes) put_f32(p, 0.5F);
        CHECK_FALSE(qc::ir::validate_payload(p).has_value());
    }
    SECTION("NaN sample") {
        auto p = make_payload();
        p.resize(0);
        put_f32(p, std::numeric_limits<float>::quiet_NaN());
        while (p.size() < qc::ir::kPayloadBytes) put_f32(p, 0.5F);
        CHECK_FALSE(qc::ir::validate_payload(p).has_value());
    }
}

TEST_CASE("wav_to_payload rejects non-WAVE input", "[ir]") {
    const std::vector<std::uint8_t> junk(64, 0x41);
    CHECK_FALSE(qc::ir::wav_to_payload(junk).has_value());
}
