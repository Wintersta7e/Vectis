#pragma once

#include <atomic>
#include <string>

#include "core/result.h"
#include "platform/http_client.h"
#include "services/ai_engine/backend.h"

namespace vectis::services {

/// HTTP-based AI backend for Claude, OpenAI, and Gemini. The provider
/// is fixed at construction and determines the endpoint, request shape,
/// and streaming chunk format (SSE vs NDJSON). One `APIBackend` instance
/// per provider; AIEngine owns them all.
///
/// Each backend owns its own `HttpClient` (one `CURL*` handle per
/// backend). libcurl easy handles are not thread-safe, so sharing a
/// handle between this backend and anything else on a different
/// thread (status-line probes, other backends) is UB. Giving each
/// backend its own handle eliminates that class of race by
/// construction.
class APIBackend final : public IBackend {
public:
    /// Construct the backend for a given provider. Reads the provider-
    /// specific API key from the environment (CLAUDE_API_KEY /
    /// OPENAI_API_KEY / GEMINI_API_KEY). `model` is the model slug
    /// passed in the request body; leave empty to use the per-provider
    /// default.
    explicit APIBackend(AIBackend   provider,
                        std::string model = "");

    [[nodiscard]] AIBackend   kind()         const override { return m_provider; }
    [[nodiscard]] bool        is_available() const override;
    [[nodiscard]] std::string display_name() const override;

    [[nodiscard]] vectis::core::Result<AIResponse>
    generate(const AIRequest& request) override;

    void generate_stream(const AIRequest&   request,
                         StreamCallback     on_token,
                         std::atomic<bool>& cancel_flag,
                         StreamComplete     on_complete) override;

private:
    AIBackend                     m_provider;
    vectis::platform::HttpClient  m_http;
    std::string                   m_api_key;
    std::string                   m_model;
};

} // namespace vectis::services
