// __REUSABLE__ mcp_stdio_server.hpp — minimaler MCP-Server (JSON-RPC 2.0 ueber stdio)
//
// Wer:      Martin.Lonkwitz
// Was:      Implementiert den Model-Context-Protocol-Lebenszyklus (initialize,
//           notifications/initialized, ping, tools/list, tools/call) ueber
//           zeilenweises JSON-RPC auf stdin/stdout. Tools werden als Name +
//           JSON-Schema + Handler registriert; Handler liefern Text oder JSON.
// Wann:     2026-09-04
// Warum:    Ein Quad-Cortex-MCP-Server soll ohne externe Framework-Abhaengigkeit
//           auskommen; der Kern ist ueberschaubar und wiederverwendbar.
// Projekt:  Ampersand (reusable)
// Abhaengig: nlohmann_json, C++23
// Nutzung:  mcp::Server srv{"qc-mcp", "0.1"}; srv.add_tool({...}); return srv.run();
// Sicherheit: stdio-Transport, kein Netz. Alles auf stderr ist Log; stdout
//           gehoert ausschliesslich JSON-RPC.
// Historie: git log
#pragma once

#include <nlohmann/json.hpp>

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp {

using json = nlohmann::json;

/// Result of a tool call: either text content or a structured JSON payload
/// (rendered as text for the model, and passed as structuredContent).
struct ToolResult {
    std::string text;
    std::optional<json> structured;
    bool is_error{false};

    static ToolResult ok(std::string t) { return {std::move(t), std::nullopt, false}; }
    static ToolResult ok_json(json j) {
        ToolResult r;
        r.text = j.dump(2);
        r.structured = std::move(j);
        return r;
    }
    static ToolResult error(std::string t) { return {std::move(t), std::nullopt, true}; }
};

struct Tool {
    std::string name;
    std::string description;
    json input_schema;  ///< JSON Schema object
    std::function<ToolResult(const json& args)> handler;
};

class Server {
public:
    Server(std::string name, std::string version, std::string instructions = {})
        : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}

    void add_tool(Tool t) { tools_[t.name] = std::move(t); }

    /// Serve until stdin closes. Returns the process exit code.
    int run(std::istream& in = std::cin, std::ostream& out = std::cout) {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            json req;
            try {
                req = json::parse(line);
            } catch (const std::exception& e) {
                emit(out, make_error(nullptr, -32700, std::string("parse error: ") + e.what()));
                continue;
            }
            if (req.is_array()) {
                for (const auto& r : req) handle(r, out);
            } else {
                handle(req, out);
            }
        }
        return 0;
    }

    /// Handle one request object; notifications produce no output.
    void handle(const json& req, std::ostream& out) {
        // A request that is not a JSON object (e.g. a bare number) would make
        // value()/operator[] below throw; JSON-RPC calls that an invalid
        // request. Guard it, and require a string "method" (both reproduced a
        // type_error that terminated the server, Review L3).
        if (!req.is_object()) {
            emit(out, make_error(nullptr, -32600, "invalid request: expected a JSON object"));
            return;
        }
        const json id = req.contains("id") ? req["id"] : json(nullptr);
        const bool is_notification = !req.contains("id");
        const auto method_it = req.find("method");
        if (method_it == req.end() || !method_it->is_string()) {
            if (!is_notification) emit(out, make_error(id, -32600, R"(invalid request: "method" must be a string)"));
            return;
        }
        const std::string method = method_it->get<std::string>();
        const json params = (req.contains("params") && req["params"].is_object()) ? req["params"] : json::object();

        if (method == "initialize") {
            json result{{"protocolVersion", negotiate(params.value("protocolVersion", std::string(kProtocolVersion)))},
                        {"capabilities", {{"tools", {{"listChanged", false}}}}},
                        {"serverInfo", {{"name", name_}, {"version", version_}}}};
            if (!instructions_.empty()) result["instructions"] = instructions_;
            emit(out, make_result(id, result));
            return;
        }
        if (method == "notifications/initialized" || method == "notifications/cancelled" ||
            method.starts_with("notifications/")) {
            return;
        }
        if (method == "ping") {
            emit(out, make_result(id, json::object()));
            return;
        }
        if (method == "tools/list") {
            json list = json::array();
            for (const auto& [n, t] : tools_) {
                list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
            }
            emit(out, make_result(id, json{{"tools", list}}));
            return;
        }
        if (method == "tools/call") {
            const std::string name = params.value("name", "");
            const json args = params.contains("arguments") ? params["arguments"] : json::object();
            auto it = tools_.find(name);
            if (it == tools_.end()) {
                emit(out, make_error(id, -32602, "unknown tool: " + name));
                return;
            }
            ToolResult r;
            try {
                r = it->second.handler(args);
            } catch (const std::exception& e) {
                r = ToolResult::error(std::string("tool threw: ") + e.what());
            }
            json result{{"content", json::array({json{{"type", "text"}, {"text", r.text}}})}, {"isError", r.is_error}};
            if (r.structured) result["structuredContent"] = *r.structured;
            emit(out, make_result(id, result));
            return;
        }
        if (method == "resources/list") { emit(out, make_result(id, json{{"resources", json::array()}})); return; }
        if (method == "prompts/list") { emit(out, make_result(id, json{{"prompts", json::array()}})); return; }
        if (!is_notification) emit(out, make_error(id, -32601, "method not found: " + method));
    }

    static constexpr std::string_view kProtocolVersion = "2025-06-18";

private:
    static std::string negotiate(const std::string& requested) {
        // We speak the 2024-11-05 and later line-delimited stdio dialects alike;
        // echo a version the client understands.
        if (requested == "2024-11-05" || requested == "2025-03-26" || requested == "2025-06-18") return requested;
        return std::string(kProtocolVersion);
    }
    static json make_result(const json& id, json result) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    }
    static json make_error(const json& id, int code, const std::string& message) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    }
    static void emit(std::ostream& out, const json& j) {
        out << j.dump() << '\n';
        out.flush();
    }

    std::string name_;
    std::string version_;
    std::string instructions_;
    std::map<std::string, Tool> tools_;
};

}  // namespace mcp
