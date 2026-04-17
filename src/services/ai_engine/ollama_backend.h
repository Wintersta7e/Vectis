#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"
#include "platform/http_client.h"
#include "services/ai_engine/backend.h"

namespace vectis::services {

constexpr std::string_view k_ollama_default_endpoint = "http://localhost:11434";
constexpr std::string_view k_ollama_default_model    = "llama3";

/// Build the JSON body for POST /api/generate. `options.num_predict`
/// carries `max_tokens`; `options.temperature` carries `temperature`.
[[nodiscard]] nlohmann::json build_ollama_request(const AIRequest& req,
                                                  std::string_view model,
                                                  bool             stream);

/// Parse a complete non-streamed Ollama response. Populates `text`
/// from `response`, usage from `prompt_eval_count` + `eval_count`, and
/// `was_truncated` from `done_reason == "length"` (Ollama mirrors the
/// OpenAI convention starting in recent versions).
[[nodiscard]] vectis::core::Result<AIResponse>
parse_ollama_response(const std::string& body);

/// Parse one NDJSON line. Returns the `response` chunk when present,
/// empty optional when the line is blank or the chunk text is empty
/// (terminal `"done":true` line included), ParseError for malformed
/// JSON lines.
[[nodiscard]] vectis::core::Result<std::optional<std::string>>
parse_ollama_ndjson_chunk(std::string_view line);

/// Local Ollama server backend. No API key — availability is
/// determined by a quick probe to `/api/tags`.
///
/// Owns its own HttpClient (one CURL handle per backend) — sharing a
/// handle with other backends or the status-line probe would be UB
/// under libcurl's thread rules.
class OllamaBackend final : public IBackend {
public:
    explicit OllamaBackend(std::string endpoint = std::string(k_ollama_default_endpoint),
                           std::string model    = std::string(k_ollama_default_model));

    [[nodiscard]] AIBackend   kind()         const override { return AIBackend::Ollama; }
    [[nodiscard]] bool        is_available() const override;
    [[nodiscard]] std::string display_name() const override;

    [[nodiscard]] vectis::core::Result<AIResponse>
    generate(const AIRequest& request) override;

    void generate_stream(const AIRequest&   request,
                         StreamCallback     on_token,
                         std::atomic<bool>& cancel_flag,
                         StreamComplete     on_complete) override;

private:
    // HttpClient is marked `mutable` because `is_available() const`
    // needs to dispatch a probe request through it. The handle is an
    // implementation detail (transport), not part of the observable
    // state the class exposes to callers.
    mutable vectis::platform::HttpClient m_http;
    std::string                          m_endpoint;
    std::string                          m_model;

    /// Availability cache. The probe hits the network (3 s timeout) so
    /// we memoise for `k_probe_ttl`; after that we re-probe so that
    /// starting `ollama serve` mid-session is eventually detected.
    static constexpr std::chrono::seconds k_probe_ttl{30};
    mutable std::mutex                           m_probe_mutex;
    mutable std::chrono::steady_clock::time_point m_probe_at{};
    mutable bool                                  m_probe_valid  = false;
    mutable bool                                  m_probe_result = false;
};

} // namespace vectis::services
