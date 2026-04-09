#pragma once

#include <optional>
#include <string>
#include <string_view>

/// Process-level helpers: environment variable access, etc.
///
/// NOTE: `std::getenv` is thread-unsafe on POSIX because another thread can
/// call `setenv`/`unsetenv` concurrently. Vectis only calls `get_env` during
/// single-threaded startup (before any worker thread is spawned) and when
/// reading API tokens from config — both of which run on the main thread.
namespace vectis::platform {

/// Read an environment variable by name.
/// Returns `std::nullopt` if the variable is unset or empty.
[[nodiscard]] std::optional<std::string> get_env(std::string_view name);

} // namespace vectis::platform
