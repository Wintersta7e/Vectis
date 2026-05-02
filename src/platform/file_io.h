#pragma once

#include <filesystem>
#include <string>

#include "core/result.h"

/// Cross-platform file-system and executable-path helpers.
///
/// Every function returns `vectis::core::Result<T>` so callers can handle
/// errors explicitly; no exceptions propagate out of this layer.
namespace vectis::platform {

/// Absolute path to the currently-running executable.
///
/// Uses `/proc/self/exe` on Linux, `GetModuleFileNameW` on Windows, and
/// (in the future) `_NSGetExecutablePath` on macOS.
[[nodiscard]] vectis::core::Result<std::filesystem::path> executable_path();

/// Directory containing the currently-running executable. This is the
/// anchor for the portable `vectis-data/` directory and for
/// `vectis.toml`, regardless of the caller's current working directory.
[[nodiscard]] vectis::core::Result<std::filesystem::path> executable_dir();

/// Default data directory, i.e. `<executable_dir>/vectis-data`.
/// This is the canonical location unless a future setting overrides it.
[[nodiscard]] vectis::core::Result<std::filesystem::path> default_data_dir();

/// Create `path` and any missing parents. Success if the directory
/// already exists.
[[nodiscard]] vectis::core::Result<void> ensure_dir(const std::filesystem::path& path);

/// Read the entire contents of a file as a UTF-8 string.
[[nodiscard]] vectis::core::Result<std::string> read_file(const std::filesystem::path& path);

/// Write `contents` to `path`, replacing any existing file. Parent
/// directories must already exist.
[[nodiscard]] vectis::core::Result<void> write_file(const std::filesystem::path& path,
                                                    std::string_view contents);

} // namespace vectis::platform
