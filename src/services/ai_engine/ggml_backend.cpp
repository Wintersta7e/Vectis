#include "services/ai_engine/ggml_backend.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <optional>

#include "core/log.h"
#include "core/result.h"

#ifdef VECTIS_GGML_ENABLED
    // Real llama.cpp integration. Held behind the flag so apt-based
    // dev builds don't need to fetch ~hundreds of MB of ggml sources.
    #include "llama.h"
#endif

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

namespace {

constexpr const char* k_not_compiled_message =
    "GGML not compiled in this build (rebuild with -DVECTIS_BUILD_GGML=ON)";

#ifdef VECTIS_GGML_ENABLED
/// RAII guard for the llama.cpp global backend. `llama_backend_init()`
/// must be paired with `llama_backend_free()` regardless of whether
/// the subsequent model load succeeded — previously we guarded the
/// free with `model_loaded`, which leaked the global backend on
/// partial-load failures.
class LlamaBackendGuard {
public:
    LlamaBackendGuard() { ::llama_backend_init(); }
    ~LlamaBackendGuard() { ::llama_backend_free(); }

    LlamaBackendGuard(const LlamaBackendGuard&)            = delete;
    LlamaBackendGuard& operator=(const LlamaBackendGuard&) = delete;
    LlamaBackendGuard(LlamaBackendGuard&&)                 = delete;
    LlamaBackendGuard& operator=(LlamaBackendGuard&&)      = delete;
};
#endif

} // namespace

// =============================================================================
// Impl
// =============================================================================

struct GGMLBackend::Impl {
    std::filesystem::path model_path;
    std::string           model_name;   // last path segment for display
    bool                  model_loaded = false;

#ifdef VECTIS_GGML_ENABLED
    // Declaration order matters: `backend_guard` is initialised first
    // and destroyed LAST, so model/ctx cleanup happens before the
    // global backend is torn down.
    std::optional<LlamaBackendGuard> backend_guard;
    ::llama_model*   model = nullptr;
    ::llama_context* ctx   = nullptr;
#endif
};

// =============================================================================
// Lifecycle
// =============================================================================

GGMLBackend::GGMLBackend(std::filesystem::path model_path)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->model_path = std::move(model_path);
    if (!m_impl->model_path.empty()) {
        m_impl->model_name = m_impl->model_path.filename().string();
    }

#ifdef VECTIS_GGML_ENABLED
    if (m_impl->model_path.empty()) {
        VECTIS_LOG_INFO("GGMLBackend: no model_path configured; staying unavailable");
        return;
    }
    if (!std::filesystem::exists(m_impl->model_path)) {
        VECTIS_LOG_WARN("GGMLBackend: model file not found: {}",
                        m_impl->model_path.string());
        return;
    }

    // Activate the global llama backend via RAII — pairs with free in
    // the destructor regardless of whether the model / context load
    // below succeeds.
    m_impl->backend_guard.emplace();

    ::llama_model_params mparams = ::llama_model_default_params();
    mparams.n_gpu_layers = 0;   // CPU-only by policy.

    m_impl->model = ::llama_model_load_from_file(
        m_impl->model_path.string().c_str(), mparams);
    if (m_impl->model == nullptr) {
        VECTIS_LOG_ERROR("GGMLBackend: llama_model_load_from_file failed");
        return;
    }

    ::llama_context_params cparams = ::llama_context_default_params();
    cparams.n_ctx = 4096;
    m_impl->ctx = ::llama_new_context_with_model(m_impl->model, cparams);
    if (m_impl->ctx == nullptr) {
        VECTIS_LOG_ERROR("GGMLBackend: llama_new_context_with_model failed");
        ::llama_free_model(m_impl->model);
        m_impl->model = nullptr;
        return;
    }

    m_impl->model_loaded = true;
    VECTIS_LOG_INFO("GGMLBackend: loaded {}", m_impl->model_name);
#else
    VECTIS_LOG_INFO("GGMLBackend: {} (constructor no-op)", k_not_compiled_message);
#endif
}

GGMLBackend::~GGMLBackend()
{
#ifdef VECTIS_GGML_ENABLED
    if (m_impl->ctx   != nullptr) ::llama_free(m_impl->ctx);
    if (m_impl->model != nullptr) ::llama_free_model(m_impl->model);
    // backend_guard's destructor runs last and calls llama_backend_free()
    // unconditionally when activated, fixing the partial-load leak
    // (previously this was guarded by model_loaded, so a failed
    // llama_new_context_with_model leaked the global backend).
#endif
}

// =============================================================================
// Availability / display
// =============================================================================

bool GGMLBackend::is_available() const
{
    return m_impl->model_loaded;
}

std::string GGMLBackend::display_name() const
{
#ifdef VECTIS_GGML_ENABLED
    if (m_impl->model_loaded) {
        return "GGML: " + m_impl->model_name;
    }
    return "GGML (model not loaded)";
#else
    return "GGML (not compiled in)";
#endif
}

// =============================================================================
// Generate
// =============================================================================

Result<AIResponse> GGMLBackend::generate(const AIRequest& /*request*/)
{
#ifdef VECTIS_GGML_ENABLED
    if (!m_impl->model_loaded) {
        return make_error(ErrorKind::AIError, "GGML model not loaded");
    }
    // Real token-by-token sampling loop is deferred to Step 13 — at
    // that point we will validate against an actual GGUF file. For
    // now the flag-on path links llama.cpp and loads the model, but
    // inference returns a stub message so the flag can be flipped on
    // in CI without a model file.
    return make_error(ErrorKind::AIError,
                      "GGML inference not yet implemented "
                      "(scheduled for Step 13 polish)");
#else
    return make_error(ErrorKind::AIError, k_not_compiled_message);
#endif
}

void GGMLBackend::generate_stream(const AIRequest&   request,
                                  StreamCallback     /*on_token*/,
                                  std::atomic<bool>& /*cancel_flag*/,
                                  StreamComplete     on_complete)
{
    // Streaming shares the same compile-time gating. Since the real
    // inference path isn't wired yet, fall through to sync and report
    // the same error via on_complete.
    auto r = generate(request);
    if (on_complete) on_complete(std::move(r));
}

} // namespace vectis::services
