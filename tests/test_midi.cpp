// test_midi.cpp — die drei Bytes einer Control-Change-Nachricht
#include "qc/midi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

/// Schreibt in eine Datei statt in einen Geraeteknoten — so laesst sich pruefen,
/// was auf die Leitung ginge, ohne Hardware.
std::vector<unsigned char> gesendet(const std::function<void(qc::midi::Port&)>& tun) {
    const auto pfad = std::filesystem::temp_directory_path() / "qc_midi_test.bin";
    std::filesystem::remove(pfad);
    { std::ofstream leer(pfad); }
    {
        auto port = qc::midi::Port::open(pfad.string());
        REQUIRE(port.has_value());
        tun(*port);
    }
    std::ifstream in(pfad, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::filesystem::remove(pfad);
    return bytes;
}

}  // namespace

TEST_CASE("tuner(true) sends CC#45 with 127, tuner(false) with 0", "[midi]") {
    // Measured on the unit: 0-63 closes the tuner menu, 64-127 opens it
    // (manual, docs/evidence/84). Status byte 0xB0 = control change, channel 1.
    auto auf = gesendet([](qc::midi::Port& p) { REQUIRE(p.tuner(true).has_value()); });
    REQUIRE(auf.size() == 3);
    CHECK(auf[0] == 0xB0);
    CHECK(auf[1] == 45);
    CHECK(auf[2] == 127);

    auto zu = gesendet([](qc::midi::Port& p) { REQUIRE(p.tuner(false).has_value()); });
    REQUIRE(zu.size() == 3);
    CHECK(zu[2] == 0);
}

TEST_CASE("control_change rejects values outside the MIDI range", "[midi]") {
    auto nichts = gesendet([](qc::midi::Port& p) {
        CHECK_FALSE(p.control_change(128, 0).has_value());
        CHECK_FALSE(p.control_change(45, 128).has_value());
        CHECK_FALSE(p.control_change(45, 0, 16).has_value());
        CHECK(p.control_change(45, 0, 15).has_value());  // gerade noch gueltig
    });
    REQUIRE(nichts.size() == 3);
    CHECK(nichts[0] == 0xBF);  // Kanal 16 = 0xB0 | 15
}
