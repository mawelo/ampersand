// test_session.cpp — Handshake, Korrelation, verschluesselte Eingaenge, Verlust
#include "qc/session.hpp"

#include "fake_transport.hpp"
#include "qc/compression.hpp"

#include <ProductionAutomation.pb.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace pa = cortex_protobuf_v2;
using namespace std::chrono_literals;
using qc::MessageType;

namespace {

qc::Session::Options fast_options() {
    qc::Session::Options o;
    o.read_timeout = 20ms;
    o.keepalive_interval = 50ms;
    o.handshake_timeout = 500ms;
    o.handshake_patience = 1500ms;
    o.settle = 10ms;
    return o;
}

/// A device that answers the handshake like the real one.
struct FakeDevice {
    qc::test::FakeTransport* t;
    std::optional<std::string> comms_version;  // if set: sent verbatim in comms_version
    std::atomic<int> version_reads{0};

    void install() {
        t->set_responder([this](const qc::test::FakeTransport::HostMessage& m) { respond(m); });
    }

    void respond(const qc::test::FakeTransport::HostMessage& m) {
        switch (m.type) {
            case MessageType::ResetCommsBuffers: {
                auto req = m.parse<pa::ResetCommsBuffersMessage>();
                pa::ResetCommsBuffersMessage echo;
                echo.set_session_id(req->session_id());
                t->push(MessageType::ResetCommsBuffers, echo);
                break;
            }
            case MessageType::Version: {
                auto req = m.parse<pa::VersionMessage>();
                if (req->action() == pa::MessageAction::READ) {
                    ++version_reads;
                    pa::VersionMessage v;
                    v.set_action(pa::MessageAction::UPDATE);
                    v.set_app_fw_version("d14e");
                    v.set_device_serial_number("QCXXXXXXXXX");
                    v.set_linux_kernel_version("Linux buildroot 4.0.0-ADI");
                    if (comms_version) v.set_comms_version(*comms_version);
                    t->push(MessageType::Version, v);
                    // The device also asks the host for ITS version.
                    pa::VersionMessage ask;
                    ask.set_action(pa::MessageAction::READ);
                    t->push(MessageType::Version, ask);
                }
                break;
            }
            case MessageType::ModelRepo: {
                pa::ModelRepoMessage repo;
                repo.set_model_repo_payload("<Models/>");
                t->push(MessageType::ModelRepo, repo);
                break;
            }
            default:
                break;
        }
    }
};

}  // namespace

TEST_CASE("handshake sends the Cortex Control sequence and records the version", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    FakeDevice dev{.t = t, .comms_version = std::string("\x01\x02\xfe\xff", 4)};
    dev.install();

    qc::Session s(std::move(transport), fast_options());
    auto ok = s.connect();
    REQUIRE(ok.has_value());
    CHECK(s.connected());
    const auto v = s.device_version();
    CHECK(v.app_fw_version == "d14e");
    CHECK(v.serial_number == "QCXXXXXXXXX");
    CHECK(v.comms_version_hex == "01 02 fe ff");  // kept verbatim for offline analysis

    // Order of the host's messages: ResetComms, Version READ, Version UPDATE(s), ModelRepo READ, Connection, burst.
    auto msgs = t->host_messages();
    REQUIRE(msgs.size() >= 5);
    CHECK(msgs[0].type == MessageType::ResetCommsBuffers);
    CHECK(msgs[1].type == MessageType::Version);
    CHECK(msgs[1].parse<pa::VersionMessage>()->action() == pa::MessageAction::READ);
    // Every Version UPDATE announces our version.
    int updates = 0;
    for (const auto& m : msgs) {
        if (m.type == MessageType::Version) {
            auto vm = m.parse<pa::VersionMessage>();
            if (vm->action() == pa::MessageAction::UPDATE) {
                ++updates;
                // Der Rueckfallwert liegt bei 4.1.0, nicht darunter: ZenUI 4.1.1
                // vergleicht die angekuendigte Fassung gegen die Konstante
                // "4.1.0" und protokolliert alles Kleinere als
                // "Cortex-Control version is incompatible" (Beleg 144).
                CHECK(vm->cortex_control_version() == "4.1.0");
            }
        }
    }
    CHECK(updates >= 1);
    CHECK(t->count_of(MessageType::ModelRepo) == 1);
    auto conn = t->last_of(MessageType::Connection);
    REQUIRE(conn.has_value());
    CHECK(conn->parse<pa::ConnectionMessage>()->connected());
    CHECK(t->count_of(MessageType::RecallPreset) == 1);  // subscription READ
    CHECK(s.model_repo_payload().has_value());

    // Keepalive flows.
    std::this_thread::sleep_for(200ms);
    CHECK(t->count_of(MessageType::KeepAlive) >= 2);
    auto ka = t->last_of(MessageType::KeepAlive)->parse<pa::KeepAliveMessage>();
    CHECK(ka->action() == pa::MessageAction::UPDATE);
    CHECK(ka->is_online());

    // Disconnect says goodbye.
    s.disconnect();
    auto bye = t->last_of(MessageType::Connection);
    CHECK_FALSE(bye->parse<pa::ConnectionMessage>()->connected());
    CHECK_FALSE(s.connected());
}

