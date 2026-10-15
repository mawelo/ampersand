// test_wav.cpp — gemeinsamer WAV-Codec: Rundlauf, Kopf-Felder, Ablehnungen
#include "qc/wav.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("WAV round-trips samples and rate", "[wav]") {
    const std::vector<float> in{0.0F, 0.5F, -0.5F, 1.0F, -1.0F};
    const auto wav = qc::wav::write(in, 44100);
    REQUIRE(wav.has_value());
    CHECK(wav->size() == qc::wav::kHeaderBytes + in.size() * 4);

    const auto back = qc::wav::read(*wav);
    REQUIRE(back.has_value());
    CHECK(back->sample_rate == 44100);
    CHECK(back->samples == in);
}

TEST_CASE("WAV header describes mono IEEE float32", "[wav]") {
    const auto wav = qc::wav::write(std::vector<float>{1.0F}, 48000);
    REQUIRE(wav.has_value());
    const auto& w = *wav;
    CHECK(w[0] == 'R');
    CHECK(w[8] == 'W');
    CHECK(w[20] == 3);     // IEEE float
    CHECK(w[22] == 1);     // mono
    CHECK(w[24] == 0x80);  // 48000 = 0x0000BB80
    CHECK(w[25] == 0xBB);
}

TEST_CASE("WAV writer and reader reject bad input", "[wav]") {
    SECTION("data length is not a whole number of samples") {
        const std::vector<std::uint8_t> odd(6, 0);
        CHECK_FALSE(qc::wav::write_f32le(odd, 48000).has_value());
    }
    SECTION("zero sample rate") {
        CHECK_FALSE(qc::wav::write(std::vector<float>{1.0F}, 0).has_value());
    }
    SECTION("not a RIFF file") {
        const std::vector<std::uint8_t> junk(64, 0x41);
        CHECK_FALSE(qc::wav::read(junk).has_value());
    }
}
