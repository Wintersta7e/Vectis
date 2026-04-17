#include "services/ai_engine/ai_engine.h"

#include <chrono>
#include <future>

#include <gtest/gtest.h>

#include "core/config_manager.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace {

using vectis::core::ConfigManager;
using vectis::core::ErrorKind;
using vectis::platform::HttpClient;
using vectis::services::AIBackend;
using vectis::services::AIEngine;
using vectis::services::AIRequest;

class AIEngineSkeletonTest : public ::testing::Test {
protected:
    ConfigManager m_config;
    HttpClient    m_http;
    AIEngine      m_engine{m_config, m_http};
};

TEST_F(AIEngineSkeletonTest, DefaultConstructIsNotReady)
{
    EXPECT_FALSE(m_engine.is_ready());
    EXPECT_EQ(m_engine.status_text(), "AI: not configured");
    EXPECT_TRUE(m_engine.available_backends().empty());
}

TEST_F(AIEngineSkeletonTest, QueryWithNoBackendsReturnsAIError)
{
    AIRequest req;
    req.user_prompt = "hello";
    auto r = m_engine.query(req);

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::AIError);
    EXPECT_FALSE(r.error().message.empty());
}

TEST_F(AIEngineSkeletonTest, SetPreferredBackendStoresValue)
{
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Auto);
    m_engine.set_preferred_backend(AIBackend::Claude);
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Claude);
    m_engine.set_preferred_backend(AIBackend::Auto);
    EXPECT_EQ(m_engine.active_backend(), AIBackend::Auto);
}

TEST_F(AIEngineSkeletonTest, QueryAsyncReturnsErrorFuture)
{
    AIRequest req;
    req.user_prompt = "anything";
    auto fut = m_engine.query_async(req);

    const auto status = fut.wait_for(std::chrono::seconds(2));
    ASSERT_EQ(status, std::future_status::ready);

    const auto result = fut.get();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::AIError);
}

TEST_F(AIEngineSkeletonTest, QueryStreamInvokesOnCompleteWithError)
{
    AIRequest req;
    req.user_prompt = "stream me";
    bool completed = false;
    bool token_received = false;

    m_engine.query_stream(
        req,
        [&token_received](std::string_view) { token_received = true; },
        [&completed](vectis::core::Result<vectis::services::AIResponse> r) {
            completed = true;
            EXPECT_FALSE(r.has_value());
            if (!r.has_value()) {
                EXPECT_EQ(r.error().kind, ErrorKind::AIError);
            }
        });

    EXPECT_TRUE(completed);
    EXPECT_FALSE(token_received);
}

TEST_F(AIEngineSkeletonTest, CancelStreamIsSafeWithNoActiveStream)
{
    // Should be a no-op; no crash, no state change observable.
    m_engine.cancel_stream();
    EXPECT_FALSE(m_engine.is_ready());
}

} // namespace