TEST_CASE("encrypted pushes are not decrypted but flagged decrypt_failed", "[session]") {
    // Verschluesselte Nachrichten werden nicht unterstuetzt: die Nutzlast bleibt
    // unveraendert, die Nachricht traegt decrypt_failed und wird gezaehlt.
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    s.start();
    const std::vector<std::uint8_t> opaque{0x8a, 0x13, 0x5c, 0xe0, 0x07, 0x42};
    auto got = s.await(MessageType::Scene, [&] { t->push_raw(MessageType::Scene, opaque, true, false); }, 1000ms);
    REQUIRE(got.has_value());
    CHECK(got->encrypted);
    CHECK(got->decrypt_failed);
    CHECK(got->payload == opaque);
    CHECK(s.stats().decrypt_failures == 1);
}

TEST_CASE("request() correlates by type and request_id; READ replies without id are accepted", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    t->set_responder([t](const qc::test::FakeTransport::HostMessage& m) {
        if (m.type == MessageType::PresetDirty) {
            auto req = m.parse<pa::PresetDirtyMessage>();
            // First a stale message with the WRONG id, then the right one.
            pa::PresetDirtyMessage stale;
            stale.set_action(pa::MessageAction::UPDATE);
            stale.set_request_id(req->request_id() + 1000);
            stale.set_is_dirty(false);
            t->push(MessageType::PresetDirty, stale);
            pa::PresetDirtyMessage reply;
            reply.set_action(pa::MessageAction::UPDATE);
            reply.set_request_id(req->request_id());
            reply.set_is_dirty(true);
            t->push(MessageType::PresetDirty, reply);
        }
    });
    qc::Session s(std::move(transport), fast_options());
    s.start();
    pa::PresetDirtyMessage req;
    req.set_action(pa::MessageAction::READ);
    auto reply = s.request(MessageType::PresetDirty, req, 1000ms);
    REQUIRE(reply.has_value());
    CHECK(reply->is_dirty());
    CHECK(reply->request_id() >= 1);

    // Timeout when nothing matches.
    pa::SceneMessage sc;
    sc.set_action(pa::MessageAction::READ);
    auto none = s.request(MessageType::Scene, sc, 100ms);
    CHECK_FALSE(none.has_value());
    CHECK(s.stats().undelivered == 1);
}

TEST_CASE("gzip payloads are inflated; collect gathers a burst", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    s.start();

    pa::FileMessage f1;
    f1.mutable_folder()->set_key("/media/p4/Presets/My Presets");
    f1.mutable_folder()->add_files()->set_name("A");
    pa::FileMessage f2;
    f2.mutable_folder()->set_key("/opt/neuraldsp/Factory Library");
    f2.mutable_folder()->add_files()->set_name("B");
    const std::string raw2 = f2.SerializeAsString();
    auto gz = qc::compression::gzip(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(raw2.data()), raw2.size()));
    REQUIRE(gz.has_value());

    auto got = s.collect(
        MessageType::File,
        [&] {
            t->push(MessageType::File, f1);
            t->push_raw(MessageType::File, *gz, false, true);
        },
        300ms);
    REQUIRE(got.size() == 2);
    CHECK(got[1].compressed);
    CHECK(got[1].parse<pa::FileMessage>()->folder().key() == "/opt/neuraldsp/Factory Library");
}

TEST_CASE("device loss wakes waiters and is reported", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    std::string reason;
    s.set_lost_handler([&](const std::string& r) { reason = r; });
    s.start();
    auto res = s.await(MessageType::Scene, [&] { t->unplug(); }, 2000ms);
    CHECK_FALSE(res.has_value());
    CHECK(s.lost());
    // The lost handler runs on the RX thread right after the waiters are woken.
    for (int i = 0; i < 100 && reason.empty(); ++i) std::this_thread::sleep_for(5ms);
    CHECK(reason.find("unplugged") != std::string::npos);
    pa::SceneMessage sc;
    CHECK_FALSE(s.send(MessageType::Scene, sc).has_value());
}

TEST_CASE("the RX thread survives garbage and undecodable pushes", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    int seen = 0;
    s.set_listener([&](const qc::Inbound&) { ++seen; });
    s.start();
    std::vector<std::uint8_t> junk_report{0x01, 0x05, 0xc0, 0xff, 0xff, 0xff, 0xff, 0xff};  // body shorter than trailer
    junk_report.resize(129, 0);
    t->push_report(junk_report);
    std::vector<std::uint8_t> non_proto{0xff, 0xfe, 0xfd};
    t->push_raw(MessageType::License, non_proto);
    pa::SceneMessage sc;
    sc.set_selected_scene(1);
    auto got = s.await(MessageType::Scene, [&] { t->push(MessageType::Scene, sc); }, 1000ms);
    REQUIRE(got.has_value());
    CHECK(seen >= 2);
    CHECK(s.stats().decode_errors == 1);
}

