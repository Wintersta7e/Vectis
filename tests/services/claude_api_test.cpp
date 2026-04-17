#include "services/ai_engine/claude_api.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "services/ai_engine/ai_engine.h"

namespace {

using vectis::services::AIRequest;
using vectis::services::build_claude_request;
using vectis::services::k_claude_default_model;
using vectis::services::parse_claude_response;
using vectis::services::parse_claude_sse_frame;

// ---- build_claude_request -------------------------------------------------

TEST(ClaudeApiTest, BuildRequest_IncludesRequiredFields)
{
    AIRequest req;
    req.user_prompt    = "What is 2+2?";
    req.system_prompt  = "You are a calculator.";
    req.max_tokens     = 256;
    req.temperature    = 0.5F;

    const auto body = build_claude_request(req, k_claude_default_model, false);

    EXPECT_EQ(body["model"], std::string(k_claude_default_model));
    EXPECT_EQ(body["max_tokens"], 256);
    EXPECT_FLOAT_EQ(body["temperature"].get<float>(), 0.5F);
    EXPECT_EQ(body["system"], "You are a calculator.");
    ASSERT_TRUE(body["messages"].is_array());
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "What is 2+2?");
    EXPECT_FALSE(body.contains("stream"));
}

TEST(ClaudeApiTest, BuildRequest_StreamFlagPropagates)
{
    AIRequest req;
    req.user_prompt = "hello";
    const auto body = build_claude_request(req, k_claude_default_model, true);
    EXPECT_TRUE(body.value("stream", false));
}

TEST(ClaudeApiTest, BuildRequest_OmitsSystemWhenEmpty)
{
    AIRequest req;
    req.user_prompt = "hi";
    const auto body = build_claude_request(req, "some-model", false);
    EXPECT_FALSE(body.contains("system"));
    EXPECT_EQ(body["model"], "some-model");
}

TEST(ClaudeApiTest, BuildRequest_MergesContextIntoUserTurn)
{
    AIRequest req;
    req.context     = "relevant code: foo()";
    req.user_prompt = "explain the above";
    const auto body = build_claude_request(req, k_claude_default_model, false);

    const std::string content = body["messages"][0]["content"];
    EXPECT_NE(content.find("relevant code: foo()"), std::string::npos);
    EXPECT_NE(content.find("explain the above"), std::string::npos);
    // Order: context first, then prompt.
    EXPECT_LT(content.find("relevant code"), content.find("explain"));
}

// ---- parse_claude_response ------------------------------------------------

TEST(ClaudeApiTest, ParseResponse_ExtractsText)
{
    const std::string json = R"json({
        "id": "msg_01",
        "type": "message",
        "role": "assistant",
        "content": [
            {"type": "text", "text": "The answer is 4."}
        ],
        "model": "claude-sonnet-4-6",
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 12, "output_tokens": 8}
    })json";

    auto r = parse_claude_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "The answer is 4.");
    EXPECT_EQ(r->prompt_tokens, 12);
    EXPECT_EQ(r->completion_tokens, 8);
    EXPECT_FALSE(r->was_truncated);
}

TEST(ClaudeApiTest, ParseResponse_TruncatedStopReason)
{
    const std::string json = R"json({
        "content": [{"type": "text", "text": "cut off..."}],
        "stop_reason": "max_tokens",
        "usage": {"input_tokens": 5, "output_tokens": 1024}
    })json";

    auto r = parse_claude_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->was_truncated);
    EXPECT_EQ(r->completion_tokens, 1024);
}

TEST(ClaudeApiTest, ParseResponse_ConcatenatesMultipleTextBlocks)
{
    const std::string json = R"json({
        "content": [
            {"type": "text", "text": "part one "},
            {"type": "text", "text": "part two"}
        ]
    })json";

    auto r = parse_claude_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "part one part two");
}

TEST(ClaudeApiTest, ParseResponse_MalformedJsonReturnsParseError)
{
    auto r = parse_claude_response("{not valid json");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

// ---- parse_claude_sse_frame -----------------------------------------------

TEST(ClaudeApiTest, ParseSseFrame_ContentDeltaEmitsText)
{
    const std::string frame =
        "event: content_block_delta\n"
        R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})";

    auto r = parse_claude_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, "Hello");
}

TEST(ClaudeApiTest, ParseSseFrame_IgnoresMessageStart)
{
    const std::string frame =
        "event: message_start\n"
        R"(data: {"type":"message_start","message":{"id":"msg_01","type":"message"}})";

    auto r = parse_claude_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(ClaudeApiTest, ParseSseFrame_EmptyDataReturnsNoDelta)
{
    // Keepalive / ping frames have no data line.
    auto r = parse_claude_sse_frame("event: ping\n");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(ClaudeApiTest, ParseSseFrame_MalformedDataReturnsParseError)
{
    const std::string frame =
        "event: content_block_delta\n"
        "data: {not-json";

    auto r = parse_claude_sse_frame(frame);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

} // namespace
