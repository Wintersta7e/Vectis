#include "services/ai_engine/ollama_backend.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr std::string_view k_context_separator = "\n\n---\n\n";

std::string build_user_prompt(const AIRequest& req)
{
    if (req.context.empty()) return req.user_prompt;
    std::string out;
    out.reserve(req.context.size() + k_context_separator.size() +
                req.user_prompt.size());
    out.append(req.context);
    out.append(k_context_separator);
    out.append(req.user_prompt);
    return out;
}

} // namespace

nlohmann::json build_ollama_request(const AIRequest& req,
                                    std::string_view model,
                                    bool             stream)
{
    nlohmann::json body;
    body["model"]  = std::string(model);
    body["prompt"] = build_user_prompt(req);
    body["stream"] = stream;
    if (!req.system_prompt.empty()) {
        body["system"] = req.system_prompt;
    }
    body["options"] = {
        {"num_predict", req.max_tokens},
        {"temperature", req.temperature},
    };
    return body;
}

Result<AIResponse> parse_ollama_response(const std::string& body)
{
    try {
        const auto root = nlohmann::json::parse(body);

        AIResponse resp;
        resp.text              = root.value("response", "");
        resp.prompt_tokens     = root.value("prompt_eval_count", 0);
        resp.completion_tokens = root.value("eval_count", 0);
        resp.was_truncated     =
            (root.value("done_reason", "") == "length");
        return resp;
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Ollama response JSON parse failed: ") + e.what());
    }
}

Result<std::optional<std::string>>
parse_ollama_ndjson_chunk(std::string_view line)
{
    // Strip trailing whitespace the streaming HTTP layer may include.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' '))
    {
        line.remove_suffix(1);
    }
    if (line.empty()) {
        return std::optional<std::string>{};
    }

    try {
        const auto payload = nlohmann::json::parse(line);
        auto text = payload.value("response", std::string{});
        if (text.empty()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{std::move(text)};
    }
    catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Ollama NDJSON chunk parse failed: ") + e.what());
    }
}

// ============================================================================
// OllamaBackend
// ============================================================================

OllamaBackend::OllamaBackend(vectis::platform::HttpClient& http,
                             std::string                   endpoint,
                             std::string                   model)
    : m_http(&http), m_endpoint(std::move(endpoint)), m_model(std::move(model))
{
    VECTIS_LOG_INFO("OllamaBackend constructed (endpoint={}, model={})",
                    m_endpoint, m_model);
}

bool OllamaBackend::is_available() const
{
    const std::lock_guard<std::mutex> lock(m_probe_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (m_probe_valid && (now - m_probe_at) < k_probe_ttl) {
        return m_probe_result;
    }

    bool ok = false;
    if (m_http != nullptr) {
        vectis::platform::HttpRequest req;
        req.method     = "GET";
        req.url        = m_endpoint + "/api/tags";
        req.timeout_ms = 3000;

        auto resp = m_http->send(req);
        ok = resp && resp->status_code == 200;
    }

    m_probe_result = ok;
    m_probe_at     = now;
    m_probe_valid  = true;
    return ok;
}

std::string OllamaBackend::display_name() const
{
    if (is_available()) {
        return "Ollama: " + m_model;
    }
    return "Ollama (unavailable)";
}

Result<AIResponse> OllamaBackend::generate(const AIRequest& request)
{
    if (!is_available()) {
        return make_error(ErrorKind::AIError, "Ollama server not reachable");
    }
    if (m_http == nullptr) {
        return make_error(ErrorKind::NetworkError, "HttpClient unavailable");
    }

    const auto start = std::chrono::steady_clock::now();
    const auto body  = build_ollama_request(request, m_model, false).dump();

    vectis::platform::HttpRequest req;
    req.method     = "POST";
    req.url        = m_endpoint + "/api/generate";
    req.timeout_ms = 120000;
    req.body       = body;
    req.headers["content-type"] = "application/json";

    auto resp = m_http->send(req);
    if (!resp) return tl::unexpected(resp.error());
    if (resp->status_code != 200) {
        return make_error(ErrorKind::AIError,
                          "Ollama returned HTTP " +
                              std::to_string(resp->status_code) +
                              ": " + resp->body);
    }

    auto parsed = parse_ollama_response(resp->body);
    if (!parsed) return parsed;

    parsed->backend_used = AIBackend::Ollama;
    const auto end = std::chrono::steady_clock::now();
    parsed->latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    return parsed;
}

void OllamaBackend::generate_stream(const AIRequest&   request,
                                    StreamCallback     on_token,
                                    std::atomic<bool>& cancel_flag,
                                    StreamComplete     on_complete)
{
    auto fail = [&](ErrorKind kind, std::string msg) {
        if (on_complete) on_complete(make_error(kind, std::move(msg)));
    };

    if (!is_available()) {
        fail(ErrorKind::AIError, "Ollama server not reachable");
        return;
    }
    if (m_http == nullptr) {
        fail(ErrorKind::NetworkError, "HttpClient unavailable");
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    const auto body  = build_ollama_request(request, m_model, true).dump();

    // Rolling buffer captured by the chunk callback — Ollama emits
    // NDJSON, so we accumulate and split on '\n'.
    std::string buffer;
    std::string accumulated;

    vectis::platform::HttpStreamRequest http_req;
    http_req.method      = "POST";
    http_req.url         = m_endpoint + "/api/generate";
    http_req.timeout_ms  = 120000;
    http_req.body        = body;
    http_req.cancel_flag = &cancel_flag;
    http_req.headers["content-type"] = "application/json";

    http_req.on_chunk = [&](std::string_view chunk) -> bool {
        buffer.append(chunk);

        std::size_t nl;
        while ((nl = buffer.find('\n')) != std::string::npos) {
            const std::string_view line(buffer.data(), nl);
            auto delta = parse_ollama_ndjson_chunk(line);
            if (delta && delta->has_value()) {
                if (on_token) on_token(**delta);
                accumulated.append(**delta);
            }
            buffer.erase(0, nl + 1);
        }
        return !cancel_flag.load(std::memory_order_acquire);
    };

    auto resp = m_http->send_streaming(http_req);
    const auto end = std::chrono::steady_clock::now();

    if (!resp) {
        if (on_complete) on_complete(tl::unexpected(resp.error()));
        return;
    }
    if (resp->status_code != 200) {
        fail(ErrorKind::AIError,
             "Ollama returned HTTP " + std::to_string(resp->status_code));
        return;
    }

    AIResponse final;
    final.text         = std::move(accumulated);
    final.backend_used = AIBackend::Ollama;
    final.latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    if (on_complete) on_complete(final);
}

} // namespace vectis::services
