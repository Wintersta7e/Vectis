#pragma once

#include <atomic>
#include <string>

#include "core/result.h"
#include "services/ai_engine/ai_engine.h"

namespace vectis::services {

/// Pure-virtual contract implemented by every concrete backend
/// (`APIBackend` for Claude/OpenAI/Gemini, `OllamaBackend`, `GGMLBackend`).
/// `AIEngine` holds a `std::vector<std::unique_ptr<IBackend>>` and
/// dispatches `query()`/`query_stream()` to the first available one.
class IBackend {
public:
    virtual ~IBackend() = default;

    IBackend(const IBackend&)            = delete;
    IBackend& operator=(const IBackend&) = delete;
    IBackend(IBackend&&)                 = delete;
    IBackend& operator=(IBackend&&)      = delete;

    /// Which `AIBackend` enum value this implementation corresponds to.
    [[nodiscard]] virtual AIBackend kind() const = 0;

    /// Whether the backend can service a request right now. May touch
    /// the network (Ollama `/api/tags`), check env vars (API keys), or
    /// check a loaded model (GGML). Cheap enough to call per query; do
    /// internal caching when appropriate.
    [[nodiscard]] virtual bool is_available() const = 0;

    /// Short human-readable label for the UI
    /// (e.g., "Claude (online)", "Ollama: llama3").
    [[nodiscard]] virtual std::string display_name() const = 0;

    /// Synchronous generation. Returns a populated `AIResponse` with
    /// `backend_used` set to `kind()`, or an `Error` on failure.
    [[nodiscard]] virtual vectis::core::Result<AIResponse>
    generate(const AIRequest& request) = 0;

    /// Streaming generation. `on_token` is invoked per decoded token.
    /// `cancel_flag` is polled between tokens — when it becomes true
    /// the backend should stop as soon as practical and invoke
    /// `on_complete` with the partial response so far. `on_complete`
    /// must be called exactly once, either with the final response
    /// or an `Error`.
    virtual void generate_stream(const AIRequest&   request,
                                 StreamCallback     on_token,
                                 std::atomic<bool>& cancel_flag,
                                 StreamComplete     on_complete) = 0;

protected:
    IBackend() = default;
};

} // namespace vectis::services
