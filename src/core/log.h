#pragma once

#include <filesystem>

#include <spdlog/spdlog.h>

#include "core/result.h"

/// Vectis logging wrapper over spdlog.
///
/// Provides a single named logger ("vectis") backed by a rotating file sink
/// in `<data_dir>/logs/vectis.log` plus a stderr sink. Level is controlled
/// via the `VECTIS_LOG_LEVEL` environment variable (trace|debug|info|warn|
/// error|critical|off), defaulting to `info`.
///
/// Use the `VECTIS_LOG_*` macros instead of calling spdlog directly so the
/// underlying logger can be swapped without touching every call site.
namespace vectis::core::log {

/// Initialize the global logger. Must be called once at App startup,
/// before any `VECTIS_LOG_*` macro invocation.
///
/// Creates `<data_dir>/logs/` if it does not exist. Returns a
/// `ConfigError` / `IoError` on failure — the caller should surface this
/// and refuse to continue, since a broken logger makes every later
/// failure invisible.
[[nodiscard]] Result<void> init(const std::filesystem::path& data_dir);

/// Flush and drop the global logger. Safe to call multiple times and
/// safe to call without a matching successful `init()`.
void shutdown() noexcept;

/// Access the shared logger. Only meant for the `VECTIS_LOG_*` macros;
/// prefer those in user code. Returns `nullptr` before `init()` succeeds
/// or after `shutdown()`.
[[nodiscard]] spdlog::logger* get() noexcept;

} // namespace vectis::core::log

// -----------------------------------------------------------------------------
// Logging macros
// -----------------------------------------------------------------------------
//
// The macros check for a null logger so calls made before `log::init()`
// or after `log::shutdown()` are silently dropped instead of crashing.
// This matters during early startup (e.g., if `init()` itself fails) and
// during shutdown teardown.

#define VECTIS_LOG_IMPL(level, ...)                                                                \
    do {                                                                                           \
        if (auto* vectis_logger_ = ::vectis::core::log::get()) {                                   \
            vectis_logger_->log(level, __VA_ARGS__);                                               \
        }                                                                                          \
    } while (false)

#define VECTIS_LOG_TRACE(...) VECTIS_LOG_IMPL(::spdlog::level::trace, __VA_ARGS__)
#define VECTIS_LOG_DEBUG(...) VECTIS_LOG_IMPL(::spdlog::level::debug, __VA_ARGS__)
#define VECTIS_LOG_INFO(...) VECTIS_LOG_IMPL(::spdlog::level::info, __VA_ARGS__)
#define VECTIS_LOG_WARN(...) VECTIS_LOG_IMPL(::spdlog::level::warn, __VA_ARGS__)
#define VECTIS_LOG_ERROR(...) VECTIS_LOG_IMPL(::spdlog::level::err, __VA_ARGS__)
