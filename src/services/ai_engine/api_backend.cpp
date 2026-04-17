#include "services/ai_engine/api_backend.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/result.h"
#include "platform/http_client.h"
#include "platform/process.h"
#include "services/ai_engine/claude_api.h"
#include "services/ai_engine/gemini_api.h"
#include "services/ai_engine/openai_api.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

/// Cap the response-body preview we echo into error messages. Keeps
/// the log sane when a provider returns a large HTML error page and
/// limits the blast radius if a body ever contained echoed request
/// fragments.
constexpr std::size_t k_error_body_preview = 512;

std::string preview_body(const std::string& body)
{
    if (body.size() <= k_error_body_preview) return body;
    std::string out;
    out.reserve(k_error_body_preview + 20);
    out.append(body, 0, k_error_body_preview);
    out.append("...[truncated]");
    return out;
}

/// Which env var a given API provider reads its key from.
std::string_view env_var_for(AIBackend provider)
{
    switch (provider) {
        case AIBackend::Claude: return "CLAUDE_API_KEY";
        case AIBackend::OpenAI: return "OPENAI_API_KEY";
        case AIBackend::Gemini: return "GEMINI_API_KEY";
        default:                return "";
    }
}

std::string_view provider_name(AIBackend provider)
{
    switch (provider) {
        case AIBackend::Claude: return "Claude";
        case AIBackend::OpenAI: return "OpenAI";
        case AIBackend::Gemini: return "Gemini";
        default:                return "API";
    }
}

std::string_view default_model(AIBackend provider)
{
    switch (provider) {
        case AIBackend::Claude: return k_claude_default_model;
        case AIBackend::OpenAI: return k_openai_default_model;
        case AIBackend::Gemini: return k_gemini_default_model;
        default:                return "";
    }
}

std::string gemini_url_for(std::string_view model, std::string_view suffix)
{
    std::string out;
    out.reserve(k_gemini_endpoint_base.size() + model.size() + suffix.size());
    out.append(k_gemini_endpoint_base);
    out.append(model);
    out.append(suffix);
    return out;
}

/// Split the rolling SSE buffer on the first blank-line delimiter
/// ("\n\n" or "\r\n\r\n"). Returns the leading complete frame (with
/// the delimiter trimmed) in `out_frame` and removes it plus the
/// delimiter from `buffer`. Returns false when no complete frame is
/// available yet.
bool extract_sse_frame(std::string& buffer, std::string& out_frame)
{
    const std::size_t lf_lf = buffer.find("\n\n");
    const std::size_t cr_lf = buffer.find("\r\n\r\n");

    std::size_t end;
    std::size_t delim_len;
    if (lf_lf == std::string::npos && cr_lf == std::string::npos) {
        return false;
    }
    if (cr_lf != std::string::npos &&
        (lf_lf == std::string::npos || cr_lf < lf_lf))
    {
        end       = cr_lf;
        delim_len = 4;
    } else {
        end       = lf_lf;
        delim_len = 2;
    }

    out_frame.assign(buffer, 0, end);
    buffer.erase(0, end + delim_len);
    return true;
}

} // namespace

APIBackend::APIBackend(AIBackend                     provider,
                       vectis::platform::HttpClient& http,
                       std::string                   model)
    : m_provider(provider), m_http(&http), m_model(std::move(model))
{
    const auto env = env_var_for(provider);
    if (!env.empty()) {
        auto key = vectis::platform::get_env(env);
        if (key && !key->empty()) {
            m_api_key = std::move(*key);
        }
    }

    if (m_model.empty()) {
        m_model.assign(default_model(provider));
    }

    VECTIS_LOG_INFO("APIBackend({}) constructed, key={}",
                    provider_name(provider),
                    m_api_key.empty() ? "absent" : "present");
}

bool APIBackend::is_available() const
{
    return !m_api_key.empty();
}

std::string APIBackend::display_name() const
{
    return std::string(provider_name(m_provider)) +
           (is_available() ? " (online)" : " (unavailable)");
}

