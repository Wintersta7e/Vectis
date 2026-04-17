#include "services/ai_engine/ai_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/config_manager.h"
#include "core/log.h"
#include "platform/http_client.h"
#include "services/ai_engine/api_backend.h"
#include "services/ai_engine/backend.h"
#include "services/ai_engine/ollama_backend.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr const char* k_status_not_configured = "AI: not configured";

} // namespace

// -----------------------------------------------------------------------------
// AIEngine::Impl
// -----------------------------------------------------------------------------

struct AIEngine::Impl {
    std::vector<std::unique_ptr<IBackend>> backends;
    AIBackend                              preferred = AIBackend::Auto;
    std::atomic<bool>                      cancel_flag{false};
    mutable std::mutex                     mutex;

    /// Pick a backend for dispatch. Caller must hold `mutex`.
    IBackend* select_backend_locked()
    {
        // Explicit user preference wins if available.
        if (preferred != AIBackend::Auto) {
            for (auto& b : backends) {
                if (b && b->kind() == preferred && b->is_available()) {
                    return b.get();
                }
            }
        }
        // Otherwise the first available backend in declared priority order.
        for (auto& b : backends) {
            if (b && b->is_available()) {
                return b.get();
            }
        }
        return nullptr;
    }
};

// -----------------------------------------------------------------------------
// Constructors
// -----------------------------------------------------------------------------

AIEngine::AIEngine(vectis::core::ConfigManager&  /*config*/,
                   vectis::platform::HttpClient& http)
    : m_impl(std::make_unique<Impl>())
{
    // Priority order: local Ollama (fast, private, free) first; then
    // API providers in the canonical ordering; GGML will be appended
    // by Phase G when the VECTIS_BUILD_GGML flag gates it in.
    m_impl->backends.emplace_back(std::make_unique<OllamaBackend>(http));
    m_impl->backends.emplace_back(std::make_unique<APIBackend>(AIBackend::Claude, http));
    m_impl->backends.emplace_back(std::make_unique<APIBackend>(AIBackend::OpenAI, http));
    m_impl->backends.emplace_back(std::make_unique<APIBackend>(AIBackend::Gemini, http));

    VECTIS_LOG_INFO("AIEngine constructed with {} backends",
                    m_impl->backends.size());
}

AIEngine::AIEngine(std::vector<std::unique_ptr<IBackend>> backends)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->backends = std::move(backends);
    VECTIS_LOG_INFO("AIEngine constructed with {} injected backends",
                    m_impl->backends.size());
}

AIEngine::~AIEngine() = default;

// -----------------------------------------------------------------------------
// Querying
// -----------------------------------------------------------------------------

Result<AIResponse> AIEngine::query(const AIRequest& request)
{
    IBackend* backend = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        backend = m_impl->select_backend_locked();
    }
    if (backend == nullptr) {
        return make_error(ErrorKind::AIError, "no AI backend available");
    }

    const auto start = std::chrono::steady_clock::now();
    auto result = backend->generate(request);
    const auto end = std::chrono::steady_clock::now();

    if (result) {
        result->backend_used = backend->kind();
        if (result->latency_ms == 0.0) {
            result->latency_ms =
                std::chrono::duration<double, std::milli>(end - start).count();
        }
    }
    return result;
}

std::future<Result<AIResponse>> AIEngine::query_async(const AIRequest& request)
{
    return std::async(std::launch::async,
                      [this, request]() { return query(request); });
}

void AIEngine::query_stream(const AIRequest& request,
                            StreamCallback   on_token,
                            StreamComplete   on_complete)
{
    IBackend* backend = nullptr;
    {
        const std::lock_guard<std::mutex> lock(m_impl->mutex);
        backend = m_impl->select_backend_locked();
    }
    if (backend == nullptr) {
        if (on_complete) {
            on_complete(make_error(ErrorKind::AIError,
                                   "no AI backend available"));
        }
        return;
    }

    // Honor a cancel that fired BEFORE this call started — e.g., a
    // shutdown path that calls cancel_stream() while the worker was
    // still in context-assembly. Without this check the next line
    // would clobber the flag and let the backend run to completion
    // (or HTTP timeout) despite the caller's intent.
    if (m_impl->cancel_flag.load(std::memory_order_acquire)) {
        if (on_complete) {
            on_complete(make_error(ErrorKind::Cancelled,
                                   "stream cancelled before start"));
        }
        return;
    }

    // Reset cancellation for this stream. cancel_stream() will flip
    // this back to true; each backend polls it between tokens.
    m_impl->cancel_flag.store(false, std::memory_order_release);

    backend->generate_stream(request, std::move(on_token),
                             m_impl->cancel_flag, std::move(on_complete));
}

void AIEngine::cancel_stream()
{
    m_impl->cancel_flag.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Inspection
// -----------------------------------------------------------------------------

AIBackend AIEngine::active_backend() const
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->preferred != AIBackend::Auto) {
        return m_impl->preferred;
    }
    // When preference is Auto, report the first available backend.
    for (const auto& b : m_impl->backends) {
        if (b && b->is_available()) {
            return b->kind();
        }
    }
    return AIBackend::Auto;
}

std::vector<AIBackend> AIEngine::available_backends() const
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::vector<AIBackend> out;
    out.reserve(m_impl->backends.size());
    for (const auto& b : m_impl->backends) {
        if (b && b->is_available()) {
            out.push_back(b->kind());
        }
    }
    return out;
}

void AIEngine::set_preferred_backend(AIBackend backend)
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->preferred = backend;
}

bool AIEngine::is_ready() const
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    return std::any_of(m_impl->backends.begin(), m_impl->backends.end(),
                       [](const std::unique_ptr<IBackend>& b) {
                           return b && b->is_available();
                       });
}

std::string AIEngine::status_text() const
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);

    // If the user set a preference, describe that backend — even if
    // it's currently unavailable, so the user sees their choice
    // reflected with the unavailability note.
    if (m_impl->preferred != AIBackend::Auto) {
        for (const auto& b : m_impl->backends) {
            if (b && b->kind() == m_impl->preferred) {
                return b->display_name();
            }
        }
    }
    // Otherwise name the first available backend, or the generic not-
    // configured status.
    for (const auto& b : m_impl->backends) {
        if (b && b->is_available()) {
            return b->display_name();
        }
    }
    return k_status_not_configured;
}

} // namespace vectis::services
