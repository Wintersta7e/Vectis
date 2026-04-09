#include "modes/code/digest_exporter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/result.h"
#include "modes/code/code_index.h"
#include "modes/code/language.h"
#include "modes/code/symbol.h"
#include "platform/file_io.h"

namespace vectis::modes::code {

namespace {

constexpr const char* k_vectis_version = "0.1.0";

/// Current UTC timestamp formatted as RFC 3339, e.g. "2026-04-09T12:34:56Z".
///
/// Uses `std::gmtime` + `strftime` rather than `std::format` with
/// chrono types — libstdc++'s chrono-format support is still patchy
/// across the GCC versions Vectis targets.
[[nodiscard]] std::string current_utc_rfc3339()
{
    const auto now  = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    char buffer[32];
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — we pass a thread-local tm
    const std::size_t written =
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string{buffer, written};
}

/// Sorted unique list of language display names for every file in
/// the index. `Unknown` is excluded.
[[nodiscard]] std::vector<std::string> distinct_language_names(
    const std::vector<FileEntry>& files)
{
    std::set<std::string> names;
    for (const FileEntry& file : files) {
        if (file.language == Language::Unknown) {
            continue;
        }
        names.emplace(language_name(file.language));
    }
    return {names.begin(), names.end()};
}

/// Derive the effective project name: explicit option takes priority,
/// otherwise fall back to the project root's filename, and if that
/// too is empty, use the literal "project".
[[nodiscard]] std::string effective_project_name(const ExportOptions& options)
{
    if (!options.project_name.empty()) {
        return options.project_name;
    }
    const std::string derived = options.project_root.filename().string();
    if (!derived.empty()) {
        return derived;
    }
    return "project";
}

/// Serialize one file entry to JSON. `include_details` controls
/// whether per-file `size`, `lines`, and `symbols` are written — the
/// slim format skips them.
[[nodiscard]] nlohmann::json file_to_json(
    const FileEntry&     file,
    const CodeIndex&     index,
    bool                 include_details)
{
    nlohmann::json node = {
        // `generic_string()` forces forward slashes on Windows so the
        // digest stays cross-platform portable.
        {"path",     file.path_relative.generic_string()},
        {"language", std::string{language_name(file.language)}},
    };

    if (!include_details) {
        return node;
    }

    node["size"]  = file.size;
    node["lines"] = file.line_count;

    nlohmann::json symbols_array = nlohmann::json::array();
    for (const Symbol& sym : index.symbols_in_file(file.id)) {
        nlohmann::json symbol_node = {
            {"name", sym.name},
            {"kind", std::string{symbol_kind_name(sym.kind)}},
            {"line", sym.line_start},
        };
        if (!sym.signature.empty()) {
            symbol_node["signature"] = sym.signature;
        }
        symbols_array.push_back(std::move(symbol_node));
    }
    node["symbols"] = std::move(symbols_array);

    return node;
}

/// Build the shared top-level JSON object that full and slim formats
/// both start from.
[[nodiscard]] nlohmann::json build_json(
    const CodeIndex&      index,
    const ExportOptions&  options,
    bool                  include_file_details)
{
    const std::vector<FileEntry> files = index.snapshot_files();

    nlohmann::json root;
    root["vectis_version"] = k_vectis_version;
    root["generated_at"]   = current_utc_rfc3339();

    nlohmann::json project;
    project["name"]         = effective_project_name(options);
    project["root"]         = options.project_root.generic_string();
    project["file_count"]   = files.size();
    project["symbol_count"] = index.symbol_count();
    project["languages"]    = distinct_language_names(files);
    root["project"]         = std::move(project);

    nlohmann::json files_array = nlohmann::json::array();
    for (const FileEntry& file : files) {
        files_array.push_back(file_to_json(file, index, include_file_details));
    }
    root["files"] = std::move(files_array);

    return root;
}

/// Build the Markdown digest as a single string.
[[nodiscard]] std::string build_markdown(
    const CodeIndex&      index,
    const ExportOptions&  options)
{
    const std::vector<FileEntry>   files     = index.snapshot_files();
    const std::vector<std::string> langs     = distinct_language_names(files);
    const std::string              proj_name = effective_project_name(options);
    const std::string              timestamp = current_utc_rfc3339();

    std::ostringstream out;
    out << "# " << proj_name << "\n\n";
    out << "_Generated " << timestamp << " by Vectis " << k_vectis_version << "_\n\n";

    out << "## Overview\n\n";
    out << "- Root: `" << options.project_root.generic_string() << "`\n";
    out << "- Files: " << files.size() << "\n";
    out << "- Symbols: " << index.symbol_count() << "\n";
    out << "- Languages: ";
    for (std::size_t i = 0; i < langs.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << langs[i];
    }
    out << "\n\n";

    out << "## Files\n\n";
    if (files.empty()) {
        out << "_(no source files found)_\n";
        return out.str();
    }

    for (const FileEntry& file : files) {
        out << "### " << file.path_relative.generic_string()
            << "  _(" << language_name(file.language) << ")_\n\n";

        const std::vector<Symbol> symbols = index.symbols_in_file(file.id);
        if (symbols.empty()) {
            out << "_(no symbols extracted)_\n\n";
            continue;
        }
        for (const Symbol& sym : symbols) {
            out << "- `" << sym.name << "` ("
                << symbol_kind_name(sym.kind) << ") — line " << sym.line_start << "\n";
        }
        out << "\n";
    }

    return out.str();
}

} // namespace

std::filesystem::path default_output_path(
    const std::filesystem::path& project_root, DigestFormat format)
{
    switch (format) {
        case DigestFormat::Json:     return project_root / "vectis-digest.json";
        case DigestFormat::Markdown: return project_root / "vectis-digest.md";
        case DigestFormat::SlimJson: return project_root / "vectis-digest-slim.json";
    }
    return project_root / "vectis-digest.json";
}

std::string build_digest_string(const CodeIndex& index, const ExportOptions& options)
{
    switch (options.format) {
        case DigestFormat::Json: {
            const nlohmann::json root = build_json(index, options, /*details=*/true);
            return root.dump(2);
        }
        case DigestFormat::SlimJson: {
            const nlohmann::json root = build_json(index, options, /*details=*/false);
            return root.dump(2);
        }
        case DigestFormat::Markdown: {
            return build_markdown(index, options);
        }
    }
    return {};
}

vectis::core::Result<std::filesystem::path>
export_digest(const CodeIndex& index, const ExportOptions& options)
{
    if (options.project_root.empty()) {
        return vectis::core::make_error(
            vectis::core::ErrorKind::ConfigError,
            "export_digest called with empty project_root",
            "digest_exporter");
    }

    const std::filesystem::path out_path =
        default_output_path(options.project_root, options.format);

    std::error_code ec;
    if (std::filesystem::exists(out_path, ec)) {
        VECTIS_LOG_INFO("Overwriting existing digest at '{}'", out_path.string());
    }

    std::string content;
    try {
        content = build_digest_string(index, options);
    } catch (const std::exception& e) {
        return vectis::core::make_error(
            vectis::core::ErrorKind::ParseError,
            std::string{"failed to serialize digest: "} + e.what(),
            out_path.string());
    }

    if (auto r = vectis::platform::write_file(out_path, content); !r) {
        return vectis::core::make_error(
            vectis::core::ErrorKind::IoError,
            std::string{"failed to write digest: "} + r.error().message,
            out_path.string());
    }

    VECTIS_LOG_INFO(
        "Digest exported: {} ({} bytes)", out_path.string(), content.size());
    return out_path;
}

} // namespace vectis::modes::code
