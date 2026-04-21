#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/result.h"
#include "code/code_index.h"

namespace vectis::code {

/// Which serialization variant the digest exporter should produce.
enum class DigestFormat : std::uint8_t {
    /// Full structured JSON: project metadata + per-file size/lines +
    /// per-file symbols array. Pretty-printed, UTF-8.
    Json,

    /// Human-readable Markdown outline: H1 project title, overview
    /// stats, per-file H3 sections with bullet-list symbols.
    Markdown,

    /// Token-budget-conscious JSON: omits per-file size, lines, and
    /// the symbols array. Keeps project stats and the flat file list
    /// so it still works as a structure map.
    SlimJson,
};

/// Inputs to a single export call.
struct ExportOptions {
    DigestFormat          format = DigestFormat::Json;
    /// Absolute path to the scanned project root. Required — determines
    /// the default output filename and is written into the digest.
    std::filesystem::path project_root;
    /// Human-readable project name. If empty, derived from
    /// `project_root.filename().string()`.
    std::string           project_name;
};

/// Default output path for a given `format`, sitting directly under
/// `project_root`:
///
///     <project_root>/vectis-digest.json
///     <project_root>/vectis-digest.md
///     <project_root>/vectis-digest-slim.json
[[nodiscard]] std::filesystem::path
default_output_path(const std::filesystem::path& project_root, DigestFormat format);

/// Build the digest content as a string without touching the disk.
/// Never fails for valid input — returns an empty string only when the
/// index is empty, but even then produces a well-formed minimal
/// document.
[[nodiscard]] std::string
build_digest_string(const CodeIndex& index, const ExportOptions& options);

/// Build the digest and write it to `default_output_path(...)` for
/// the requested format. Overwrites any existing file with an INFO
/// log. Returns the written path on success.
[[nodiscard]] vectis::core::Result<std::filesystem::path>
export_digest(const CodeIndex& index, const ExportOptions& options);

} // namespace vectis::code
