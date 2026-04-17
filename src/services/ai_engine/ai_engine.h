#pragma once

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace vectis::core {
class ConfigManager;
} // namespace vectis::core

namespace vectis::platform {
class HttpClient;
} // namespace vectis::platform

namespace vectis::services {

/// Which AI backend a query should target (or which is currently active).
enum class AIBackend {
    Auto,    ///< Let AIEngine pick the best available backend.
    GGML,    ///< Embedded llama.cpp (opt-in via VECTIS_BUILD_GGML).
    Claude,  ///< Anthropic API via HTTPS.
    OpenAI,  ///< OpenAI chat completions API via HTTPS.
    Gemini,  ///< Google Generative Language API via HTTPS.
    Ollama,  ///< Local Ollama server via HTTP.
};

/// Prompt + decoding parameters for a single AI call.
struct AIRequest {
    std::string system_prompt;
    std::string user_prompt;
    std::string context;       ///< Additional context (code, web results, etc.)
    int         max_tokens   = 1024;
    float       temperature  = 0.3F;
    bool        stream       = false;
};

/// Response from a completed AI call.
struct AIResponse {
    std::string text;
    AIBackend   backend_used      = AIBackend::Auto;
    int         prompt_tokens     = 0;
    int         completion_tokens = 0;
    double      latency_ms        = 0.0;
    bool        was_truncated     = false;
};

/// Callback invoked once per streamed token (or token-like chunk).
using StreamCallback = std::function<void(std::string_view token)>;

/// Callback invoked once at the end of a stream with the full response or error.
using StreamComplete = std::function<void(vectis::core::Result<AIResponse>)>;

/// Forward declaration of the backend contract. Full definition in
/// `services/ai_engine/backend.h` — `ai_engine.h` itself stays lean so
/// callers that only want the request/response types don't drag in
/// the polymorphic hierarchy.
class IBackend;

/// Unified AI service. Holds a priority-ordered list of backends and
/// dispatches queries to the first one that is available (or to the
/// user-specified preference).
///
/// Thread safety: instances are not internally synchronized. Call from
/// one thread, or guard with your own mutex. Individual backends may
/// be invoked concurrently by different AIEngine instances.
class AIEngine {
public:
    /// Production constructor — instantiates the full backend set
    /// (Ollama, Claude/OpenAI/Gemini API backends, GGML stub-or-real).
    AIEngine(vectis::core::ConfigManager&  config,
             vectis::platform::HttpClient& http);

    /// Test-only constructor — inject a prebuilt backend list. The
    /// order you pass determines auto-selection priority.
    explicit AIEngine(std::vector<std::unique_ptr<IBackend>> backends);

    ~AIEngine();

    AIEngine(const AIEngine&)            = delete;
    AIEngine& operator=(const AIEngine&) = delete;
    AIEngine(AIEngine&&) noexcept;
    AIEngine& operator=(AIEngine&&) noexcept;

    /// Synchronous query. Blocks the calling thread until the response
    /// arrives or an error is produced.
    [[nodiscard]] vectis::core::Result<AIResponse> query(const AIRequest& request);

    /// Asynchronous query — wraps `query()` in `std::async`. Caller owns
    /// the future and must wait/get before the AIEngine is destroyed.
    [[nodiscard]] std::future<vectis::core::Result<AIResponse>>
    query_async(const AIRequest& request);

    /// Streaming query. `on_token` is invoked on the caller's thread for
    /// each decoded token; `on_complete` is invoked exactly once at the
    /// end with the full response or error. AskMode and other UI callers
    /// should dispatch this onto a TaskQueue to avoid blocking the render
    /// loop — AIEngine performs no thread marshalling of its own.
    void query_stream(const AIRequest& request,
                      StreamCallback   on_token,
                      StreamComplete   on_complete);

    /// Request cancellation of the currently active streaming query.
    /// Safe to call from a different thread than `query_stream`.
    void cancel_stream();

    /// Backend currently selected for auto-routing (the preferred one if
    /// set and available, otherwise the first available backend).
    [[nodiscard]] AIBackend active_backend() const;

    /// List of backends that self-report as available right now. Useful
    /// for UI pickers that want to grey out backends with missing keys.
    [[nodiscard]] std::vector<AIBackend> available_backends() const;

    /// Override the auto-selection. Pass `AIBackend::Auto` to clear.
    void set_preferred_backend(AIBackend backend);

    /// Whether any backend is currently available.
    [[nodiscard]] bool is_ready() const;

    /// Human-readable status line for the UI footer
    /// (e.g., "Claude (online)", "Ollama: llama3", "AI: not configured").
    [[nodiscard]] std::string status_text() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::services
