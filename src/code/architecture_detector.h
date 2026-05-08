#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "code/exclude_dirs.h"

namespace vectis::code {

class CodeIndex;

/// Coarse architectural pattern label assigned to a project after a
/// directory-structure analysis. Confidence is carried separately so
/// callers can decide whether to trust the result.
enum class ArchitectureLabel : std::uint8_t
{
    Unknown = 0,
    Monolith,
    Layered,
    Mvc,
    Monorepo,
    FrontendSpa,
    ApiBackend,
    /// View + ViewModel + (often) Models directories. Typical of WPF,
    /// Avalonia, MAUI, UWP, SwiftUI-with-MVVM projects.
    Mvvm,
    /// Domain + Application + Infrastructure (+ Presentation) layering.
    /// The "onion" / "hexagonal" family popular in larger .NET and
    /// enterprise Java codebases.
    CleanArchitecture,
    /// A .NET-style multi-project solution: several sibling directories
    /// whose leaf name matches `<Solution>.<Tag>` (e.g. `FlowForge.UI`,
    /// `FlowForge.CLI`, `FlowForge.Core`).
    DotNetSolution,
    /// Reusable library: a public `include/` (or `lib/`) interface
    /// alongside `src/`, with no application entry point. Typical of
    /// C++ libraries (fmt, spdlog, nlohmann/json with src split out)
    /// and many language-agnostic SDKs.
    Library,
};

/// Description returned by `detect_architecture`.
///
/// The struct serves two consumers:
///   * **Agents (JSON digest)** read `label`, `confidence`, and `signals` —
///     all structured fields. `signals` is a flat list of short tokens
///     (e.g. `"workspace:cargo"`, `"dir:controllers"`, `"runtime:Java"`)
///     that explain *which* heuristics fired. Lowercase, hyphen-separated,
///     `category:value` shape — agent-greppable, not prose.
///   * **`vectis explain` (humans)** reads `reasoning` — a short English
///     justification. Not emitted in JSON.
struct ArchitectureDescription
{
    ArchitectureLabel label = ArchitectureLabel::Unknown;
    /// Short English justification ("found src/controllers/ and
    /// src/services/" or "single main, no layered directories").
    /// Human-only; consumed by `vectis explain`. Not emitted in JSON.
    std::string reasoning;
    /// Flat list of `category:value` tokens describing the heuristic
    /// matches that produced `label`. Empty for `Unknown` and the
    /// "no source files" case. Emitted in JSON for agents.
    std::vector<std::string> signals;
    /// 0-100 integer confidence. 0 = pure guess, 100 = unambiguous.
    std::uint8_t confidence = 0;
};

/// Convert a label to its short display name.
[[nodiscard]] std::string_view architecture_label_name(ArchitectureLabel label) noexcept;

/// Infer a coarse architectural pattern from the files in `index`
/// and their locations relative to `project_root`.
///
/// This is a pure heuristic: it looks at top-level and one-level
/// nested directory names for tell-tale signs and counts the
/// matches. No runtime code execution, no framework-specific
/// detection. Deliberately conservative — falls back to Monolith
/// (for small projects) or Unknown (for projects with no recognized
/// indicators) rather than guessing wildly.
///
/// `exclude_dir_names` controls which directory names the supplemental
/// disk walk treats as off-limits. Defaults to the scanner's canonical
/// list so tests don't have to thread it explicitly. Production callers
/// should pass `ScanConfig::exclude_dir_names` so .gitignore-derived
/// names also apply.
///
/// An **empty** set is treated as "use the canonical default" so a
/// default-constructed `ExportOptions{}` still produces a sensible
/// disk walk. This is intentional — callers that genuinely want zero
/// excludes (extremely rare) should pass a single placeholder name
/// they know won't match anything in the project.
[[nodiscard]] ArchitectureDescription detect_architecture(
    const CodeIndex& index, const std::filesystem::path& project_root,
    const std::unordered_set<std::string>& exclude_dir_names = default_scanner_exclude_dir_names());

} // namespace vectis::code
