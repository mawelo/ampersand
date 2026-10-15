// test_catalog.cpp — ModelRepo-Parser (gzip(tar(xml)))
#include "qc/catalog.hpp"
#include "qc/compression.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace qc::catalog;

namespace {

const std::string kXml = R"xml(<?xml version="1.0"?>
<Models>
  <Category id="4" name="Equalizer">
    <Model id="4002" name="Graphic-9" tm="MXR 10 band">
      <Parameter name="LEVEL" min="-12" max="12" defaultValue="0" units="dB" type="float"/>
    </Model>
    <Model id="4005" name="Graphic-9" replaces="4002">
      <Parameter name="LEVEL" min="-12" max="12" defaultValue="0" units="dB" type="float"/>
      <Parameter name="MODE" min="0" max="2" defaultValue="0" type="comboBox" steps="3"/>
    </Model>
  </Category>
  <Category id="14" name="Neural Capture Internal" hidden="true">
    <Model id="14000" name="Capture" internal="true"/>
  </Category>
  <Category id="21" name="Cabsim Bass">
    <Model id="21005" name="212 Darkglass Neo (M)" sku="DG-1" plugin_id="darkglass">
      <Parameter name="MIXER LEVEL" min="0" max="1" defaultValue="0.769" units="dB" type="float"/>
    </Model>
  </Category>
</Models>)xml";

std::vector<std::uint8_t> make_tar(const std::string& name, const std::string& content) {
    std::vector<std::uint8_t> tar(512, 0);
    std::copy(name.begin(), name.end(), tar.begin());
    const std::string size = std::format("{:011o}", content.size());
    std::copy(size.begin(), size.end(), tar.begin() + 124);
    tar[156] = '0';
    const std::string magic = "ustar";
    std::copy(magic.begin(), magic.end(), tar.begin() + 257);
    tar.insert(tar.end(), content.begin(), content.end());
    tar.resize(((tar.size() + 511) / 512) * 512 + 1024, 0);
    return tar;
}

}  // namespace

TEST_CASE("bare xml parses into a catalog", "[catalog]") {
    auto cat = parse_model_repo(std::vector<std::uint8_t>(kXml.begin(), kXml.end()));
    REQUIRE(cat.has_value());
    CHECK(cat->size() == 4);
    const auto* eq = cat->find(4005);
    REQUIRE(eq != nullptr);
    CHECK(eq->name == "Graphic-9");
    CHECK(eq->category == "Equalizer");
    CHECK(eq->category_id == 4);
    REQUIRE(eq->parameters.size() == 2);
    CHECK(eq->parameters[1].type == "comboBox");
    CHECK(eq->parameters[1].option_count() == 3);
    CHECK(eq->parameter("level")->units == "dB");
    CHECK(eq->replaces == std::vector<int>{4002});
    CHECK(cat->find(4002)->superseded);
    CHECK_FALSE(eq->superseded);
    CHECK(eq->is_factory());
    CHECK(cat->find(14000)->is_capture_block());
    CHECK_FALSE(cat->find(14000)->is_factory());
    CHECK_FALSE(cat->find(21005)->is_factory());
    CHECK(cat->find(21005)->parameters[0].range_is_placeholder());
    CHECK(cat->categories().size() == 3);
    CHECK(cat->by_category("equalizer").size() == 2);
    CHECK(cat->search("darkglass").size() == 1);
    CHECK(cat->find_by_name("capture")->id == 14000);
}

TEST_CASE("gzip(tar(xml)) is the device's container", "[catalog]") {
    auto tar = make_tar("ModelRepo.xml", kXml);
    auto gz = qc::compression::gzip(tar);
    REQUIRE(gz.has_value());
    auto cat = parse_model_repo(*gz);
    REQUIRE(cat.has_value());
    CHECK(cat->find(21005)->name == "212 Darkglass Neo (M)");
    auto member = tar_extract(tar, ".xml");
    REQUIRE(member.has_value());
    CHECK(std::string(member->begin(), member->end()) == kXml);
    CHECK_FALSE(tar_extract(tar, ".json").has_value());
}

// Ein Modell nennt oft nur die Parameter, die es AENDERT, und erbt den Rest
// vom Modell in `clones`; `replaces="N"` sagt, welchen Eintrag es aendert.
// Ohne Aufloesung landen diese Parameter auf 0..N-1 - und dann heisst auf dem
// Draht alles falsch. Am Geraet nachgewiesen: Modell 12050 hat die Mikrofone
// auf 1 und 9, DISTANCE auf 4 und 12 (Beleg 115), waehrend wir bis zum
// 2026-09-16 "ir selector" auf 0 und "DISTANCE" auf 2 und 3 gemeldet haben.
const std::string kKlonXml = R"xml(<?xml version="1.0"?>
<Models>
  <Category id="12" name="Cabsim">
    <Model id="100" name="Vorlage">
      <Parameter name="BYPASS" min="0" max="1" defaultValue="0" type="switch"/>
      <Parameter name="IR" min="0" max="999" defaultValue="" type="string"/>
      <Parameter name="LEVEL" min="-12" max="12" defaultValue="0" units="dB" type="float"/>
      <Parameter name="DISTANCE" min="0" max="1" defaultValue="0.1" type="float"/>
    </Model>
    <Model id="101" name="Klon" clones="100">
      <Parameter name="IR B" replaces="1" min="0" max="999" defaultValue="" type="string"/>
      <Parameter name="DISTANCE B" replaces="3" min="0" max="1" defaultValue="0.0" type="float"/>
      <Parameter name="EXTRA" min="0" max="1" defaultValue="0" type="float"/>
    </Model>
    <Model id="102" name="Kette" clones="101">
      <Parameter name="LEVEL C" replaces="2" min="-40" max="6" defaultValue="-6" units="dB" type="float"/>
    </Model>
    <Model id="103" name="Ausserhalb" clones="100">
      <Parameter name="ZU HOCH" replaces="99" min="0" max="1" defaultValue="0" type="float"/>
    </Model>
    <Model id="104" name="Zyklus A" clones="105">
      <Parameter name="A" min="0" max="1" defaultValue="0" type="float"/>
    </Model>
    <Model id="105" name="Zyklus B" clones="104">
      <Parameter name="B" min="0" max="1" defaultValue="0" type="float"/>
    </Model>
    <Model id="106" name="Leerer Klon" clones="100"/>
  </Category>
