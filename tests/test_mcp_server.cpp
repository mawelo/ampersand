// test_mcp_server.cpp — mcp::Server: ein fehlgeformter Request darf den Server
// nie beenden, sondern muss einen sauberen JSON-RPC-Fehler liefern.
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-10-20
// Warum:    Review L3 (Forum Go-GitHub): Eingaben wie `5`, `{"method":5}` oder
//           ein nicht-stringiges params-Feld (protocolVersion/name) loesten in
//           handle() einen type_error ausserhalb jeder try aus -> terminate
//           (rc 134). Diese Faelle werden hier festgehalten.
// Projekt:  Ampersand
// Historie: git log
#include "../src/mcp/mcp_stdio_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>
#include <vector>

using mcp::json;

namespace {

// Fuettert die Zeilen durch einen Server mit einem Dummy-Tool und gibt die
// geparsten Antwortzeilen zurueck. Stuerzt der Server ab, bricht der
// Testprozess hier ab - genau das soll der Test verhindern.
std::vector<json> serve(std::initializer_list<std::string> lines) {
    mcp::Server srv{"test", "0.0"};
    srv.add_tool({"echo", "echo", json{{"type", "object"}},
                  [](const json&) { return mcp::ToolResult::ok("hi"); }});
    std::string input;
    for (const auto& l : lines) input += l + "\n";
    std::istringstream in{input};
    std::ostringstream out;
    REQUIRE(srv.run(in, out) == 0);
    std::vector<json> replies;
    std::istringstream rs{out.str()};
    std::string line;
    while (std::getline(rs, line)) {
        if (!line.empty()) replies.push_back(json::parse(line));
    }
    return replies;
}

}  // namespace

TEST_CASE("a non-object request is rejected, not crashed") {
    const auto r = serve({"5"});
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].contains("error"));
    REQUIRE(r[0]["error"]["code"] == -32600);
}

TEST_CASE("a non-string params field is reported, not crashed") {
    // protocolVersion and name used to reach value()/get<string>() unguarded.
    const auto r = serve({R"({"id":1,"method":"initialize","params":{"protocolVersion":5}})",
                          R"({"id":2,"method":"tools/call","params":{"name":5}})"});
    REQUIRE(r.size() == 2);
    REQUIRE(r[0]["id"] == 1);
    REQUIRE(r[0].contains("error"));
    REQUIRE(r[1]["id"] == 2);
    REQUIRE(r[1].contains("error"));
}

TEST_CASE("a malformed notification produces no output and does not crash") {
    // No "id" -> notification; an exception in handling is swallowed silently.
    const auto r = serve({R"({"method":"initialize","params":{"protocolVersion":5}})",
                          R"({"method":5})"});
    REQUIRE(r.empty());
}

TEST_CASE("well-formed requests still work") {
    const auto r = serve({R"({"id":3,"method":"ping"})",
                          R"({"id":4,"method":"tools/call","params":{"name":"echo","arguments":{}}})"});
    REQUIRE(r.size() == 2);
    REQUIRE(r[0]["id"] == 3);
    REQUIRE(r[0].contains("result"));
    REQUIRE(r[1]["id"] == 4);
    REQUIRE(r[1]["result"]["isError"] == false);
}
