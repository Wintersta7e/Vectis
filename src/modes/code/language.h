#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include <imgui.h>

namespace vectis::modes::code {

/// Source languages Vectis knows how to detect and parse.
///
/// Step 2 supports the seven grammars wired through CMake: Python,
/// JavaScript (incl. `.jsx`), TypeScript (incl. `.tsx`), C, C++, Rust,
/// and Java. All other languages fall into `Unknown` and are skipped
/// by the scanner.
enum class Language : std::uint8_t {
    Unknown = 0,
    Python,
    JavaScript,
    TypeScript,
    C,
    Cpp,
    Rust,
    Java,
};

/// Count of real language entries (excludes `Unknown`). Handy for
/// sizing bitmasks and arrays.
inline constexpr std::size_t k_language_count = 7;

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

/// UI accent color for the language, used by the file tree to tint
/// filenames or draw a small colored dot next to the entry.
[[nodiscard]] ImVec4 language_color(Language language) noexcept;

} // namespace vectis::modes::code