</Models>)xml";

TEST_CASE("clones/replaces: geerbte Parameterliste wird aufgeloest", "[catalog]") {
    auto cat = parse_model_repo(std::vector<std::uint8_t>(kKlonXml.begin(), kKlonXml.end()));
    REQUIRE(cat.has_value());

    SECTION("der Klon ersetzt an der richtigen Stelle und haengt den Rest an") {
        const auto* m = cat->find(101);
        REQUIRE(m != nullptr);
        REQUIRE(m->parameters.size() == 5);
        CHECK(m->parameters[0].name == "BYPASS");      // geerbt
        CHECK(m->parameters[1].name == "IR B");        // replaces=1
        CHECK(m->parameters[2].name == "LEVEL");       // geerbt
        CHECK(m->parameters[3].name == "DISTANCE B");  // replaces=3
        CHECK(m->parameters[4].name == "EXTRA");       // ohne replaces: angehaengt
        // Der Index muss nach der Aufloesung der Position entsprechen -
        // Model::parameter(int) greift ueber die Position zu.
        for (int i = 0; i < 5; ++i) CHECK(m->parameters[static_cast<std::size_t>(i)].index == i);
        CHECK(m->parameter(3) != nullptr);
        CHECK(m->parameter(3)->name == "DISTANCE B");
        // Und der geerbte Wertebereich kommt mit, nicht nur der Name.
        CHECK(m->parameters[2].minimum == -12.0f);
    }

    SECTION("eine Kette von Klonen") {
        const auto* m = cat->find(102);
        REQUIRE(m != nullptr);
        REQUIRE(m->parameters.size() == 5);
        CHECK(m->parameters[1].name == "IR B");     // aus 101
        CHECK(m->parameters[2].name == "LEVEL C");  // eigen
        CHECK(m->parameters[4].name == "EXTRA");    // aus 101
    }

    SECTION("replaces ausserhalb der geerbten Liste wird angehaengt, nicht verworfen") {
        const auto* m = cat->find(103);
        REQUIRE(m != nullptr);
        REQUIRE(m->parameters.size() == 5);
        CHECK(m->parameters[4].name == "ZU HOCH");
        CHECK(m->parameters[0].name == "BYPASS");
    }

    SECTION("ein Zyklus haengt nicht und laesst die eigene Liste stehen") {
        const auto* a = cat->find(104);
        const auto* b = cat->find(105);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->parameters.size() >= 1);
        CHECK(b->parameters.size() >= 1);
    }

    SECTION("ein Klon ohne eigene Parameter erbt die ganze Liste") {
        const auto* m = cat->find(106);
        REQUIRE(m != nullptr);
        REQUIRE(m->parameters.size() == 4);
        CHECK(m->parameters[3].name == "DISTANCE");
    }

    SECTION("ein Modell ohne clones bleibt unangetastet") {
        const auto* m = cat->find(100);
        REQUIRE(m != nullptr);
        CHECK(m->parameters.size() == 4);
        CHECK(m->parameters[1].name == "IR");
    }
}

TEST_CASE("parameter conversions", "[catalog]") {
    Parameter p{0, "THRESHOLD", -60.0f, 12.0f, 0.0f, "dB", "float", std::nullopt, std::nullopt};
    CHECK(p.to_normalized(12.0f) == 1.0f);
    CHECK(p.to_normalized(-60.0f) == 0.0f);
    CHECK(p.to_real(0.5f) == -24.0f);
    CHECK(p.to_normalized(100.0f) == 1.0f);  // clamped
    Parameter ph{0, "MIXER LEVEL", 0.0f, 1.0f, 0.77f, "dB", "float", std::nullopt, std::nullopt};
    CHECK(ph.range_is_placeholder());
    CHECK(ph.to_real(0.3f) == 0.3f);
    Parameter m{0, "GAIN REDUCTION", -40.0f, 0.0f, 0.0f, "dB", "grMeter", std::nullopt, std::nullopt};
    CHECK(m.is_meter());
}

TEST_CASE("garbage is rejected", "[catalog]") {
    std::vector<std::uint8_t> junk{1, 2, 3, 4};
    CHECK_FALSE(parse_model_repo(junk).has_value());
    std::string empty_xml = "<Models></Models>";
    CHECK_FALSE(parse_model_repo(std::vector<std::uint8_t>(empty_xml.begin(), empty_xml.end())).has_value());
}
