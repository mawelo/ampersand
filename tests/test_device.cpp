// test_device.cpp — Wire-Shapes der QuadCortex-API (offline)
#include "qc/device.hpp"

#include "fake_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace pa = cortex_protobuf_v2;
using namespace std::chrono_literals;
using qc::MessageType;

namespace {

struct Rig {
    qc::test::FakeTransport* t{nullptr};
    std::unique_ptr<qc::QuadCortex> qc;
    Rig() {
        auto transport = std::make_unique<qc::test::FakeTransport>();
        t = transport.get();
        qc::Session::Options o;
        o.read_timeout = 20ms;
        o.keepalive_interval = 10s;
        qc = std::make_unique<qc::QuadCortex>(std::move(transport), o);
    }
    ::BinaryPreset last_grid(int expected_action = 1) {
        auto m = t->last_of(MessageType::Grid);
        REQUIRE(m.has_value());
        auto g = m->parse<pa::GridMessage>();
        REQUIRE(g.has_value());
        CHECK(static_cast<int>(g->action()) == expected_action);
        return g->preset();
    }
};

}  // namespace

TEST_CASE("recall_preset: SetlistPosition UPDATE with folder key, position, is_factory", "[device]") {
    Rig r;
    REQUIRE(r.qc->recall_preset(qc::view::kFactorySetlist, 218).has_value());
    auto m = r.t->last_of(MessageType::SetlistPosition)->parse<pa::SetlistPositionMessage>();
    CHECK(m->action() == pa::MessageAction::UPDATE);
    CHECK(m->folder_key() == "/opt/neuraldsp/Factory Library/");
    CHECK(m->position() == 218);
    CHECK(m->is_factory());
    REQUIRE(r.qc->recall_preset(qc::view::kUserSetlist, 0).has_value());
    CHECK_FALSE(r.t->last_of(MessageType::SetlistPosition)->parse<pa::SetlistPositionMessage>()->is_factory());
    CHECK_FALSE(r.qc->recall_preset(qc::view::kUserSetlist, 256).has_value());
}

TEST_CASE("scene messages", "[device]") {
    Rig r;
    REQUIRE(r.qc->switch_scene(1).has_value());
    auto s = r.t->last_of(MessageType::Scene)->parse<pa::SceneMessage>();
    CHECK(s->selected_scene() == 1);
    CHECK_FALSE(r.qc->switch_scene(8).has_value());
    REQUIRE(r.qc->set_scene_label(2, "").has_value());
    CHECK(r.t->last_of(MessageType::SceneLabel)->parse<pa::SceneLabelMessage>()->label() == " ");
    REQUIRE(r.qc->set_scene_color(3, 0xFFFF02C2u).has_value());
    CHECK(r.t->last_of(MessageType::SceneColor)->parse<pa::SceneColorMessage>()->color() == 0xFFFF02C2u);
    REQUIRE(r.qc->copy_scene(1, 3, true).has_value());
    auto c = r.t->last_of(MessageType::SceneCopy)->parse<pa::SceneCopyMessage>();
    CHECK(c->action() == pa::MessageAction::UPDATE);
    CHECK(c->from_index() == 1);
    CHECK(c->to_index() == 3);
    CHECK(c->is_swap());
}

TEST_CASE("set_param is a row/column keyed sparse Grid UPDATE", "[device]") {
    Rig r;
    REQUIRE(r.qc->set_param(0, 1, 1, 0.4553f, {}, false).has_value());  // shape only, no read-back
    auto p = r.last_grid();
    REQUIRE(p.chains_size() == 1);
    CHECK(p.chains(0).row() == 0);
    CHECK(p.chains(0).has_row());
    REQUIRE(p.chains(0).models_size() == 1);
    CHECK(p.chains(0).models(0).column() == 1);
    CHECK_FALSE(p.chains(0).models(0).has_hash());
    REQUIRE(p.chains(0).models(0).params_size() == 1);
    CHECK(p.chains(0).models(0).params(0).index() == 1);
    CHECK(p.chains(0).models(0).params(0).param_values(0).float_value() == 0.4553f);
    CHECK_FALSE(p.chains(0).models(0).params(0).has_scene_mode());
    CHECK(p.bypass_size() == 0);
}

