#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace vectis::code {

class CodeIndex;

/// Coarse architectural pattern label assigned to a project after a
/// directory-structure analysis. Confidence is carried separately so
/// callers can decide whether to trust the result.
enum class ArchitectureLabel : std::uint8_t {
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
};

/// Human-readable description returned by `detect_architecture`.
struct ArchitectureDescription {
    ArchitectureLabel label      = ArchitectureLabel::Unknown;
    /// Short English justification ("found src/controllers/ and
    /// src/services/" or "single main, no layered directories").
    std::string       reasoning;
    /// 0-100 integer confidence. 0 = pure guess, 100 = unambiguous.
    std::uint8_t      confidence = 0;
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
[[nodiscard]] ArchitectureDescription
detect_architecture(const CodeIndex&             index,
                    const std::filesystem::path& project_root);

} // namespace vectis::code
