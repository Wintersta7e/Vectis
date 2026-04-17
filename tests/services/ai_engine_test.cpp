#include "services/ai_engine/ai_engine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/result.h"
#include "services/ai_engine/backend.h"

namespace {

using vectis::core::ErrorKind;
using vectis::services::AIBackend;
using vectis::services::AIEngine;
using vectis::services::AIRequest;
using vectis::services::AIResponse;
using vectis::services::IBackend;
using vectis::services::StreamCallback;
using vectis::services::StreamComplete;

/// Test fake: reports availability on demand, returns a canned response
/// or error, records how many times it was invoked.
class MockBackend final : public IBackend {
public:
    MockBackend(AIBackend   kind,
                bool        available,
                std::string reply_text)
        : m_kind(kind), m_available(available), m_reply(std::move(reply_text)) {}

    AIBackend   kind()         const override { return m_kind; }
    bool        is_available() const override { return m_available; }
    std::string display_name() const override
    {
        switch (m_kind) {
            case AIBackend::Claude: return m_available ? "Claude (online)" : "Claude (unavailable)";
            case AIBackend::OpenAI: return m_available ? "OpenAI (online)" : "OpenAI (unavailable)";
            case AIBackend::Gemini: return m_available ? "Gemini (online)" : "Gemini (unavailable)";
            case AIBackend::Ollama: return m_available ? "Ollama: fake"    : "Ollama (unavailable)";
            case AIBackend::GGML:   return m_available ? "GGML: fake"      : "GGML (unavailable)";
            default:                return "Auto";
        }
    }

    vectis::core::Result<AIResponse> generate(const AIRequest& /*req*/) override
    {
        ++m_generate_calls;
        AIResponse resp;
        resp.text         = m_reply;
        resp.backend_used = m_kind;
        resp.latency_ms   = 1.0; // nonzero so engine doesn't overwrite
        return resp;
    }

    void generate_stream(const AIRequest& /*req*/,
                         StreamCallback     /*on_token*/,
                         std::atomic<bool>& /*cancel_flag*/,
                         StreamComplete     on_complete) override
    {
        ++m_stream_calls;
        if (on_complete) {
            AIResponse resp;
            resp.text         = m_reply;
            resp.backend_used = m_kind;
            on_complete(std::move(resp));
        }
    }

    int generate_calls() const { return m_generate_calls; }
    int stream_calls()   const { return m_stream_calls; }

    void set_available(bool a) { m_available = a; }

private:
    AIBackend   m_kind;
    bool        m_available;
    std::string m_reply;
    int         m_generate_calls = 0;
    int         m_stream_calls   = 0;
};

// ---------------------------------------------------------------------------
// Skeleton tests (engine with no backends)
// ---------------------------------------------------------------------------

class AIEngineEmptyTest : public ::testing::Test {
protected:
    AIEngine m_engine{std::vector<std::unique_ptr<IBackend>>{}};
};

TEST_F(AIEngineEmptyTest, IsNotReadyWhenNoBackends)
{
    EXPECT_FALSE(m_engine.is_ready());
    EXPECT_EQ(m_engine.status_text(), "AI: not configured");
    EXPECT_TRUE(m_engine.available_backends().empty());
}

TEST_F(AIEngineEmptyTest, QueryReturnsAIError)
{
    AIRequest req;
    req.user_prompt = "hello";
    auto r = m_engine.query(req);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::AIError);
}

TEST_F(AIEngineEmptyTest, PreferredBackendStoredEvenIfUnavailable)
{
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Auto);
    m_engine.set_preferred_backend(AIBackend::Claude);
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Claude);
    m_engine.set_preferred_backend(AIBackend::Auto);
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Auto);
}

TEST_F(AIEngineEmptyTest, QueryAsyncReturnsErrorFuture)
{
    AIRequest req;
    auto fut = m_engine.query_async(req);
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    auto r = fut.get();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::AIError);
}

// ---------------------------------------------------------------------------
// Dispatch / selection tests (engine with mock backends)
// ---------------------------------------------------------------------------

TEST(AIEngineDispatchTest, FirstAvailableBackendWinsInAutoMode)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, true,  "from-ollama");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, true,  "from-claude");
    MockBackend* ollama_raw = ollama.get();
    MockBackend* claude_raw = claude.get();

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));

    AIEngine engine(std::move(backends));
    EXPECT_TRUE(engine.is_ready());
    EXPECT_EQ(engine.status_text(), "Ollama: fake");

    AIRequest req;
    req.user_prompt = "hi";
    auto r = engine.query(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->text, "from-ollama");
    EXPECT_EQ(r->backend_used, AIBackend::Ollama);

    EXPECT_EQ(ollama_raw->generate_calls(), 1);
    EXPECT_EQ(claude_raw->generate_calls(), 0);
}

TEST(AIEngineDispatchTest, PreferredBackendOverridesAutoOrder)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, true, "ollama");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, true, "claude");
    MockBackend* claude_raw = claude.get();

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));

    AIEngine engine(std::move(backends));
    engine.set_preferred_backend(AIBackend::Claude);

    AIRequest req;
    auto r = engine.query(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->backend_used, AIBackend::Claude);
    EXPECT_EQ(claude_raw->generate_calls(), 1);
}

TEST(AIEngineDispatchTest, FallsBackWhenPreferredUnavailable)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, true,  "ollama");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, false, "claude");
    MockBackend* ollama_raw = ollama.get();

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));

    AIEngine engine(std::move(backends));
    engine.set_preferred_backend(AIBackend::Claude);

    AIRequest req;
    auto r = engine.query(req);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->backend_used, AIBackend::Ollama);
    EXPECT_EQ(ollama_raw->generate_calls(), 1);
}

TEST(AIEngineDispatchTest, AvailableBackendsFiltersUnavailable)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, false, "");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, true,  "");
    auto openai = std::make_unique<MockBackend>(AIBackend::OpenAI, false, "");
    auto gemini = std::make_unique<MockBackend>(AIBackend::Gemini, true,  "");

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));
    backends.emplace_back(std::move(openai));
    backends.emplace_back(std::move(gemini));

    AIEngine engine(std::move(backends));
    const auto list = engine.available_backends();

    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[0], AIBackend::Claude);
    EXPECT_EQ(list[1], AIBackend::Gemini);
}

TEST(AIEngineDispatchTest, StatusReflectsPreferredBackendEvenWhenUnavailable)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, false, "");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, false, "");

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));

    AIEngine engine(std::move(backends));
    engine.set_preferred_backend(AIBackend::Claude);

    EXPECT_EQ(engine.status_text(), "Claude (unavailable)");
}

TEST(AIEngineDispatchTest, NoBackendAvailableReturnsError)
{
    auto ollama = std::make_unique<MockBackend>(AIBackend::Ollama, false, "");
    auto claude = std::make_unique<MockBackend>(AIBackend::Claude, false, "");

    std::vector<std::unique_ptr<IBackend>> backends;
    backends.emplace_back(std::move(ollama));
    backends.emplace_back(std::move(claude));

    AIEngine engine(std::move(backends));
    AIRequest req;
    auto r = engine.query(req);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::AIError);
}

} // namespace
