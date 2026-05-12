#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace vectis::code {

/// Source languages and manifest formats Vectis knows about.
///
/// The first twelve entries are real source languages parsed by
/// tree-sitter: Python, JavaScript (incl. `.jsx`), TypeScript (incl.
/// `.tsx`), C, C++, Rust, Java, C#, Go, Ruby, PHP, and SQL. The last
/// six are manifest / project-file formats registered by the manifest
/// scanner (Phase 1-4 of ISSUE-07) — not parsed via tree-sitter but
/// still tagged on `FileEntry` so digest consumers can tell which
/// language family a file belongs to. All other extensions fall into
/// `Unknown` and are skipped by the source scanner.
enum class Language : std::uint8_t
{
    Unknown = 0,
    Python,
    JavaScript,
    TypeScript,
    C,
    Cpp,
    Rust,
    Java,
    CSharp,
    Go,
    Ruby,
    Php,
    Sql,
    MavenPom,
    Csproj,
    DotNetSolution,
    SpringXml,
    Properties,
    MsbuildProps,
};

/// Count of real entries (excludes `Unknown`). Handy for sizing bitmasks
/// and arrays. MUST stay in sync with the `Language` enum.
inline constexpr std::size_t k_language_count = 18;

/// Detect language from a file's extension.
///
/// Matching is case-insensitive, tolerant of leading dot, and falls
/// through to `Unknown` on miss. The `.h` extension is treated as `C`
/// (not C++) — for header-only C++ files the user can rename or
/// Vectis will still index them under `C`, which is acceptable for
/// Step 2.
[[nodiscard]] Language detect_language(const std::filesystem::path& path) noexcept;

/// Refine a tentative language classification by sniffing file content.
/// Currently only acts on `.h` files: if the header contains no C/C++
/// preprocessor or declaration markers but does contain JavaScript
/// markers (legacy "JS aliases" includes pulled in by HTML help
/// systems), reclassify as JavaScript so the grammar matches.
/// Cheap path-side pre-check ensures non-`.h` files do no string work.
[[nodiscard]] Language refine_language(Language tentative, const std::filesystem::path& path,
                                       std::string_view content) noexcept;

/// Short human-readable name for a language, e.g. "TypeScript".
[[nodiscard]] std::string_view language_name(Language language) noexcept;

/// Reverse of `language_name`: parses a string back to a Language.
/// Returns `Language::Unknown` if the name is not recognized.
[[nodiscard]] Language language_from_name(std::string_view name) noexcept;

} // namespace vectis::code
