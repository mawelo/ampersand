// test_io_meter.cpp — Kanaltabelle und Zusammenfuehren der IOMeter-Meldungen
#include "qc/io_meter.hpp"

#include <ProductionAutomation.pb.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

namespace pa = cortex_protobuf_v2;

TEST_CASE("channel table covers fields 3..41 except 19, in order, with unique keys", "[iometer]") {
    const auto ch = qc::iometer::channels();
    REQUIRE(ch.size() == qc::iometer::kChannelCount);
    std::set<std::string_view> keys;
    int expected_field = 3;
    for (std::size_t i = 0; i < ch.size(); ++i) {
        if (expected_field == 19) ++expected_field;  // hp_limiter_active ist bool
        CHECK(ch[i].index == i);
        CHECK(ch[i].field == expected_field);
        CHECK(keys.insert(ch[i].key).second);
        ++expected_field;
    }
    CHECK(expected_field == 42);
    // Jede Feldnummer der Tabelle muss im Proto ein float-Feld sein.
    const auto* d = pa::IOMeterMessage::descriptor();
    for (const auto& c : ch) {
        const auto* f = d->FindFieldByNumber(c.field);
        REQUIRE(f != nullptr);
        CHECK(f->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_FLOAT);
    }
}

TEST_CASE("merge maps every field to its channel and keeps absent ones", "[iometer]") {
    // Jedes Feld per Reflection mit einem eigenen Wert setzen und pruefen,
    // dass er am richtigen Platz landet - faengt Vertauschungen in der Tabelle.
    pa::IOMeterMessage m;
    const auto* d = m.GetDescriptor();
    const auto* r = m.GetReflection();
    for (const auto& c : qc::iometer::channels()) {
        r->SetFloat(&m, d->FindFieldByNumber(c.field), 0.001f * static_cast<float>(c.field));
    }
    m.set_hp_limiter_active(true);
    qc::iometer::Levels l;
    qc::iometer::merge(l, m);
    for (const auto& c : qc::iometer::channels()) {
        CHECK(l.value(c.index) == Catch::Approx(0.001f * static_cast<float>(c.field)));
    }
    CHECK(l.hp_limiter_active);
    CHECK(l.updates == 1);

    pa::IOMeterMessage nur_eingang;
    nur_eingang.set_input_1(0.5f);
    qc::iometer::merge(l, nur_eingang);
    CHECK(l.value(0) == Catch::Approx(0.5f));
    CHECK(l.value(1) == Catch::Approx(0.004f));  // input_2 (Feld 4) unberuehrt
    CHECK(l.hp_limiter_active);
    CHECK(l.updates == 2);

    // Eine explizite 0 ist ein Wert (letzte Meldung beim Verstummen, Beleg 182).
    pa::IOMeterMessage stille;
    stille.set_input_1(0.0f);
    qc::iometer::merge(l, stille);
    CHECK(l.value(0) == 0.0f);
}

TEST_CASE("to_db reads the value as linear amplitude and floors silence", "[iometer]") {
    CHECK(qc::iometer::to_db(1.0f) == Catch::Approx(0.0));
    CHECK(qc::iometer::to_db(0.5f) == Catch::Approx(-6.0206).margin(1e-3));
    CHECK(qc::iometer::to_db(0.0f) == qc::iometer::kFloorDb);
    CHECK(qc::iometer::to_db(-1.0f) == qc::iometer::kFloorDb);
}

TEST_CASE("Accumulator keeps last, max and mean per channel", "[iometer]") {
    qc::iometer::Accumulator acc;
    qc::iometer::Levels l;
    l.values[0] = 0.2f;
    l.hp_limiter_active = true;
    acc.add(l);
    l.values[0] = 0.6f;
    l.values[28] = 0.4f;
    l.hp_limiter_active = false;
    acc.add(l);
    l.values[0] = 0.1f;
    acc.add(l);
    CHECK(acc.messages() == 3);
    CHECK(acc.stats(0).last == Catch::Approx(0.1f));
    CHECK(acc.stats(0).max == Catch::Approx(0.6f));
    CHECK(acc.mean(0) == Catch::Approx(0.3).margin(1e-6));
    CHECK(acc.stats(28).max == Catch::Approx(0.4f));
    CHECK(acc.mean(28) == Catch::Approx(0.8 / 3.0).margin(1e-6));
    CHECK(acc.stats(1).max == 0.0f);
    CHECK(acc.hp_limiter_true() == 1);
    qc::iometer::Accumulator leer;
    CHECK(leer.mean(0) == 0.0);
}
