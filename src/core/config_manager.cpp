#include "core/config_manager.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "core/log.h"
#include "platform/process.h"

namespace vectis::core {

struct ConfigManager::Impl {
    toml::table           table;
    std::filesystem::path source;
    bool                  loaded_from_file = false;
};

namespace {

/// Walk a dot-separated key path down through nested TOML tables.
/// Returns the final node or nullptr if any intermediate segment is
/// missing or is not a table.
[[nodiscard]] const toml::node* walk(const toml::table& root, std::string_view dotted_key)
{
    const toml::table* current = &root;
    std::size_t        start   = 0;

    while (start < dotted_key.size()) {
        const std::size_t dot = dotted_key.find('.', start);
        const std::string_view segment =
            dotted_key.substr(start, dot == std::string_view::npos ? dot : dot - start);

        const toml::node* child = current->get(segment);
        if (child == nullptr) {
            return nullptr;
        }

        if (dot == std::string_view::npos) {
            return child;
        }

        current = child->as_table();
        if (current == nullptr) {
            return nullptr;
        }
        start = dot + 1;
    }
    return nullptr;
}

} // namespace

ConfigManager::ConfigManager() : m_impl(std::make_unique<Impl>()) {}
ConfigManager::~ConfigManager() = default;

ConfigManager::ConfigManager(ConfigManager&&) noexcept            = default;
ConfigManager& ConfigManager::operator=(ConfigManager&&) noexcept = default;

Result<void> ConfigManager::load(const std::filesystem::path& toml_path)
{
    m_impl->source = toml_path;

    std::error_code ec;
    const bool      exists = std::filesystem::exists(toml_path, ec);
    if (ec) {
        return make_error(
            ErrorKind::IoError,
            "failed to stat config file: " + ec.message(),
            toml_path.string());
    }
    if (!exists) {
        VECTIS_LOG_INFO(
            "No vectis.toml at '{}' — using built-in defaults", toml_path.string());
        m_impl->table            = toml::table{};
        m_impl->loaded_from_file = false;
        return {};
    }

    try {
        m_impl->table            = toml::parse_file(toml_path.string());
        m_impl->loaded_from_file = true;
        VECTIS_LOG_INFO("Loaded config from '{}'", toml_path.string());
        return {};
    } catch (const toml::parse_error& e) {
        const auto&       src  = e.source();
        const std::string where = std::to_string(src.begin.line) + ":" +
                                  std::to_string(src.begin.column);
        // toml::parse_error::description() returns std::string_view —
        // convert before concatenating to avoid the string_view + string
        // ambiguity with std::operator+.
        std::string message = "TOML parse error at ";
        message.append(where);
        message.append(": ");
        message.append(e.description());
        return make_error(
            ErrorKind::ConfigError, std::move(message), toml_path.string());
    } catch (const std::exception& e) {
        return make_error(
            ErrorKind::ConfigError,
            std::string{"unexpected error loading config: "} + e.what(),
            toml_path.string());
    }
}

void ConfigManager::reset_to_defaults() noexcept
{
    m_impl->table            = toml::table{};
    m_impl->source.clear();
    m_impl->loaded_from_file = false;
}

std::string ConfigManager::get_string(std::string_view key, std::string_view fallback) const
{
    const toml::node* node = walk(m_impl->table, key);
    if (node == nullptr) {
        return std::string{fallback};
    }
    if (const auto* str = node->as_string()) {
        return str->get();
    }
    return std::string{fallback};
}

std::int64_t ConfigManager::get_int(std::string_view key, std::int64_t fallback) const
{
    const toml::node* node = walk(m_impl->table, key);
    if (node == nullptr) {
        return fallback;
    }
    if (const auto* integer = node->as_integer()) {
        return integer->get();
    }
    return fallback;
}

double ConfigManager::get_double(std::string_view key, double fallback) const
{
    const toml::node* node = walk(m_impl->table, key);
    if (node == nullptr) {
        return fallback;
    }
    if (const auto* flt = node->as_floating_point()) {
        return flt->get();
    }
    if (const auto* integer = node->as_integer()) {
        return static_cast<double>(integer->get());
    }
    return fallback;
}

bool ConfigManager::get_bool(std::string_view key, bool fallback) const
{
    const toml::node* node = walk(m_impl->table, key);
    if (node == nullptr) {
        return fallback;
    }
    if (const auto* boolean = node->as_boolean()) {
        return boolean->get();
    }
    return fallback;
}

std::vector<std::string> ConfigManager::get_string_array(
    std::string_view key, std::vector<std::string> fallback) const
{
    const toml::node* node = walk(m_impl->table, key);
    if (node == nullptr) {
        return fallback;
    }
    const auto* array = node->as_array();
    if (array == nullptr) {
        return fallback;
    }

    std::vector<std::string> result;
    result.reserve(array->size());
    for (const auto& element : *array) {
        if (const auto* str = element.as_string()) {
            result.push_back(str->get());
        } else {
            // Mixed-type array — fall back entirely rather than
            // silently dropping elements.
            return fallback;
        }
    }
    return result;
}

std::optional<std::string> ConfigManager::get_env(std::string_view var_name) const
{
    return vectis::platform::get_env(var_name);
}

bool ConfigManager::loaded_from_file() const noexcept
{
    return m_impl->loaded_from_file;
}

std::filesystem::path ConfigManager::source_path() const
{
    return m_impl->source;
}

} // namespace vectis::core
