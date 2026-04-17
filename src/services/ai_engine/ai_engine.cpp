#include "services/ai_engine/ai_engine.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/config_manager.h"
#include "core/log.h"
#include "platform/http_client.h"
#include "services/ai_engine/backend.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr const char* k_status_not_configured = "AI: not configured";

} // namespace

// -----------------------------------------------------------------------------
// AIEngine::Impl — holds backends + selection state.
// -----------------------------------------------------------------------------

struct AIEngine::Impl {
    std::vector<std::unique_ptr<IBackend>> backends;
    AIBackend                              preferred = AIBackend::Auto;
    std::atomic<bool>                      cancel_flag{false};
    mutable std::mutex                     mutex;
};

// -----------------------------------------------------------------------------
// Constructors, destructor, move.
// -----------------------------------------------------------------------------

AIEngine::AIEngine(vectis::core::ConfigManager&  /*config*/,
                   vectis::platform::HttpClient& /*http*/)
    : m_impl(std::make_unique<Impl>())
{
    // Real backends wired in subsequent commits (Claude, OpenAI, Gemini,
    // Ollama, GGML). For now the backend list is empty and every
    // production query returns AIError.
    VECTIS_LOG_INFO("AIEngine constructed (no backends yet)");
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
// Querying — skeleton returns "no backend" until orchestration lands in
// Phase E. Once the real dispatch is in place these bodies will grow;
// the interface does not change.
// -----------------------------------------------------------------------------

Result<AIResponse> AIEngine::query(const AIRequest& /*request*/)
{
    return make_error(ErrorKind::AIError, "no AI backend available");
}

std::future<Result<AIResponse>> AIEngine::query_async(const AIRequest& request)
{
    return std::async(std::launch::async,
                      [this, request]() { return query(request); });
}

void AIEngine::query_stream(const AIRequest& /*request*/,
                            StreamCallback   /*on_token*/,
                            StreamComplete   on_complete)
{
    if (on_complete) {
        on_complete(make_error(ErrorKind::AIError, "no AI backend available"));
    }
}

void AIEngine::cancel_stream()
{
    m_impl->cancel_flag.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
// Backend inspection.
// -----------------------------------------------------------------------------

AIBackend AIEngine::active_backend() const
{
    const std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->preferred;
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
    for (const auto& b : m_impl->backends) {
        if (b && b->is_available()) {
            return b->display_name();
        }
    }
    return k_status_not_configured;
}

} // namespace vectis::services