Result<AIResponse> APIBackend::generate(const AIRequest& request)
{
    if (!is_available()) {
        return make_error(ErrorKind::AIError,
                          std::string(provider_name(m_provider)) +
                              " API key not set");
    }
    if (m_http == nullptr) {
        return make_error(ErrorKind::NetworkError, "HttpClient unavailable");
    }

    const auto start = std::chrono::steady_clock::now();

    vectis::platform::HttpRequest http_req;
    http_req.method     = "POST";
    http_req.timeout_ms = 60000;
    http_req.headers["content-type"] = "application/json";

    if (m_provider == AIBackend::Claude) {
        http_req.url  = std::string(k_claude_endpoint);
        http_req.body = build_claude_request(request, m_model, false).dump();
        http_req.headers["x-api-key"]         = m_api_key;
        http_req.headers["anthropic-version"] = "2023-06-01";
    } else if (m_provider == AIBackend::OpenAI) {
        http_req.url  = std::string(k_openai_endpoint);
        http_req.body = build_openai_request(request, m_model, false).dump();
        http_req.headers["authorization"] = "Bearer " + m_api_key;
    } else if (m_provider == AIBackend::Gemini) {
        http_req.url  = gemini_url_for(m_model, k_gemini_gen_suffix);
        http_req.body = build_gemini_request(request).dump();
        http_req.headers["x-goog-api-key"] = m_api_key;
    } else {
        // Defensive: APIBackend is constructed only with Claude / OpenAI
        // / Gemini in production; Ollama and GGML each have their own
        // class. If anything reaches here it means the constructor was
        // misused — fail loudly rather than pretending to do work.
        return make_error(ErrorKind::AIError,
                          "APIBackend does not handle " +
                              std::string(provider_name(m_provider)));
    }

    auto resp = m_http->send(http_req);
    if (!resp) {
        return tl::unexpected(resp.error());
    }
    if (resp->status_code != 200) {
        return make_error(ErrorKind::AIError,
                          std::string(provider_name(m_provider)) +
                              " API returned HTTP " +
                              std::to_string(resp->status_code) +
                              ": " + preview_body(resp->body));
    }

    Result<AIResponse> parsed = AIResponse{};
    switch (m_provider) {
        case AIBackend::Claude: parsed = parse_claude_response(resp->body); break;
        case AIBackend::OpenAI: parsed = parse_openai_response(resp->body); break;
        case AIBackend::Gemini: parsed = parse_gemini_response(resp->body); break;
        default: break;
    }
    if (!parsed) return parsed;
    parsed->backend_used = m_provider;
    const auto end = std::chrono::steady_clock::now();
    parsed->latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    return parsed;
}

void APIBackend::generate_stream(const AIRequest&   request,
                                 StreamCallback     on_token,
                                 std::atomic<bool>& cancel_flag,
                                 StreamComplete     on_complete)
{
    auto fail = [&](ErrorKind kind, std::string msg) {
        if (on_complete) on_complete(make_error(kind, std::move(msg)));
    };

    if (!is_available()) {
        fail(ErrorKind::AIError,
             std::string(provider_name(m_provider)) + " API key not set");
        return;
    }
    if (m_http == nullptr) {
        fail(ErrorKind::NetworkError, "HttpClient unavailable");
        return;
    }

    const auto start = std::chrono::steady_clock::now();

    vectis::platform::HttpStreamRequest http_req;
    http_req.method      = "POST";
    http_req.timeout_ms  = 120000;
    http_req.cancel_flag = &cancel_flag;
    http_req.headers["content-type"] = "application/json";
    http_req.headers["accept"]       = "text/event-stream";

    // Per-provider: endpoint, request body, auth header, SSE parser.
    using SseParser =
        Result<std::optional<std::string>>(*)(std::string_view);
    SseParser parser = nullptr;

    if (m_provider == AIBackend::Claude) {
        http_req.url  = std::string(k_claude_endpoint);
        http_req.body = build_claude_request(request, m_model, true).dump();
        http_req.headers["x-api-key"]         = m_api_key;
        http_req.headers["anthropic-version"] = "2023-06-01";
        parser = &parse_claude_sse_frame;
    } else if (m_provider == AIBackend::OpenAI) {
        http_req.url  = std::string(k_openai_endpoint);
        http_req.body = build_openai_request(request, m_model, true).dump();
        http_req.headers["authorization"] = "Bearer " + m_api_key;
        parser = &parse_openai_sse_frame;
    } else if (m_provider == AIBackend::Gemini) {
        http_req.url  = gemini_url_for(m_model, k_gemini_stream_suffix);
        http_req.body = build_gemini_request(request).dump();
        http_req.headers["x-goog-api-key"] = m_api_key;
        parser = &parse_gemini_sse_frame;
    } else {
        // Defensive — see note in generate(). APIBackend is only
        // constructed for the three HTTP providers.
        fail(ErrorKind::AIError,
             "APIBackend does not handle " +
                 std::string(provider_name(m_provider)));
        return;
    }

    // Rolling state captured by reference in the chunk callback.
    std::string buffer;
    std::string accumulated;

    http_req.on_chunk = [&](std::string_view chunk) -> bool {
        buffer.append(chunk);
        std::string frame;
        while (extract_sse_frame(buffer, frame)) {
            auto delta = parser(frame);
            if (!delta) continue;  // malformed frame — keep streaming
            if (delta->has_value() && !(*delta)->empty()) {
                if (on_token) on_token(**delta);
                accumulated.append(**delta);
            }
            frame.clear();
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
             std::string(provider_name(m_provider)) +
                 " API returned HTTP " + std::to_string(resp->status_code));
        return;
    }

    AIResponse final;
    final.text         = std::move(accumulated);
    final.backend_used = m_provider;
    final.latency_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    if (on_complete) on_complete(final);
}

} // namespace vectis::services
