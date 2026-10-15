// test_preset_view.cpp — Slot-Arithmetik und Grid-Sicht
#include "qc/preset_view.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace qc::view;

TEST_CASE("slot names <-> positions", "[view]") {
    CHECK(slot_to_position("28C").value() == 218);
    CHECK(slot_to_position("28E").value() == 220);
    CHECK(slot_to_position("1A").value() == 0);
    CHECK(slot_to_position("32H").value() == 255);
    CHECK(slot_to_position("30a").value() == 232);
    CHECK(slot_to_position("218").value() == 218);
    CHECK_FALSE(slot_to_position("33A").has_value());
    CHECK_FALSE(slot_to_position("0A").has_value());
    CHECK_FALSE(slot_to_position("28I").has_value());
    CHECK_FALSE(slot_to_position("").has_value());
    CHECK_FALSE(slot_to_position("256").has_value());
    CHECK(position_to_slot(218) == "28C");
    CHECK(position_to_slot(0) == "1A");
    CHECK(scene_letter(3) == "D");
}

TEST_CASE("folder helpers", "[view]") {
    CHECK(same_folder("/opt/neuraldsp/Factory Library/", "/opt/neuraldsp/Factory Library"));
    CHECK(is_factory_setlist(kFactorySetlist));
    CHECK_FALSE(is_factory_setlist(kUserSetlist));
    CHECK(input_port_name(4) == "Return 1");
    CHECK(output_port_name(19) == "Multi Out");
    CHECK(output_port_name(99) == "out 99");
    CHECK(capture_display_name(std::string(64, 'a') + "Darkglass VMT 1") == "Darkglass VMT 1");
    CHECK(capture_display_name("plain") == "plain");
}

TEST_CASE("build() follows the positional wire conventions", "[view]") {
    qc::catalog::Catalog cat;
    qc::catalog::Model amp;
    amp.id = 3001;
    amp.name = "Brit 2203";
    amp.category = "Amplifier";
    amp.parameters = {{0, "GAIN", 0.0f, 10.0f, 5.0f, "", "float", std::nullopt, std::nullopt},
                      {1, "MASTER", 0.0f, 10.0f, 5.0f, "", "float", std::nullopt, std::nullopt}};
    cat.add(amp);
    qc::catalog::Model lane;
    lane.id = 23000;
    lane.name = "Lane Output Control";
    lane.parameters = {{0, "VOLUME", 0.0f, 1.0f, 0.77f, "dB", "float", std::nullopt, std::nullopt}};
    cat.add(lane);
    cat.finalize();

    ::BinaryPreset p;
    p.set_name("Test");
    p.set_author_name("me");
    p.set_default_scene(2);
    for (int i = 0; i < 8; ++i) {
        p.add_scene_labels(i == 0 ? "Clean" : " ");
        p.add_scene_colors(0xFF000000u + static_cast<std::uint32_t>(i));
    }
    // Row 0 (no explicit row): input 1, 8 model slots, block at column 1.
    auto* c0 = p.add_chains();
    c0->set_in_portid(1);
    c0->set_out_portid(19);
    for (int col = 0; col < 8; ++col) {
        auto* m = c0->add_models();
        if (col == 1) {
            m->set_hash(3001);
            auto* g = m->add_params();  // index 0 GAIN, positional
            g->add_param_values()->set_float_value(0.5f);
            auto* master = m->add_params();  // index 1, scene-following
            master->set_scene_mode(true);
            for (int s = 0; s < 8; ++s) master->add_param_values()->set_float_value(0.1f * static_cast<float>(s));
        }
    }
    auto* oc = c0->add_output_control();
    oc->set_hash(23000);
    oc->add_params()->add_param_values()->set_float_value(0.769f);
    auto* scp = c0->add_split_control_points();
    scp->set_split(-1);
    scp->set_mix(-1);
    // Rows 1..3 empty.
    for (int r = 1; r < 4; ++r) {
        auto* c = p.add_chains();
        for (int col = 0; col < 8; ++col) c->add_models();
    }
    // Bypass table: row 0 col 1 scene-mode, bypassed in scene 3 only.
    for (int r = 0; r < 4; ++r) {
        auto* b = p.add_bypass();
        for (int col = 0; col < 8; ++col) {
            auto* cb = b->add_colbypass();
            if (r == 0 && col == 1) {
                cb->set_scenemode(true);
                for (int s = 0; s < 8; ++s) cb->add_scenebypass()->set_bypass(s == 3);
            }
        }
    }
    auto* tempo = p.add_tempoprogramdata();
    tempo->add_params()->add_param_values()->set_float_value(0.4f);  // 120 bpm

    auto v0 = build(p, &cat, 0);
    CHECK(v0.name == "Test");
    CHECK(v0.default_scene == 2);
    REQUIRE(v0.rows.size() == 4);
    CHECK(v0.rows[0].in_name == "Input 1");
    CHECK(v0.rows[0].out_name == "Multi Out");
    REQUIRE(v0.rows[0].blocks.size() == 1);
    const auto& b = v0.rows[0].blocks[0];
    CHECK(b.column == 1);
    CHECK(b.name == "Brit 2203");
    CHECK_FALSE(b.bypassed);
    CHECK(b.scene_mode_bypass);
    REQUIRE(b.params.size() == 2);
    CHECK(b.params[0].name == "GAIN");
    CHECK(b.params[0].value.value() == 0.5f);
    CHECK(b.params[0].real_value().value() == 5.0f);
    CHECK(b.params[1].scene_mode);
    CHECK(b.params[1].value.value() == 0.0f);
    CHECK_FALSE(v0.rows[0].split.has_value());
    REQUIRE(v0.rows[0].output_control.size() == 1);
    CHECK(v0.rows[0].output_control[0].name == "VOLUME");
    CHECK(v0.rows[0].output_control[0].placeholder_range);
    CHECK(v0.tempo_bpm.value() == 120.0f);
    CHECK(v0.scene_labels[0] == "Clean");
    CHECK(v0.rows[1].blocks.empty());

    auto v3 = build(p, &cat, 3);
    CHECK(v3.rows[0].blocks[0].bypassed);
    CHECK(v3.rows[0].blocks[0].params[1].value.value() > 0.29f);

    auto v_nocat = build(p, nullptr, 0);
    CHECK(v_nocat.rows[0].blocks[0].name == "#3001");

    auto j = to_json(v3);
    CHECK(j["rows"][0]["blocks"][0]["bypassed"] == true);
    CHECK(j["rows"][0]["blocks"][0]["params"][0]["display"] == "5");
}

