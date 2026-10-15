// test_framing.cpp — Report-Codec gegen echte Wire-Captures
#include "qc/framing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace qc::framing;

namespace {

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

}  // namespace

TEST_CASE("Version READ encodes to the captured frame", "[framing]") {
    // REAL captured frame (Cortex Control 4.0.1, windows-session-01.pcap frame 5584):
    // [id 02][len 0a][flags c0][pb 08 03][trailer 0a 00 + 6 zero bytes] + padding
    const std::vector<std::uint8_t> payload{0x08, 0x03};
    auto reports = encode(10, payload);
    REQUIRE(reports.size() == 1);
    const auto& r = reports[0];
    CHECK(r[0] == 0x02);
    CHECK(r[1] == 0x0a);
    CHECK(r[2] == 0xc0);
    CHECK(r[3] == 0x08);
    CHECK(r[4] == 0x03);
    CHECK(r[5] == 0x0a);
    CHECK(r[6] == 0x00);
    for (std::size_t i = 7; i < kReportSize; ++i) CHECK(r[i] == 0);
}

TEST_CASE("trailer flags", "[framing]") {
    auto t = make_trailer(0x0123, true, true);
    CHECK(t[0] == 0x23);
    CHECK(t[1] == 0x01);
    CHECK(t[4] == 1);
    CHECK(t[5] == 1);
    CHECK(t[6] == 0);
    CHECK(t[7] == 0);
}

TEST_CASE("multi-report encode/decode round trip", "[framing]") {
    std::vector<std::uint8_t> payload(300);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 7);
    auto reports = encode(15, payload);
    REQUIRE(reports.size() == 3);  // 308 bytes -> 126 + 126 + 56
    CHECK(reports[0][1] == 126);
    CHECK(reports[0][2] == 0x40);
    CHECK(reports[1][1] == 126);
    CHECK(reports[1][2] == 0x00);
    CHECK(reports[2][1] == 56);
    CHECK(reports[2][2] == 0x80);

    Reassembler ra;
    std::optional<std::vector<std::uint8_t>> body;
    for (const auto& r : reports) {
        auto res = ra.push(r);
        REQUIRE(res.has_value());
        if (*res) body = **res;
    }
    REQUIRE(body.has_value());
    auto env = decode_body(*body);
    REQUIRE(env.has_value());
    CHECK(env->type == 15);
    CHECK_FALSE(env->encrypted);
    CHECK_FALSE(env->compressed);
    CHECK(env->payload == payload);
    CHECK(ra.stats().messages == 1);
}

TEST_CASE("device padding beyond len is ignored", "[framing]") {
    Report r{};
    r[0] = kInReportId;
    r[1] = 0x0a;
    r[2] = 0xc0;
    r[3] = 0x08;
    r[4] = 0x03;
    r[5] = 0x0a;  // type 10
    r[6] = 0x00;
    r[11] = 0x60;  // device-filled trailer bytes
    r[12] = 0xb3;
    for (std::size_t i = 13; i < kReportSize; ++i) r[i] = 0xEE;  // stale buffer
    Reassembler ra;
    auto res = ra.push(r);
    REQUIRE(res.has_value());
    REQUIRE(res->has_value());
    auto env = decode_body(**res);
    REQUIRE(env.has_value());
    CHECK(env->type == 10);
    CHECK(env->payload == std::vector<std::uint8_t>{0x08, 0x03});
}

TEST_CASE("single-report message interleaved into a multi-report transfer", "[framing]") {
    std::vector<std::uint8_t> big(200, 0xAB);
    auto big_reports = encode(51, big);
    REQUIRE(big_reports.size() == 2);
    auto small = encode(33, std::vector<std::uint8_t>{0x08, 0x01});
    Reassembler ra;
    auto r1 = ra.push(big_reports[0]);
    REQUIRE((r1.has_value() && !r1->has_value()));
    auto r2 = ra.push(small[0]);  // FIRST|LAST while assembling
    REQUIRE(r2.has_value());
    REQUIRE(r2->has_value());
    CHECK(decode_body(**r2)->type == 33);
    CHECK(ra.assembling());
    auto r3 = ra.push(big_reports[1]);
    REQUIRE(r3.has_value());
    REQUIRE(r3->has_value());
    auto env = decode_body(**r3);
    CHECK(env->type == 51);
    CHECK(env->payload == big);
    CHECK(ra.stats().dropped_partials == 0);
}

TEST_CASE("a FIRST report drops a stale partial; stray continuations are ignored", "[framing]") {
    std::vector<std::uint8_t> big(200, 1);
    auto reports = encode(4, big);
    Reassembler ra;
    (void)ra.push(reports[0]);
    (void)ra.push(reports[0]);  // FIRST again
    CHECK(ra.stats().dropped_partials == 1);
    auto done = ra.push(reports[1]);
    REQUIRE(done->has_value());
    auto stray = ra.push(reports[1]);  // continuation with nothing pending
    REQUIRE(stray.has_value());
    CHECK_FALSE(stray->has_value());
    CHECK(ra.stats().stray_continuations == 1);
}

TEST_CASE("bodies shorter than the trailer are rejected", "[framing]") {
    std::vector<std::uint8_t> tiny{1, 2, 3};
    CHECK_FALSE(decode_body(tiny).has_value());
    std::vector<std::uint8_t> short_report{0x01, 0x05};
    CHECK_FALSE(parse_header(short_report).has_value());
}

TEST_CASE("ResetCommsBuffers captured frame decodes", "[framing]") {
    // From docs: 02 2c c0 | 08 00 | 12 20 <32 hex chars> | 34 00 00 00 00 00 00 00
    std::string frame = "022cc00800122030313233343536373839616263646566303132333435363738396162636465663400000000000000";
    auto bytes = from_hex(frame);
    bytes.resize(kReportSize, 0);
    Reassembler ra;
    auto res = ra.push(bytes);
    REQUIRE(res.has_value());
    REQUIRE(res->has_value());
    auto env = decode_body(**res);
    REQUIRE(env.has_value());
    CHECK(env->type == 52);
    CHECK(env->payload.size() == 36);
}

TEST_CASE("hex helper", "[framing]") {
    std::vector<std::uint8_t> b{0x08, 0x03, 0xff};
    CHECK(hex(b) == "08 03 ff");
    CHECK(hex(b, 2) == "08 03 ...(+1)");
}