TEST_CASE("the device's own Version READ is answered", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    s.start();
    pa::VersionMessage ask;
    ask.set_action(pa::MessageAction::READ);
    t->push(MessageType::Version, ask);
    for (int i = 0; i < 50 && t->count_of(MessageType::Version) == 0; ++i) std::this_thread::sleep_for(10ms);
    auto reply = t->last_of(MessageType::Version);
    REQUIRE(reply.has_value());
    auto vm = reply->parse<pa::VersionMessage>();
    CHECK(vm->action() == pa::MessageAction::UPDATE);
    CHECK(vm->cortex_control_version() == "4.1.0");  // siehe oben, Beleg 144
}

TEST_CASE("handshake times out on a silent device and reports it", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto o = fast_options();
    o.handshake_timeout = 100ms;
    o.handshake_patience = 250ms;
    qc::Session s(std::move(transport), o);
    auto r = s.connect();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("attempt") != std::string::npos);
}

TEST_CASE("await sits out a busy device: silence is the budget, not total time", "[session]") {
    // The unit queues a request behind its seed push and answers 60 ms after
    // its own last message — but the whole thing takes 12 to 16 s
    // (docs/evidence/78-antwortzeiten). A budget on total time gives up on a
    // device that is merely busy; one on silence does not.
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    FakeDevice dev{.t = t, .comms_version = std::nullopt};
    dev.install();
    qc::Session s{std::move(transport), fast_options()};
    REQUIRE(s.connect().has_value());

    // Chatter for 600 ms, then the reply we are after.
    std::jthread device{[t] {
        for (int i = 0; i < 12; ++i) {
            pa::GlobalTempoMessage chatter;
            chatter.set_action(pa::MessageAction::UPDATE);
            t->push(MessageType::GlobalTempo, chatter);
            std::this_thread::sleep_for(50ms);
        }
        pa::SetlistPositionMessage pos;
        pos.set_action(pa::MessageAction::UPDATE);
        pos.set_folder_key("/media/p4/Presets/My Presets");
        pos.set_position(2);
        t->push(MessageType::SetlistPosition, pos);
    }};

    const auto start = std::chrono::steady_clock::now();
    auto in = s.await(MessageType::SetlistPosition, [] {}, 200ms, [](const qc::Inbound& i) {
        auto m = i.parse<pa::SetlistPositionMessage>();
        return m && m->has_folder_key();
    });
    const auto waited = std::chrono::steady_clock::now() - start;

    REQUIRE(in.has_value());  // used to give up after 200 ms
    CHECK(in->parse<pa::SetlistPositionMessage>()->position() == 2);
    CHECK(waited > 500ms);  // it really did sit through the chatter
}

TEST_CASE("await still gives up on a device that says nothing", "[session]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    FakeDevice dev{.t = t, .comms_version = std::nullopt};
    dev.install();
    qc::Session s{std::move(transport), fast_options()};
    REQUIRE(s.connect().has_value());

    const auto start = std::chrono::steady_clock::now();
    auto in = s.await(MessageType::Scene, [] {}, 150ms);
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK_FALSE(in.has_value());
    CHECK(in.error().contains("silent for 150 ms"));
    CHECK(waited < 2s);  // no hanging on the hard cap
}

TEST_CASE("set_listener waits for a running listener call and stops further calls", "[session][lifetime]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    std::atomic<bool> drin{false};
    std::atomic<bool> fertig{false};
    std::atomic<int> aufrufe{0};
    s.set_listener([&](const qc::Inbound&) {
        ++aufrufe;
        drin = true;
        std::this_thread::sleep_for(200ms);  // ein laufender Aufruf
        fertig = true;
    });
    s.start();
    pa::SceneMessage sc;
    sc.set_selected_scene(1);
    t->push(MessageType::Scene, sc);
    for (int i = 0; i < 200 && !drin; ++i) std::this_thread::sleep_for(5ms);
    REQUIRE(drin);
    s.set_listener({});
    // Die Garantie: nach der Rueckkehr ist der alte Aufruf zu Ende ...
    CHECK(fertig);
    // ... und es kommt keiner mehr.
    const int vorher = aufrufe;
    t->push(MessageType::Scene, sc);
    std::this_thread::sleep_for(100ms);
    CHECK(aufrufe == vorher);
    s.stop();
}

TEST_CASE("a listener may clear itself without deadlocking", "[session][lifetime]") {
    auto transport = std::make_unique<qc::test::FakeTransport>();
    auto* t = transport.get();
    qc::Session s(std::move(transport), fast_options());
    std::atomic<int> aufrufe{0};
    s.set_listener([&](const qc::Inbound&) {
        ++aufrufe;
        s.set_listener({});  // aus dem RX-Thread heraus: rekursive Sperre
    });
    s.start();
    pa::SceneMessage sc;
    t->push(MessageType::Scene, sc);
    t->push(MessageType::Scene, sc);
    std::this_thread::sleep_for(200ms);
    CHECK(aufrufe == 1);
    s.stop();
}
