#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "cli/mcp_server.h"

namespace {

using vectis::cli::McpHandlerError;
using vectis::cli::McpServerInfo;
using vectis::cli::McpTool;
using vectis::cli::run_mcp_server;
using json = nlohmann::json;

McpServerInfo info()
{
    return McpServerInfo{.name = "vectis-test", .version = "9.9.9"};
}

McpTool echo_tool()
{
    return McpTool{
        .name = "echo",
        .description = "Echo back the input.",
        .input_schema_json = R"({"type":"object","properties":{"text":{"type":"string"}},
                                 "required":["text"]})",
        .handler = [](const std::string& arguments_json) -> std::string {
            const auto args = json::parse(arguments_json);
            if (!args.contains("text") || !args["text"].is_string()) {
                throw McpHandlerError{-32602, "missing `text`"};
            }
            return args["text"].get<std::string>();
        },
    };
}

/// Run a fixed input through the server and split the multi-line output
/// into one JSON object per line.
std::vector<json> run_and_collect(const std::string& input, const std::vector<McpTool>& tools)
{
    std::istringstream is(input);
    std::ostringstream os;
    EXPECT_EQ(run_mcp_server(is, os, info(), tools), 0);
    std::vector<json> result;
    std::string line;
    std::istringstream replay(os.str());
    while (std::getline(replay, line)) {
        if (line.empty()) {
            continue;
        }
        result.push_back(json::parse(line));
    }
    return result;
}

TEST(McpServerTest, InitializeReturnsServerInfoAndProtocolVersion)
{
    const auto out = run_and_collect(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"
                                     "\n",
                                     {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0]["jsonrpc"], "2.0");
    EXPECT_EQ(out[0]["id"], 1);
    ASSERT_TRUE(out[0].contains("result"));
    EXPECT_EQ(out[0]["result"]["serverInfo"]["name"], "vectis-test");
    EXPECT_EQ(out[0]["result"]["serverInfo"]["version"], "9.9.9");
    EXPECT_TRUE(out[0]["result"].contains("protocolVersion"));
    EXPECT_TRUE(out[0]["result"]["capabilities"].contains("tools"));
}

TEST(McpServerTest, ToolsListReturnsAllRegisteredTools)
{
    const auto out = run_and_collect(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
                                     "\n",
                                     {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("result"));
    const auto& tools = out[0]["result"]["tools"];
    ASSERT_TRUE(tools.is_array());
    ASSERT_EQ(tools.size(), 1U);
    EXPECT_EQ(tools[0]["name"], "echo");
    EXPECT_EQ(tools[0]["inputSchema"]["type"], "object");
}

TEST(McpServerTest, ToolsCallReturnsHandlerOutputAsTextContent)
{
    const auto out = run_and_collect(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})"
        "\n",
        {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("result"));
    const auto& content = out[0]["result"]["content"];
    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 1U);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[0]["text"], "hi");
    EXPECT_EQ(out[0]["result"]["isError"], false);
}

TEST(McpServerTest, ToolsCallUnknownToolReturnsMethodNotFound)
{
    const auto out = run_and_collect(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ghosts","arguments":{}}})"
        "\n",
        {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("error"));
    EXPECT_EQ(out[0]["error"]["code"], -32601);
}

TEST(McpServerTest, HandlerErrorIsReturnedWithItsCodeAndMessage)
{
    const auto out = run_and_collect(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{}}})"
        "\n",
        {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("error"));
    EXPECT_EQ(out[0]["error"]["code"], -32602);
}

TEST(McpServerTest, ParseErrorReturnsParseErrorResponse)
{
    const auto out = run_and_collect("{not valid json\n", {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("error"));
    EXPECT_EQ(out[0]["error"]["code"], -32700);
}

TEST(McpServerTest, NotificationsProduceNoResponse)
{
    // No `id` field → notification per JSON-RPC 2.0 → no reply expected.
    const auto out = run_and_collect(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
                                     "\n",
                                     {echo_tool()});
    EXPECT_TRUE(out.empty());
}

TEST(McpServerTest, UnknownMethodReturnsMethodNotFound)
{
    const auto out = run_and_collect(R"({"jsonrpc":"2.0","id":6,"method":"telepathy/transmit"})"
                                     "\n",
                                     {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    ASSERT_TRUE(out[0].contains("error"));
    EXPECT_EQ(out[0]["error"]["code"], -32601);
}

TEST(McpServerTest, BlankLinesBetweenMessagesAreTolerated)
{
    const auto out = run_and_collect("\n   \n"
                                     R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})"
                                     "\n\n",
                                     {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0]["id"], 7);
}

TEST(McpServerTest, BatchRequestsAreDispatchedIndependently)
{
    // Single line carrying a JSON array of two requests.
    const auto out = run_and_collect(
        R"([{"jsonrpc":"2.0","id":8,"method":"tools/list"},{"jsonrpc":"2.0","id":9,"method":"ping"}])"
        "\n",
        {echo_tool()});
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out[0]["id"], 8);
    EXPECT_EQ(out[1]["id"], 9);
    // ping returns an empty result object.
    EXPECT_TRUE(out[1]["result"].is_object());
}

TEST(McpServerTest, PingReturnsEmptyResult)
{
    const auto out = run_and_collect(R"({"jsonrpc":"2.0","id":10,"method":"ping"})"
                                     "\n",
                                     {echo_tool()});
    ASSERT_EQ(out.size(), 1U);
    EXPECT_TRUE(out[0]["result"].is_object());
}

} // namespace
