#include "services/ai_engine/openai_api.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "services/ai_engine/ai_engine.h"

namespace {

using vectis::services::AIRequest;
using vectis::services::build_openai_request;
using vectis::services::k_openai_default_model;
using vectis::services::parse_openai_response;
using vectis::services::parse_openai_sse_frame;

// ---- build_openai_request -------------------------------------------------

TEST(OpenAIApiTest, BuildRequest_IncludesRequiredFields)
{
    AIRequest req;
    req.user_prompt   = "hi";
    req.system_prompt = "you are a bot";
    req.max_tokens    = 128;
    req.temperature   = 0.9F;

    const auto body = build_openai_request(req, k_openai_default_model, false);

    EXPECT_EQ(body["model"], std::string(k_openai_default_model));
    EXPECT_EQ(body["max_tokens"], 128);
    EXPECT_FLOAT_EQ(body["temperature"].get<float>(), 0.9F);
    ASSERT_TRUE(body["messages"].is_array());
    ASSERT_EQ(body["messages"].size(), 2U);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][0]["content"], "you are a bot");
    EXPECT_EQ(body["messages"][1]["role"], "user");
    EXPECT_EQ(body["messages"][1]["content"], "hi");
    EXPECT_FALSE(body.contains("stream"));
}

TEST(OpenAIApiTest, BuildRequest_StreamFlagPropagates)
{
    AIRequest req;
    req.user_prompt = "hi";
    const auto body = build_openai_request(req, k_openai_default_model, true);
    EXPECT_TRUE(body.value("stream", false));
}

TEST(OpenAIApiTest, BuildRequest_OmitsSystemWhenEmpty)
{
    AIRequest req;
    req.user_prompt = "hi";
    const auto body = build_openai_request(req, "gpt-4o", false);
    ASSERT_TRUE(body["messages"].is_array());
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
}

TEST(OpenAIApiTest, BuildRequest_MergesContextIntoUserTurn)
{
    AIRequest req;
    req.context     = "def foo(): pass";
    req.user_prompt = "what does this do?";
    const auto body = build_openai_request(req, k_openai_default_model, false);
    const std::string content = body["messages"].back()["content"];
    EXPECT_NE(content.find("def foo()"), std::string::npos);
    EXPECT_NE(content.find("what does this do?"), std::string::npos);
}

// ---- parse_openai_response ------------------------------------------------

TEST(OpenAIApiTest, ParseResponse_ExtractsTextAndUsage)
{
    const std::string json = R"json({
        "id": "chatcmpl-1",
        "object": "chat.completion",
        "choices": [
            {
                "index": 0,
                "message": {"role": "assistant", "content": "pong"},
                "finish_reason": "stop"
            }
        ],
        "usage": {"prompt_tokens": 3, "completion_tokens": 1, "total_tokens": 4}
    })json";

    auto r = parse_openai_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "pong");
    EXPECT_EQ(r->prompt_tokens, 3);
    EXPECT_EQ(r->completion_tokens, 1);
    EXPECT_FALSE(r->was_truncated);
}

TEST(OpenAIApiTest, ParseResponse_FinishReasonLengthSetsTruncated)
{
    const std::string json = R"json({
        "choices": [
            {"index": 0,
             "message": {"role": "assistant", "content": "partial..."},
             "finish_reason": "length"}
        ],
        "usage": {"prompt_tokens": 1, "completion_tokens": 128}
    })json";

    auto r = parse_openai_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->was_truncated);
}

TEST(OpenAIApiTest, ParseResponse_MalformedJsonReturnsParseError)
{
    auto r = parse_openai_response("not json");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

// ---- parse_openai_sse_frame ----------------------------------------------

TEST(OpenAIApiTest, ParseSseFrame_ContentDeltaEmitsText)
{
    const std::string frame =
        R"(data: {"id":"cc-1","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})";

    auto r = parse_openai_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, "Hello");
}

TEST(OpenAIApiTest, ParseSseFrame_RoleOnlyDeltaEmitsNothing)
{
    const std::string frame =
        R"(data: {"choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]})";

    auto r = parse_openai_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(OpenAIApiTest, ParseSseFrame_DoneSentinelIsNotError)
{
    const std::string frame = "data: [DONE]";
    auto r = parse_openai_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(OpenAIApiTest, ParseSseFrame_MalformedDataReturnsParseError)
{
    const std::string frame = "data: {not-json";
    auto r = parse_openai_sse_frame(frame);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

TEST(OpenAIApiTest, ParseSseFrame_JoinsMultipleDataLinesPerSpec)
{
    // Two `data:` lines in one frame should be joined with '\n' before
    // JSON parsing (SSE spec). JSON tolerates internal whitespace.
    const std::string frame =
        R"(data: {"choices":[{"index":0,)"
        "\n"
        R"(data: "delta":{"content":"multi"}}]})";

    auto r = parse_openai_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, "multi");
}

} // namespace