TEST_CASE("per-scene set_param promotes ALONE, switches scene, then writes", "[device]") {
    Rig r;
    REQUIRE(r.qc->set_param(2, 5, 0, 0.8f, 3, false).has_value());  // shape only, no read-back
    auto msgs = r.t->host_messages();
    REQUIRE(msgs.size() == 3);
    auto promote = msgs[0].parse<pa::GridMessage>();
    CHECK(promote->preset().chains(0).models(0).params(0).scene_mode());
    CHECK(promote->preset().chains(0).models(0).params(0).param_values_size() == 0);
    CHECK(msgs[1].type == MessageType::Scene);
    CHECK(msgs[1].parse<pa::SceneMessage>()->selected_scene() == 3);
    auto write = msgs[2].parse<pa::GridMessage>();
    CHECK(write->preset().chains(0).models(0).params(0).param_values(0).float_value() == 0.8f);
    CHECK_FALSE(write->preset().chains(0).models(0).params(0).has_scene_mode());
}

TEST_CASE("set_bypass carries only the bypass group, index 0 = active scene", "[device]") {
    Rig r;
    REQUIRE(r.qc->set_bypass(0, 4, true).has_value());
    auto p = r.last_grid();
    CHECK(p.chains_size() == 0);
    REQUIRE(p.bypass_size() == 1);
    CHECK(p.bypass(0).row() == 0);
    REQUIRE(p.bypass(0).colbypass_size() == 1);
    CHECK(p.bypass(0).colbypass(0).column() == 4);
    CHECK_FALSE(p.bypass(0).colbypass(0).scenemode());
    REQUIRE(p.bypass(0).colbypass(0).scenebypass_size() == 1);
    CHECK(p.bypass(0).colbypass(0).scenebypass(0).bypass());
}

TEST_CASE("set_block verifies the Grid echo; remove_block uses DELETE", "[device]") {
    Rig r;
    r.t->set_responder([&](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::Grid) return;
        auto g = m.parse<pa::GridMessage>();
        const auto& mdl = g->preset().chains(0).models(0);
        if (mdl.hash() == 4005) r.t->push(MessageType::Grid, *g);  // accepted -> echo
        // 21005 is "refused": no echo at all.
    });
    REQUIRE(r.qc->set_block(1, 0, 4005, true, 500ms).has_value());
    // No echo AND no way to read the preset back (this stand-in answers neither):
    // the message says exactly that instead of guessing at a cause.
    auto refused = r.qc->set_block(1, 1, 21005, true, 200ms);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("no Grid echo"));
    CHECK(refused.error().contains("reading the preset back failed"));
    REQUIRE(r.qc->set_block(1, 2, 21005, false).has_value());
    CHECK_FALSE(r.qc->set_block(1, 2, 0).has_value());
    REQUIRE(r.qc->remove_block(1, 0).has_value());
    auto p = r.last_grid(2);
    CHECK(p.chains(0).models(0).has_hash());
    CHECK(p.chains(0).models(0).hash() == 0);
}

TEST_CASE("routing, split, lane output, tempo", "[device]") {
    Rig r;
    REQUIRE(r.qc->set_chain_input(0, 4).has_value());
    CHECK(r.last_grid().chains(0).in_portid() == 4);
    REQUIRE(r.qc->set_chain_output(2, 19).has_value());
    CHECK(r.last_grid().chains(0).out_portid() == 19);
    CHECK_FALSE(r.qc->set_split(1, 3, 5).has_value());
    REQUIRE(r.qc->set_split(0, 3, -1).has_value());
    auto sp = r.last_grid().chains(0).split_control_points(0);
    CHECK(sp.split() == 3);
    CHECK(sp.mix() == -1);
    REQUIRE(r.qc->set_lane_output(3, 0, 0.0f).has_value());
    CHECK(r.last_grid().chains(0).output_control(0).hash() == 23000);
    REQUIRE(r.qc->set_tempo_bpm(120.0f).has_value());
    auto tp = r.last_grid();
    CHECK(tp.tempoprogramdata(0).hash() == 25000);
    CHECK(tp.tempoprogramdata(0).params(0).param_values(0).float_value() == 0.4f);
    CHECK_FALSE(r.qc->set_tempo_bpm(300.0f).has_value());
    REQUIRE(r.qc->move_block(0, 6, 1, 6).has_value());
    auto mv = r.t->last_of(MessageType::GridMove)->parse<pa::GridMoveMessage>();
    CHECK(mv->move(0).to_row() == 1);
    CHECK(mv->move(0).is_drop());
}

