#include "modes/code/language.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

namespace vectis::modes::code {

namespace {

struct ExtensionEntry {
    std::string_view extension;  // with leading dot, lowercase
    Language         language;
};

/// File-extension → language map. Kept small and deliberately explicit
/// so it's easy to review and easy to extend without regex pitfalls.
constexpr std::array<ExtensionEntry, 25> k_extension_map = {{
    {".py",    Language::Python},
    {".pyi",   Language::Python},
    {".js",    Language::JavaScript},
    {".jsx",   Language::JavaScript},
    {".mjs",   Language::JavaScript},
    {".cjs",   Language::JavaScript},
    {".ts",    Language::TypeScript},
    {".tsx",   Language::TypeScript},
    {".c",     Language::C},
    {".h",     Language::C},
    {".cpp",   Language::Cpp},
    {".cxx",   Language::Cpp},
    {".cc",    Language::Cpp},
    {".hpp",   Language::Cpp},
    {".rs",    Language::Rust},
    {".java",  Language::Java},
    {".cs",    Language::CSharp},
    {".go",    Language::Go},
    {".rb",    Language::Ruby},
    {".php",   Language::Php},
    {".phtml", Language::Php},
    {".sql",   Language::Sql},
    {".ddl",   Language::Sql},
    {".pks",   Language::Sql},   // Oracle PL/SQL package spec
    {".pkb",   Language::Sql},   // Oracle PL/SQL package body
}};

/// Lowercase an extension string in-place for case-insensitive lookup.
/// ASCII-only is fine — file extensions that matter are all ASCII.
[[nodiscard]] std::string to_lower_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

} // namespace

Language detect_language(const std::filesystem::path& path) noexcept
{
    try {
        const std::string ext = to_lower_ascii(path.extension().string());
        if (ext.empty()) {
            return Language::Unknown;
        }
        for (const auto& entry : k_extension_map) {
            if (entry.extension == ext) {
                return entry.language;
            }
        }
    } catch (...) {
        // Defensive: any filesystem / allocation hiccup falls through
        // to Unknown. noexcept boundary protects call sites.
    }
    return Language::Unknown;
}

std::string_view language_name(Language language) noexcept
{
    switch (language) {
        case Language::Python:     return "Python";
        case Language::JavaScript: return "JavaScript";
        case Language::TypeScript: return "TypeScript";
        case Language::C:          return "C";
        case Language::Cpp:        return "C++";
        case Language::Rust:       return "Rust";
        case Language::Java:       return "Java";
        case Language::CSharp:     return "C#";
        case Language::Go:         return "Go";
        case Language::Ruby:       return "Ruby";
        case Language::Php:        return "PHP";
        case Language::Sql:        return "SQL";
        case Language::Unknown:    return "Unknown";
    }
    return "Unknown";
}

ImVec4 language_color(Language language) noexcept
{
    // Palette borrowed from common IDE/GitHub language colors.
    switch (language) {
        case Language::Python:     return ImVec4(0.22F, 0.52F, 0.78F, 1.0F); // blue
        case Language::JavaScript: return ImVec4(0.94F, 0.86F, 0.21F, 1.0F); // yellow
        case Language::TypeScript: return ImVec4(0.19F, 0.48F, 0.78F, 1.0F); // darker blue
        case Language::C:          return ImVec4(0.33F, 0.55F, 0.67F, 1.0F); // steel
        case Language::Cpp:        return ImVec4(0.94F, 0.33F, 0.42F, 1.0F); // red-pink
        case Language::Rust:       return ImVec4(0.87F, 0.44F, 0.18F, 1.0F); // rust orange
        case Language::Java:       return ImVec4(0.83F, 0.35F, 0.14F, 1.0F); // dark orange
        case Language::CSharp:     return ImVec4(0.41F, 0.19F, 0.63F, 1.0F); // purple
        case Language::Go:         return ImVec4(0.00F, 0.68F, 0.83F, 1.0F); // cyan
        case Language::Ruby:       return ImVec4(0.80F, 0.16F, 0.16F, 1.0F); // crimson
        case Language::Php:        return ImVec4(0.30F, 0.35F, 0.62F, 1.0F); // indigo
        case Language::Sql:        return ImVec4(0.56F, 0.43F, 0.20F, 1.0F); // brown
        case Language::Unknown:    return ImVec4(0.55F, 0.58F, 0.62F, 1.0F); // gray
    }
    return ImVec4(0.55F, 0.58F, 0.62F, 1.0F);
}

} // namespace vectis::modes::code
