#pragma once

#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "core/version.h"

namespace vectis::cli {

/// One MCP tool exposed to clients. The handler is invoked when an MCP
/// client issues `tools/call` with `name` matching this tool's `name`.
/// `arguments_json` carries the raw `arguments` object from the call;
/// the handler is responsible for validating it.
///
/// Returning `std::nullopt` from the handler is *not* legal — the
/// protocol requires either a result string or an exception. Throw
/// `McpHandlerError` for a structured error response; any other
/// exception becomes a JSON-RPC `internal error`.
struct McpTool
{
    std::string name;
    std::string description;
    std::string input_schema_json; ///< JSON-serialised JSON Schema.
    std::function<std::string(const std::string& arguments_json)> handler;
};

/// Thrown by tool handlers to surface a JSON-RPC error to the client
/// with a specific code + message rather than the generic `internal
/// error` we map other exceptions to.
struct McpHandlerError
{
    int code = -32602; ///< default to "Invalid params"
    std::string message;
};

struct McpServerInfo
{
    std::string name = "vectis";
    std::string version = vectis::core::k_vectis_version;
};

/// Run the MCP server loop on `in` / `out`. Reads newline-delimited
/// JSON-RPC 2.0 messages from `in`, handles `initialize`, `tools/list`,
/// and `tools/call`, dispatching the latter to one of `tools` by name.
/// Returns when `in` reports EOF. Always returns 0 — the loop is the
/// long-running operation; failure modes are surfaced via JSON-RPC
/// error responses, not return codes.
[[nodiscard]] int run_mcp_server(std::istream& in, std::ostream& out, const McpServerInfo& info,
                                 const std::vector<McpTool>& tools);

} // namespace vectis::cli