TEST_CASE("file operations", "[device]") {
    Rig r;
    REQUIRE(r.qc->save_current_preset(qc::view::kUserSetlist, 220, "My Preset", 2).has_value());
    auto f = r.t->last_of(MessageType::File)->parse<pa::FileMessage>();
    CHECK(f->action() == pa::MessageAction::CREATE);
    CHECK(f->type() == 0);
    CHECK(f->folder().key() == "/media/p4/Presets/My Presets");
    CHECK_FALSE(f->folder().is_factory());
    CHECK(f->folder().files(0).index() == 220);
    CHECK(f->folder().files(0).name() == "My Preset");
    CHECK(f->folder().files(0).instrument() == 2);
    CHECK_FALSE(f->has_preset_payload());
    CHECK_FALSE(r.qc->save_current_preset(qc::view::kFactorySetlist, 1, "x").has_value());

    REQUIRE(r.qc->delete_preset(qc::view::kUserSetlist, "My Preset").has_value());
    auto d = r.t->last_of(MessageType::File)->parse<pa::FileMessage>();
    CHECK(d->action() == pa::MessageAction::DELETE);
    CHECK(d->folder().files(0).key() == "/media/p4/Presets/My Presets/My Preset.pb");

    REQUIRE(r.qc->move_preset(qc::view::kUserSetlist, "My Preset", 219).has_value());
    auto mv = r.t->last_of(MessageType::File)->parse<pa::FileMessage>();
    CHECK(mv->action() == pa::MessageAction::MOVE);
    CHECK(mv->to_folder().files(0).index() == 219);
}

TEST_CASE("read_current_preset correlates on request_id; seed pushes are ignored", "[device]") {
    Rig r;
    r.t->set_responder([&](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::RecallPreset) return;
        auto req = m.parse<pa::RecallPresetMessage>();
        pa::RecallPresetMessage seed;  // no request_id
        seed.mutable_preset()->set_name("SEED");
        r.t->push(MessageType::RecallPreset, seed);
        pa::RecallPresetMessage reply;
        reply.set_request_id(req->request_id());
        reply.mutable_preset()->set_name("LIVE");
        r.t->push(MessageType::RecallPreset, reply);
    });
    auto p = r.qc->read_current_preset(1000ms);
    REQUIRE(p.has_value());
    CHECK(p->name() == "LIVE");
}

TEST_CASE("list_presets filters empty slots and matches keys without trailing slash", "[device]") {
    Rig r;
    r.t->set_responder([&](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::File) return;
        pa::FileMessage other;
        other.mutable_folder()->set_key("106_f");
        other.mutable_folder()->add_files()->set_name("VMT");
        r.t->push(MessageType::File, other);
        pa::FileMessage factory;
        factory.mutable_folder()->set_key("/opt/neuraldsp/Factory Library");
        for (int i = 0; i < 4; ++i) {
            auto* f = factory.mutable_folder()->add_files();
            f->set_index(i);
            if (i != 2) f->set_name(std::format("P{}", i));
        }
        r.t->push(MessageType::File, factory);
    });
    auto list = r.qc->list_presets(qc::view::kFactorySetlist, false, 1000ms);
    REQUIRE(list.has_value());
    REQUIRE(list->size() == 3);
    CHECK((*list)[1].slot == "1B");
    auto found = r.qc->find_preset("P3", qc::view::kFactorySetlist, 1000ms);
    REQUIRE(found.has_value());
    CHECK(found->index == 3);
    CHECK_FALSE(r.qc->find_preset("nope", qc::view::kFactorySetlist, 1000ms).has_value());
}

TEST_CASE("events are emitted for device pushes", "[device]") {
    Rig r;
    std::atomic<int> scenes{-1};
    std::atomic<bool> recalled{false};
    r.qc->set_event_sink([&](const qc::Event& e) {
        if (const auto* s = std::get_if<qc::event::SceneSelected>(&e)) scenes = s->scene;
        if (std::get_if<qc::event::PresetRecalled>(&e)) recalled = true;
    });
    pa::SceneMessage sc;
    sc.set_selected_scene(6);
    r.t->push(MessageType::Scene, sc);
    pa::RecallPresetMessage rp;
    rp.mutable_preset()->set_name("x");
    r.t->push(MessageType::RecallPreset, rp);
    for (int i = 0; i < 100 && !(scenes == 6 && recalled); ++i) std::this_thread::sleep_for(10ms);
    CHECK(scenes == 6);
    CHECK(recalled);
}

