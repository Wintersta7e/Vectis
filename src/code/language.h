#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace vectis::code {

/// Source languages Vectis knows how to detect and parse.
///
/// Step 2 ships with twelve built-in grammars wired through CMake:
/// Python, JavaScript (incl. `.jsx`), TypeScript (incl. `.tsx`), C,
/// C++, Rust, Java, C#, Go, Ruby, PHP, and SQL. All other languages
/// fall into `Unknown` and are skipped by the scanner.
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
};

/// Count of real language entries (excludes `Unknown`). Handy for
/// sizing bitmasks and arrays.
inline constexpr std::size_t k_language_count = 12;

/// Detect language from a file's extension.
///
/// Matching is case-insensitive, tolerant of leading dot, and falls
/// through to `Unknown` on miss. The `.h` extension is treated as `C`
/// (not C++) — for header-only C++ files the user can rename or
/// Vectis will still index them under `C`, which is acceptable for
/// Step 2.
[[nodiscard]] Language detect_language(const std::filesystem::path& path) noexcept;

/// Short human-readable name for a language, e.g. "TypeScript".
[[nodiscard]] std::string_view language_name(Language language) noexcept;

/// Reverse of `language_name`: parses a string back to a Language.
/// Returns `Language::Unknown` if the name is not recognized.
[[nodiscard]] Language language_from_name(std::string_view name) noexcept;

} // namespace vectis::code