TEST_CASE("explicit row/column keys are honoured (grid echoes)", "[view]") {
    ::BinaryPreset p;
    auto* c = p.add_chains();
    c->set_row(2);
    auto* m = c->add_models();
    m->set_column(5);
    m->set_hash(4005);
    auto v = build(p, nullptr, 0);
    CHECK(v.rows[2].blocks.size() == 1);
    CHECK(v.rows[2].blocks[0].column == 5);
    CHECK(v.rows[0].blocks.empty());
}

TEST_CASE("chain_report orders items along the signal path and expands a branch", "[view]") {
    ::BinaryPreset p;
    p.set_name("Branchy");
    auto* c0 = p.add_chains();
    c0->set_in_portid(1);
    c0->set_out_portid(19);
    for (int col = 0; col < 8; ++col) {
        auto* m = c0->add_models();
        if (col == 0 || col == 3 || col == 6) {
            m->set_hash(4005);
            m->add_params()->add_param_values()->set_float_value(0.5f);
        }
    }
    auto* scp = c0->add_split_control_points();
    scp->set_split(2);
    scp->set_mix(4);
    auto* c1 = p.add_chains();  // lane row
    for (int col = 0; col < 8; ++col) {
        auto* m = c1->add_models();
        if (col == 3) m->set_hash(4002);
    }
    for (int r = 2; r < 4; ++r) p.add_chains();
    auto rep = chain_report(build(p, nullptr, 0));
    REQUIRE(rep["chains"].size() == 1);
    const auto& items = rep["chains"][0]["items"];
    REQUIRE(items.size() == 3);
    CHECK(items[0]["item"] == "#4005");
    CHECK(items[0]["column"] == 1);
    CHECK(items[1]["type"] == "parallel");
    CHECK(items[1]["split_column"] == 3);
    CHECK(items[1]["mix_column"] == 5);
    CHECK(items[1]["path_a"].size() == 1);
    CHECK(items[1]["path_b"].size() == 1);
    CHECK(items[1]["path_b"][0]["item"] == "#4002");
    CHECK(items[2]["column"] == 7);
    CHECK(rep["chains"][0]["output"] == "Multi Out");
    CHECK(items[0]["settings"][0]["normalized"] == 0.5f);
}

TEST_CASE("rows and columns are 1-based at every boundary", "[view]") {
    // The unit labels ROW 1..4; export, JSON, CLI and MCP must all say the same,
    // otherwise an address copied from one lands one cell off in the other.
    using namespace qc::view;

    CHECK(row_from_user(1).value() == 0);
    CHECK(row_from_user(4).value() == 3);
    CHECK(column_from_user(1).value() == 0);
    CHECK(column_from_user(8).value() == 7);
    CHECK(row_to_user(0) == 1);
    CHECK(column_to_user(7) == 8);

    // Out of range says which counting is meant — 0 is the tempting mistake.
    REQUIRE_FALSE(row_from_user(0).has_value());
    CHECK(row_from_user(0).error().contains("rows are numbered 1 to 4"));
    CHECK_FALSE(row_from_user(5).has_value());
    REQUIRE_FALSE(column_from_user(0).has_value());
    CHECK(column_from_user(0).error().contains("columns are numbered 1 to 8"));
    CHECK_FALSE(column_from_user(9).has_value());
}

TEST_CASE("the JSON export counts like the unit", "[view]") {
    ::BinaryPreset p;
    auto* chain = p.add_chains();
    chain->set_in_portid(1);
    for (int i = 0; i < qc::view::kColumns; ++i) {
        auto* m = chain->add_models();
        if (i == 2) m->set_hash(4005);  // library index 2 -> column 3 for a reader
    }

    auto j = to_json(build(p, nullptr, 0));
    CHECK(j["rows"][0]["row"] == 1);                     // was 0
    CHECK(j["rows"][0]["blocks"][0]["column"] == 3);     // was 2
    CHECK(std::string(j["note"]).contains("1-based"));
}
