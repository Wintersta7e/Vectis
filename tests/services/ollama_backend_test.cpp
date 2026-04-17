#include "services/ai_engine/ollama_backend.h"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include "services/ai_engine/ai_engine.h"

namespace {

using vectis::services::AIRequest;
using vectis::services::build_ollama_request;
using vectis::services::k_ollama_default_model;
using vectis::services::parse_ollama_ndjson_chunk;
using vectis::services::parse_ollama_response;

// ---- build_ollama_request -------------------------------------------------

TEST(OllamaBackendTest, BuildRequest_IncludesRequiredFields)
{
    AIRequest req;
    req.user_prompt   = "Hello";
    req.system_prompt = "You are helpful.";
    req.max_tokens    = 256;
    req.temperature   = 0.7F;

    const auto body = build_ollama_request(req, k_ollama_default_model, false);

    EXPECT_EQ(body["model"], std::string(k_ollama_default_model));
    EXPECT_EQ(body["prompt"], "Hello");
    EXPECT_EQ(body["system"], "You are helpful.");
    EXPECT_FALSE(body["stream"].get<bool>());
    EXPECT_EQ(body["options"]["num_predict"], 256);
    EXPECT_FLOAT_EQ(body["options"]["temperature"].get<float>(), 0.7F);
}

TEST(OllamaBackendTest, BuildRequest_StreamTrueFlagPropagates)
{
    AIRequest req;
    req.user_prompt = "hi";
    const auto body = build_ollama_request(req, "llama3", true);
    EXPECT_TRUE(body["stream"].get<bool>());
}

TEST(OllamaBackendTest, BuildRequest_MergesContextIntoPrompt)
{
    AIRequest req;
    req.context     = "class Foo {};";
    req.user_prompt = "document it";
    const auto body = build_ollama_request(req, "llama3", false);
    const std::string prompt = body["prompt"];
    EXPECT_NE(prompt.find("class Foo"), std::string::npos);
    EXPECT_NE(prompt.find("document it"), std::string::npos);
}

// ---- parse_ollama_response ------------------------------------------------

TEST(OllamaBackendTest, ParseResponse_ExtractsFields)
{
    const std::string json = R"json({
        "model": "llama3",
        "created_at": "2026-04-17T12:00:00Z",
        "response": "Hi there",
        "done": true,
        "prompt_eval_count": 7,
        "eval_count": 12,
        "total_duration": 100000000
    })json";

    auto r = parse_ollama_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "Hi there");
    EXPECT_EQ(r->prompt_tokens, 7);
    EXPECT_EQ(r->completion_tokens, 12);
    EXPECT_FALSE(r->was_truncated);
}

TEST(OllamaBackendTest, ParseResponse_TruncatedDoneReason)
{
    const std::string json = R"json({
        "response": "cut off...",
        "done": true,
        "done_reason": "length"
    })json";

    auto r = parse_ollama_response(json);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->was_truncated);
}

TEST(OllamaBackendTest, ParseResponse_MalformedJsonReturnsParseError)
{
    auto r = parse_ollama_response("not json at all");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

// ---- parse_ollama_ndjson_chunk -------------------------------------------

TEST(OllamaBackendTest, ParseNdjsonChunk_ResponseLineYieldsText)
{
    const std::string line =
        R"({"model":"llama3","response":"hello","done":false})";
    auto r = parse_ollama_ndjson_chunk(line);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(**r, "hello");
}

TEST(OllamaBackendTest, ParseNdjsonChunk_DoneLineYieldsNoDelta)
{
    const std::string line =
        R"({"model":"llama3","response":"","done":true,"eval_count":20})";
    auto r = parse_ollama_ndjson_chunk(line);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(OllamaBackendTest, ParseNdjsonChunk_EmptyLineIsNotError)
{
    auto r = parse_ollama_ndjson_chunk("");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(OllamaBackendTest, ParseNdjsonChunk_MalformedLineReturnsParseError)
{
    auto r = parse_ollama_ndjson_chunk(R"({"model":"llama3",)");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::ParseError);
}

} // namespace