TEST_CASE("read_version answers from the handshake, without asking twice", "[device]") {
    // The unit answers a Version READ in full only once. Everything after that
    // must come out of what the session already knows, otherwise the second
    // caller waits for a reply that never arrives (bug seen 2026-09-10:
    // `qcctl coros` failed while `qcctl version` worked).
    Rig r;
    int full_answers = 0;
    r.t->set_responder([&](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::Version) return;
        auto req = m.parse<pa::VersionMessage>();
        if (!req || req->action() != pa::MessageAction::READ) return;
        if (full_answers++ > 0) return;  // as the unit does: silent from here on
        pa::VersionMessage v;
        v.set_action(pa::MessageAction::UPDATE);
        v.set_app_fw_version("d14e");
        v.set_device_serial_number("QCXXXXXXXXX");
        v.set_zenos_git_hash("4.1.0");
        r.t->push(MessageType::Version, v);
    });

    auto first = r.qc->read_version(1000ms);
    REQUIRE(first.has_value());
    CHECK(first->app_fw_version == "d14e");
    const auto asked_once = r.t->count_of(MessageType::Version);

    auto second = r.qc->read_version(200ms);
    REQUIRE(second.has_value());  // used to time out
    CHECK(second->zenos_git_hash == "4.1.0");
    CHECK(r.t->count_of(MessageType::Version) == asked_once);  // nothing sent again
}

namespace {

/// A unit that stays silent on Grid but answers a preset read — the shape seen
/// on the real device, where the echo takes 10 to 13 s or never arrives.
void answer_preset_only(qc::test::FakeTransport* t, std::optional<std::uint32_t> model_at_r3c0) {
    t->set_responder([t, model_at_r3c0](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::RecallPreset) return;  // Grid: no echo, ever
        auto req = m.parse<pa::RecallPresetMessage>();
        if (!req || req->action() != pa::MessageAction::READ) return;
        pa::RecallPresetMessage reply;
        if (req->has_request_id()) reply.set_request_id(req->request_id());
        auto* preset = reply.mutable_preset();
        for (int row = 0; row < qc::view::kRows; ++row) {
            auto* chain = preset->add_chains();
            chain->set_row(static_cast<std::uint32_t>(row));
            for (int col = 0; col < qc::view::kColumns; ++col) {
                auto* model = chain->add_models();
                model->set_column(static_cast<std::uint32_t>(col));
                if (row == 3 && col == 0 && model_at_r3c0) model->set_hash(*model_at_r3c0);
            }
        }
        t->push(MessageType::RecallPreset, reply);
    });
}

}  // namespace

TEST_CASE("set_block believes the grid, not the missing echo", "[device]") {
    // The unit rebuilds its DSP chain before echoing and sometimes never echoes
    // at all, while the block sits perfectly well (measured 2026-09-10:
    // 10,4 s and 12,52 s). Reporting a refusal there sent the caller looking
    // for a fault that was not one.
    Rig r;
    answer_preset_only(r.t, 14000);
    CHECK(r.qc->set_block(3, 0, 14000, true, 100ms).has_value());
}

TEST_CASE("set_block still reports a cell the block never reached", "[device]") {
    Rig r;
    answer_preset_only(r.t, std::nullopt);  // no echo AND the cell stays empty
    auto res = r.qc->set_block(3, 0, 14000, true, 100ms);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().contains("row 4 column 1"));  // reported as the unit labels it
    CHECK(res.error().contains("the cell is empty"));
}

namespace {

/// A unit whose cell holds `stored` for parameter 0 — or nothing at all when the
/// cell is empty. Grid writes are swallowed, as the real one does.
void answer_param_read(qc::test::FakeTransport* t, std::optional<float> stored) {
    t->set_responder([t, stored](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type != MessageType::RecallPreset) return;
        auto req = m.parse<pa::RecallPresetMessage>();
        if (!req || req->action() != pa::MessageAction::READ) return;
        pa::RecallPresetMessage reply;
        if (req->has_request_id()) reply.set_request_id(req->request_id());
        auto* preset = reply.mutable_preset();
        for (int row = 0; row < qc::view::kRows; ++row) {
            auto* chain = preset->add_chains();
            chain->set_row(static_cast<std::uint32_t>(row));
            for (int col = 0; col < qc::view::kColumns; ++col) {
                auto* model = chain->add_models();
                model->set_column(static_cast<std::uint32_t>(col));
                if (row != 0 || col != 0 || !stored) continue;
                model->set_hash(4005);
                model->add_params()->add_param_values()->set_float_value(*stored);
            }
        }
        t->push(MessageType::RecallPreset, reply);
    });
}

}  // namespace

