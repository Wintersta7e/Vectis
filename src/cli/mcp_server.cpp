#include "cli/mcp_server.h"

#include <exception>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vectis::cli {

namespace {

using json = nlohmann::json;

// JSON-RPC 2.0 error codes per the spec.
constexpr int k_parse_error = -32700;
constexpr int k_invalid_request = -32600;
constexpr int k_method_not_found = -32601;
constexpr int k_invalid_params = -32602;
constexpr int k_internal_error = -32603;

/// MCP protocol version this server speaks. Bump when the server adds
/// or changes a capability that's tied to a newer protocol revision.
constexpr const char* k_protocol_version = "2024-11-05";

[[nodiscard]] json make_error(const json& id, int code, std::string message)
{
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error",
         json{
             {"code", code},
             {"message", std::move(message)},
         }},
    };
}

[[nodiscard]] json make_result(const json& id, json value)
{
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(value)},
    };
}

void write_message(std::ostream& out, const json& msg)
{
    // Newline-delimited JSON: one object per line, MUST end with \n
    // and MUST NOT contain embedded newlines outside string values
    // (nlohmann::json::dump() guarantees this with default args).
    out << msg.dump() << "\n";
    out.flush();
}

[[nodiscard]] json handle_initialize(const json& id, const McpServerInfo& info)
{
    return make_result(id, json{
                               {"protocolVersion", k_protocol_version},
                               {"capabilities", json{{"tools", json::object()}}},
                               {"serverInfo", json{{"name", info.name}, {"version", info.version}}},
                           });
}

[[nodiscard]] json tool_to_json(const McpTool& tool)
{
    json schema;
    try {
        schema = json::parse(tool.input_schema_json);
    }
    catch (const json::parse_error&) {
        // Defensive — every tool's schema is built in-process from a
        // known-good string; if we somehow get malformed JSON, fail
        // gracefully with an empty object schema rather than crashing.
        schema = json::object();
    }
    return json{
        {"name", tool.name},
        {"description", tool.description},
        {"inputSchema", schema},
    };
}

[[nodiscard]] json handle_tools_list(const json& id, const std::vector<McpTool>& tools)
{
    json arr = json::array();
    for (const McpTool& t : tools) {
        arr.push_back(tool_to_json(t));
    }
    return make_result(id, json{{"tools", std::move(arr)}});
}

[[nodiscard]] json handle_tools_call(const json& id, const json& params,
                                     const std::vector<McpTool>& tools)
{
    if (!params.is_object()) {
        return make_error(id, k_invalid_params, "tools/call params must be an object");
    }
    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) {
        return make_error(id, k_invalid_params, "tools/call missing string `name`");
    }
    const std::string name = name_it->get<std::string>();

    const McpTool* match = nullptr;
    for (const McpTool& t : tools) {
        if (t.name == name) {
            match = &t;
            break;
        }
    }
    if (match == nullptr) {
        return make_error(id, k_method_not_found, "unknown tool: " + name);
    }

    const json arguments = params.value("arguments", json::object());
    std::string output;
    try {
        output = match->handler(arguments.dump());
    }
    catch (const McpHandlerError& e) {
        return make_error(id, e.code, e.message);
    }
    catch (const std::exception& e) {
        return make_error(id, k_internal_error, std::string{"handler threw: "} + e.what());
    }

    // MCP `tools/call` results wrap the raw output in a `content`
    // array. We always emit a single text item — the digest body or
    // explain narrative — which is enough for clients that just want
    // the raw answer back.
    json result = json{
        {"content", json::array({
                        json{{"type", "text"}, {"text", std::move(output)}},
                    })},
        {"isError", false},
    };
    return make_result(id, std::move(result));
}

void process_message(std::ostream& out, const json& msg, const std::vector<McpTool>& tools,
                     const McpServerInfo& info)
{
    if (!msg.is_object()) {
        write_message(out, make_error(json{}, k_invalid_request, "request must be a JSON object"));
        return;
    }

    // Notifications (no `id`) get no response per JSON-RPC 2.0.
    const bool is_notification = msg.find("id") == msg.end();
    const json id = msg.value("id", json{});

    const auto method_it = msg.find("method");
    if (method_it == msg.end() || !method_it->is_string()) {
        if (!is_notification) {
            write_message(out, make_error(id, k_invalid_request, "missing `method`"));
        }
        return;
    }
    const std::string method = method_it->get<std::string>();
    const json params = msg.value("params", json::object());

    json response;
    if (method == "initialize") {
        response = handle_initialize(id, info);
    }
    else if (method == "tools/list") {
        response = handle_tools_list(id, tools);
    }
    else if (method == "tools/call") {
        response = handle_tools_call(id, params, tools);
    }
    else if (method == "notifications/initialized" || method == "notifications/cancelled") {
        // Spec-defined notifications we accept silently.
        return;
    }
    else if (method == "ping") {
        // Some clients send `ping` as a liveness check. Empty result
        // object is the canonical reply.
        response = make_result(id, json::object());
    }
    else if (method == "shutdown") {
        // Per spec, respond with empty result then expect `exit`.
        response = make_result(id, json{});
    }
    else {
        if (is_notification) {
            return; // unknown notification, ignore
        }
        response = make_error(id, k_method_not_found, "method not found: " + method);
    }

    if (!is_notification || response.contains("error")) {
        write_message(out, response);
    }
}

} // namespace

int run_mcp_server(std::istream& in, std::ostream& out, const McpServerInfo& info,
                   const std::vector<McpTool>& tools)
{
    std::string line;
    while (std::getline(in, line)) {
        // Tolerate empty / whitespace-only lines — some clients pad
        // their stream with them between messages.
        bool only_ws = true;
        for (const char c : line) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                only_ws = false;
                break;
            }
        }
        if (only_ws) {
            continue;
        }

        json parsed;
        try {
            parsed = json::parse(line);
        }
        catch (const json::parse_error& e) {
            write_message(
                out, make_error(json{}, k_parse_error, std::string{"parse error: "} + e.what()));
            continue;
        }

        // A single line may carry a batch (JSON array of requests). We
        // dispatch each item independently and write each response.
        if (parsed.is_array()) {
            for (const auto& item : parsed) {
                process_message(out, item, tools, info);
            }
        }
        else {
            process_message(out, parsed, tools, info);
        }
    }
    return 0;
}

} // namespace vectis::cli
