#include "modes/code/language.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace vectis::modes::code {

namespace {

struct ExtensionEntry {
    std::string_view extension;  // with leading dot, lowercase
    Language         language;
};

/// File-extension → language map. Kept small and deliberately explicit
/// so it's easy to review and easy to extend without regex pitfalls.
constexpr std::array<ExtensionEntry, 27> k_extension_map = {{
    {".py",    Language::Python},
    {".pyi",   Language::Python},
    {".js",    Language::JavaScript},
    {".jsx",   Language::JavaScript},
    {".mjs",   Language::JavaScript},
    {".cjs",   Language::JavaScript},
    {".ts",    Language::TypeScript},
    {".tsx",   Language::TypeScript},
    {".c",     Language::C},
    // `.h` is treated as C++ in mixed codebases because pure-C projects
    // are rare today and C++ headers are by far the common case.
    // Projects with `.h` files intended as C can still work — the C++
    // grammar is a (nearly complete) superset of C syntax.
    {".h",     Language::Cpp},
    {".cpp",   Language::Cpp},
    {".cxx",   Language::Cpp},
    {".cc",    Language::Cpp},
    {".hpp",   Language::Cpp},
    {".hh",    Language::Cpp},
    {".hxx",   Language::Cpp},
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

Language language_from_name(std::string_view name) noexcept
{
    if (name == "Python")     return Language::Python;
    if (name == "JavaScript") return Language::JavaScript;
    if (name == "TypeScript") return Language::TypeScript;
    if (name == "C")          return Language::C;
    if (name == "C++")        return Language::Cpp;
    if (name == "Rust")       return Language::Rust;
    if (name == "Java")       return Language::Java;
    if (name == "C#")         return Language::CSharp;
    if (name == "Go")         return Language::Go;
    if (name == "Ruby")       return Language::Ruby;
    if (name == "PHP")        return Language::Php;
    if (name == "SQL")        return Language::Sql;
    return Language::Unknown;
}

} // namespace vectis::modes::code
