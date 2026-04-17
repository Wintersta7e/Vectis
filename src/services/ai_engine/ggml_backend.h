#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#include "core/result.h"
#include "services/ai_engine/backend.h"

namespace vectis::services {

/// Offline GGML/llama.cpp backend. Always present as a class; whether
/// it is functional depends on the `VECTIS_BUILD_GGML` CMake option:
///
///   - OFF (default): every call fails with "GGML not compiled in this
///     build". `is_available()` returns false. No llama.cpp linkage.
///
///   - ON: on construction, attempts to load a GGUF model from the
///     path passed in. If the model loads, `is_available()` returns
///     true and `generate()` runs CPU inference via llama.cpp.
class GGMLBackend final : public IBackend {
public:
    /// `model_path` is the absolute or relative path to a GGUF file.
    /// Empty path means "no model configured" — the backend stays
    /// unavailable.
    explicit GGMLBackend(std::filesystem::path model_path = {});
    ~GGMLBackend() override;

    [[nodiscard]] AIBackend   kind()         const override { return AIBackend::GGML; }
    [[nodiscard]] bool        is_available() const override;
    [[nodiscard]] std::string display_name() const override;

    [[nodiscard]] vectis::core::Result<AIResponse>
    generate(const AIRequest& request) override;

    void generate_stream(const AIRequest&   request,
                         StreamCallback     on_token,
                         std::atomic<bool>& cancel_flag,
                         StreamComplete     on_complete) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::services
