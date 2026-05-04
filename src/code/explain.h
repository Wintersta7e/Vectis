#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace vectis::code {

class CodeIndex;

/// Inputs to a single `build_explanation` call. Mirrors `ExportOptions`
/// but only carries what the explanation actually consumes.
struct ExplainOptions
{
    std::filesystem::path project_root;
    /// Optional project name; when empty, derived from `project_root`'s
    /// last path component.
    std::string project_name;
    /// Forwarded to `detect_architecture` so the disk walk skips the
    /// same directories the scanner did. Empty falls back to defaults.
    std::unordered_set<std::string> exclude_dir_names;
};

/// Build a short, narrative, plain-text summary of a project from
/// its in-memory `CodeIndex`. The output is meant to be ~15–30 lines
/// and consumable directly by a human or an LLM agent — no JSON, no
/// re-parsing required.
///
/// Sections (each elided if empty):
///   - Header: `<name> — <architecture label>`
///   - Architecture line with confidence and reasoning
///   - Scale: file count, symbol count, language mix
///   - API surface: public/private/internal split
///   - Top hotspots by cyclomatic complexity
///   - External dependencies (top 5 by import count)
///   - Internal-graph stats (edges, cycles)
[[nodiscard]] std::string build_explanation(const CodeIndex& index, const ExplainOptions& options);

} // namespace vectis::code
