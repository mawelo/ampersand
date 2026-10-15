// test_pitch.cpp — Tonhoehe aus synthetischen Signalen, ohne Hardware
#include "qc/pitch.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace {

/// Ton mit Obertoenen, wie ihn eine gezupfte Saite liefert — ein reiner Sinus
/// waere zu freundlich und wuerde die Oktav-/Quintfalle gar nicht erst stellen.
std::vector<float> saite(double f0, int rate, double seconds, double decay = 2.0) {
    std::vector<float> out(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double huelle = std::exp(-decay * t);
        double v = 0.0;
        for (int h = 1; h <= 6; ++h) {
            v += std::sin(2.0 * std::numbers::pi * f0 * h * t) / h;
        }
        out[i] = static_cast<float>(0.3 * huelle * v);
    }
    return out;
}

}  // namespace

TEST_CASE("detect finds the fundamental of a plucked string, not a harmonic", "[pitch]") {
    // 61.74 Hz is the open B string the unit was measured against — low enough
    // that plain autocorrelation reads the fifth above it (measured: 93 Hz).
    for (const double f0 : {61.74, 82.41, 110.0, 220.0, 440.0}) {
        auto s = saite(f0, 48000, 0.5);
        auto r = qc::pitch::detect(s, 48000);
        INFO("f0 = " << f0 << ", gemessen " << r.frequency_hz << ", Guete " << r.aperiodicity);
        REQUIRE(r.reliable());
        CHECK_THAT(r.frequency_hz, Catch::Matchers::WithinRel(f0, 0.005));
    }
}

TEST_CASE("detect reports silence as unusable rather than inventing a pitch", "[pitch]") {
    std::vector<float> stille(24000, 0.0f);
    auto r = qc::pitch::detect(stille, 48000);
    CHECK_FALSE(r.reliable());
    CHECK(r.rms == 0.0);
}

TEST_CASE("detect marks noise as unreliable", "[pitch]") {
    std::vector<float> rauschen(24000);
    unsigned state = 12345;
    for (auto& v : rauschen) {
        state = state * 1103515245u + 12345u;
        v = static_cast<float>(static_cast<int>((state >> 16) & 0x7FFF) - 16384) / 32768.0f;
    }
    auto r = qc::pitch::detect(rauschen, 48000);
    CHECK_FALSE(r.reliable());   // die Guete schlaegt an, nicht der Pegel
    CHECK(r.rms > 0.0);
}

TEST_CASE("nearest_note names the note and the deviation in cents", "[pitch]") {
    auto a4 = qc::pitch::nearest_note(440.0);
    CHECK(a4.name == "A4");
    CHECK(a4.midi == 69);
    CHECK_THAT(a4.cents, Catch::Matchers::WithinAbs(0.0, 0.001));

    // Die am Geraet gemessene leere B-Saite: 61,85 Hz waren dort +3,2 Cent.
    auto b1 = qc::pitch::nearest_note(61.85);
    CHECK(b1.name == "B1");
    CHECK_THAT(b1.cents, Catch::Matchers::WithinAbs(3.2, 0.3));

    // Ein Halbton hoeher muss auch als naechste Note kippen.
    CHECK(qc::pitch::nearest_note(65.41).name == "C2");
    CHECK(qc::pitch::nearest_note(0.0).name.empty());
}
