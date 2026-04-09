#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace vectis::core {

/// TOML-backed configuration store for Vectis.
///
/// Semantics:
/// - Every getter takes a fallback and is infallible.
/// - Keys are dot-separated paths into nested TOML tables
///   (e.g. `"code.digest.max_depth"`).
/// - `load()` treats a missing file as success (soft fallback to defaults)
///   but reports a `Result` error on a present-but-malformed file.
/// - API tokens are **never** read from the file. Modes read them from the
///   environment via `get_env()`.
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    ConfigManager(const ConfigManager&)            = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) noexcept;
    ConfigManager& operator=(ConfigManager&&) noexcept;

    /// Load `vectis.toml` from `toml_path`.
    ///
    /// - Missing file: returns success, logs an INFO message, state is left
    ///   at built-in defaults, `loaded_from_file()` stays `false`.
    /// - Parse error on an existing file: returns
    ///   `Result` error with `ErrorKind::ConfigError`.
    /// - Success: `loaded_from_file()` becomes `true`.
    [[nodiscard]] Result<void> load(const std::filesystem::path& toml_path);

    /// Drop any loaded file and reset to built-in defaults.
    void reset_to_defaults() noexcept;

    // Typed getters — dot-separated keys walk into nested tables.
    [[nodiscard]] std::string get_string(std::string_view key, std::string_view fallback) const;
    [[nodiscard]] std::int64_t get_int(std::string_view key, std::int64_t fallback) const;
    [[nodiscard]] double get_double(std::string_view key, double fallback) const;
    [[nodiscard]] bool get_bool(std::string_view key, bool fallback) const;
    [[nodiscard]] std::vector<std::string> get_string_array(
        std::string_view key, std::vector<std::string> fallback) const;

    /// Look up an environment variable. This is a thin pass-through to
    /// `vectis::platform::get_env` and is exposed here so modes have a
    /// single dependency for "configuration values".
    [[nodiscard]] std::optional<std::string> get_env(std::string_view var_name) const;

    /// `true` if `load()` successfully read a file (not defaults).
    [[nodiscard]] bool loaded_from_file() const noexcept;

    /// Path of the file last passed to `load()`, or empty if none.
    [[nodiscard]] std::filesystem::path source_path() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::core
