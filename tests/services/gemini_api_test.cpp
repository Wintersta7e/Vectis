#include "services/ai_engine/gemini_api.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "services/ai_engine/ai_engine.h"

namespace {

using vectis::services::AIRequest;
using vectis::services::build_gemini_request;
using vectis::services::parse_gemini_response;
using vectis::services::parse_gemini_sse_frame;

// ---- build_gemini_request -------------------------------------------------

TEST(GeminiApiTest, BuildRequest_IncludesRequiredFields)
{
    AIRequest req;
    req.user_prompt    = "hello";
    req.system_prompt  = "be nice";
    req.max_tokens     = 512;
    req.temperature    = 0.2F;

    const auto body = build_gemini_request(req);

    ASSERT_TRUE(body["contents"].is_array());
    ASSERT_EQ(body["contents"].size(), 1U);
    EXPECT_EQ(body["contents"][0]["role"], "user");
    EXPECT_EQ(body["contents"][0]["parts"][0]["text"], "hello");

    ASSERT_TRUE(body.contains("systemInstruction"));
    EXPECT_EQ(body["systemInstruction"]["parts"][0]["text"], "be nice");

    EXPECT_EQ(body["generationConfig"]["maxOutputTokens"], 512);
    EXPECT_FLOAT_EQ(body["generationConfig"]["temperature"].get<float>(), 0.2F);
}

TEST(GeminiApiTest, BuildRequest_OmitsSystemWhenEmpty)
{
    AIRequest req;
    req.user_prompt = "hi";
    const auto body = build_gemini_request(req);
    EXPECT_FALSE(body.contains("systemInstruction"));
}

TEST(GeminiApiTest, BuildRequest_MergesContextIntoUserTurn)
{
    AIRequest req;
    req.context     = "def bar(): ...";
    req.user_prompt = "explain";
    const auto body = build_gemini_request(req);
    const std::string text = body["contents"][0]["parts"][0]["text"];
    EXPECT_NE(text.find("def bar()"), std::string::npos);
    EXPECT_NE(text.find("explain"), std::string::npos);
}

// ---- parse_gemini_response ------------------------------------------------

TEST(GeminiApiTest, ParseResponse_ExtractsTextAndUsage)
{
    const std::string json = R"json({
        "candidates": [
            {
                "content": {
                    "parts": [{"text": "hi there"}],
                    "role": "model"
                },
                "finishReason": "STOP",
                "index": 0
            }
        ],
        "usageMetadata": {
            "promptTokenCount": 4,
            "candidatesTokenCount": 3,
            "totalTokenCount": 7
        }
    })json";

    auto r = parse_gemini_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "hi there");
    EXPECT_EQ(r->prompt_tokens, 4);
    EXPECT_EQ(r->completion_tokens, 3);
    EXPECT_FALSE(r->was_truncated);
}

TEST(GeminiApiTest, ParseResponse_TruncatedFinishReason)
{
    const std::string json = R"json({
        "candidates": [
            {
                "content": {"parts": [{"text": "cut..."}], "role": "model"},
                "finishReason": "MAX_TOKENS"
            }
        ],
        "usageMetadata": {"promptTokenCount": 2, "candidatesTokenCount": 100}
    })json";

    auto r = parse_gemini_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->was_truncated);
}

TEST(GeminiApiTest, ParseResponse_ConcatenatesMultipleParts)
{
    const std::string json = R"json({
        "candidates": [
            {
                "content": {
                    "parts": [
                        {"text": "first "},
                        {"text": "second"}
                    ]
                }
            }
        ]
    })json";

    auto r = parse_gemini_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "first second");
}

TEST(GeminiApiTest, ParseResponse_MalformedJsonReturnsParseError)
{
    auto r = parse_gemini_response("{{{");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

// ---- parse_gemini_sse_frame ----------------------------------------------

TEST(GeminiApiTest, ParseSseFrame_ContentDeltaEmitsText)
{
    const std::string frame =
        R"(data: {"candidates":[{"content":{"parts":[{"text":"hello"}],"role":"model"}}]})";

    auto r = parse_gemini_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, "hello");
}

TEST(GeminiApiTest, ParseSseFrame_EmptyTextReturnsNoDelta)
{
    const std::string frame =
        R"(data: {"candidates":[{"content":{"parts":[{"text":""}],"role":"model"},"finishReason":"STOP"}]})";

    auto r = parse_gemini_sse_frame(frame);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(GeminiApiTest, ParseSseFrame_MalformedDataReturnsParseError)
{
    const std::string frame = "data: not-a-json-object";
    auto r = parse_gemini_sse_frame(frame);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

} // namespace