TEST_CASE("set_param confirms the value arrived", "[device]") {
    Rig r;
    answer_param_read(r.t, 0.62f);
    CHECK(r.qc->set_param(0, 0, 0, 0.62f, {}, true, 100ms).has_value());
}

TEST_CASE("set_param no longer says ok to a write that vanished", "[device]") {
    // The mistake this catches: an address off by one lands on an empty cell,
    // the write disappears without a word, and "ok" sends the caller hunting
    // for a device fault that never was (2026-09-09).
    Rig r;
    answer_param_read(r.t, std::nullopt);  // the cell is empty
    auto res = r.qc->set_param(0, 0, 0, 0.62f, {}, true, 100ms);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().contains("row 1 column 1"));  // counted as the unit labels it
    CHECK(res.error().contains("the cell is empty"));
}

TEST_CASE("set_param reports a value the unit changed", "[device]") {
    Rig r;
    answer_param_read(r.t, 0.25f);  // unit clamped or ignored it
    auto res = r.qc->set_param(0, 0, 0, 0.62f, {}, true, 100ms);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().contains("reads 0.2500 after writing 0.6200"));
}

TEST_CASE("set_param with verify=false stays a plain send", "[device]") {
    Rig r;  // nothing answers at all
    CHECK(r.qc->set_param(0, 0, 0, 0.62f, {}, false).has_value());
}


TEST_CASE("subscribe_io_meter sends CREATE (no action byte) / DELETE with a request_id", "[device][iometer]") {
    Rig r;
    REQUIRE(r.qc->subscribe_io_meter(true).has_value());
    auto on = r.t->last_of(MessageType::IOMeter);
    REQUIRE(on.has_value());
    // CREATE ist der Nullwert: auf dem Draht steht nur die request_id (10 xx),
    // genau wie bei Cortex Control und `qcctl raw IOMeter 1006` (Beleg 182).
    REQUIRE(on->payload.size() >= 2);
    CHECK(on->payload[0] == 0x10);
    auto m = on->parse<pa::IOMeterMessage>();
    REQUIRE(m.has_value());
    CHECK(m->action() == pa::MessageAction::CREATE);
    CHECK(m->has_request_id());
    REQUIRE(r.qc->subscribe_io_meter(false).has_value());
    auto off = r.t->last_of(MessageType::IOMeter)->parse<pa::IOMeterMessage>();
    CHECK(off->action() == pa::MessageAction::DELETE);
    CHECK(off->request_id() != m->request_id());
}

TEST_CASE("IOMeter pushes become IoMeter events with merged levels", "[device][iometer]") {
    Rig r;
    std::atomic<int> events{0};
    std::atomic<float> input1{-1.0f};
    r.qc->set_event_sink([&](const qc::Event& e) {
        if (const auto* io = std::get_if<qc::event::IoMeter>(&e)) {
            input1 = io->levels.value(0);
            ++events;
        }
    });
    pa::IOMeterMessage m;
    m.set_action(pa::MessageAction::UPDATE);
    m.set_input_1(0.25f);
    m.set_hp_l(0.5f);
    r.t->push(MessageType::IOMeter, m);
    for (int i = 0; i < 100 && events == 0; ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(events == 1);
    CHECK(input1 == 0.25f);
    const auto l = r.qc->io_levels();
    CHECK(l.value(14) == 0.5f);  // hp_l
    CHECK(l.updates == 1);
}

TEST_CASE("set_event_sink waits for a running sink call", "[device][lifetime]") {
    Rig r;
    std::atomic<bool> drin{false};
    std::atomic<bool> fertig{false};
    r.qc->set_event_sink([&](const qc::Event&) {
        drin = true;
        std::this_thread::sleep_for(200ms);
        fertig = true;
    });
    pa::SceneMessage sc;
    sc.set_selected_scene(2);
    r.t->push(MessageType::Scene, sc);
    for (int i = 0; i < 200 && !drin; ++i) std::this_thread::sleep_for(5ms);
    REQUIRE(drin);
    r.qc->set_event_sink({});
    CHECK(fertig);
}
