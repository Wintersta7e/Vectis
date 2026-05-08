#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "code/code_index.h"
#include "core/result.h"

namespace vectis::code {

/// Which serialization variant the digest exporter should produce.
///
/// Both variants are JSON. Vectis is consumed by agents — slim is the
/// default, full carries excerpts and per-file symbol arrays for the
/// rare cases an agent needs the extra detail.
enum class DigestFormat : std::uint8_t
{
    /// Full structured JSON: project metadata + per-file size/lines +
    /// per-file symbols array + hotspot body excerpts. Pretty-printed,
    /// UTF-8.
    Json,

    /// Token-budget-conscious JSON: omits per-file size, lines, and
    /// the symbols array. Keeps project stats and the flat file list
    /// so it still works as a structure map.
    SlimJson,
};

/// Inputs to a single export call.
struct ExportOptions
{
    DigestFormat format = DigestFormat::Json;
    /// Absolute path to the scanned project root. Required — determines
    /// the default output filename and is written into the digest.
    std::filesystem::path project_root;
    /// Human-readable project name. If empty, derived from
    /// `project_root.filename().string()`.
    std::string project_name;
    /// Forwarded to `detect_architecture` so its disk walk skips the
    /// same directories the scanner did. Empty means "fall back to
    /// the scanner's static defaults" (matches the `detect_architecture`
    /// default arg). CLI populates this from `ScanConfig::exclude_dir_names`
    /// so .gitignore-derived names also apply.
    std::unordered_set<std::string> exclude_dir_names;
};

/// Default output path for a given `format`, sitting directly under
/// `project_root`:
///
///     <project_root>/vectis-digest.json
///     <project_root>/vectis-digest-slim.json
[[nodiscard]] std::filesystem::path default_output_path(const std::filesystem::path& project_root,
                                                        DigestFormat format);

/// Build the digest content as a string without touching the disk.
/// Never fails for valid input — returns an empty string only when the
/// index is empty, but even then produces a well-formed minimal
/// document.
[[nodiscard]] std::string build_digest_string(const CodeIndex& index, const ExportOptions& options);

/// Build the digest and write it to `default_output_path(...)` for
/// the requested format. Overwrites any existing file with an INFO
/// log. Returns the written path on success.
[[nodiscard]] vectis::core::Result<std::filesystem::path>
export_digest(const CodeIndex& index, const ExportOptions& options);

} // namespace vectis::code
