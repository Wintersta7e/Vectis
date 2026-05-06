#include "code/language.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace vectis::code {

namespace {

struct ExtensionEntry
{
    std::string_view extension; // with leading dot, lowercase
    Language language;
};

/// File-extension → language map. Kept small and deliberately explicit
/// so it's easy to review and easy to extend without regex pitfalls.
constexpr std::array<ExtensionEntry, 27> k_extension_map = {{
    {".py", Language::Python},
    {".pyi", Language::Python},
    {".js", Language::JavaScript},
    {".jsx", Language::JavaScript},
    {".mjs", Language::JavaScript},
    {".cjs", Language::JavaScript},
    {".ts", Language::TypeScript},
    {".tsx", Language::TypeScript},
    {".c", Language::C},
    // `.h` is treated as C++ in mixed codebases because pure-C projects
    // are rare today and C++ headers are by far the common case.
    // Projects with `.h` files intended as C can still work — the C++
    // grammar is a (nearly complete) superset of C syntax.
    {".h", Language::Cpp},
    {".cpp", Language::Cpp},
    {".cxx", Language::Cpp},
    {".cc", Language::Cpp},
    {".hpp", Language::Cpp},
    {".hh", Language::Cpp},
    {".hxx", Language::Cpp},
    {".rs", Language::Rust},
    {".java", Language::Java},
    {".cs", Language::CSharp},
    {".go", Language::Go},
    {".rb", Language::Ruby},
    {".php", Language::Php},
    {".phtml", Language::Php},
    {".sql", Language::Sql},
    {".ddl", Language::Sql},
    {".pks", Language::Sql}, // Oracle PL/SQL package spec
    {".pkb", Language::Sql}, // Oracle PL/SQL package body
}};

/// Lowercase an extension string in-place for case-insensitive lookup.
/// ASCII-only is fine — file extensions that matter are all ASCII.
[[nodiscard]] std::string to_lower_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// Cheap substring presence check on the head of a content sample.
[[nodiscard]] bool head_contains(std::string_view head, std::string_view needle) noexcept
{
    return head.find(needle) != std::string_view::npos;
}

} // namespace

Language refine_language(Language tentative, std::string_view extension,
                         std::string_view content) noexcept
{
    // Only `.h` is path-ambiguous in our extension table — `.hpp` /
    // `.hh` / `.hxx` are unambiguously C++, and every other extension
    // we map is single-language. A `.h` file in a directory without a
    // single C/C++ sibling, e.g. an HTML help system that ships its
    // JS includes with `.h` extensions, otherwise inflates the C++
    // language count and drags non-C++ symbols into the digest.
    if (extension != ".h" || tentative != Language::Cpp) {
        return tentative;
    }

    // Sniff the first 4 KiB — enough to spot include guards, license
    // banners, leading function/var declarations.
    constexpr std::size_t k_sniff_bytes = 4096;
    const std::string_view head = content.substr(0, std::min(content.size(), k_sniff_bytes));

    // Any of these strongly imply a real C/C++ header.
    const bool looks_c = head_contains(head, "#include") || head_contains(head, "#define") ||
                         head_contains(head, "#pragma") || head_contains(head, "#ifndef") ||
                         head_contains(head, "extern \"C\"") || head_contains(head, "\nclass ") ||
                         head_contains(head, "\nnamespace ") || head_contains(head, "\ntypedef ") ||
                         head_contains(head, "\ntemplate<") || head_contains(head, "\ntemplate <");
    if (looks_c) {
        return tentative;
    }

    // No C-ish markers — does it actually look like JavaScript?
    const bool looks_js = head_contains(head, "function ") || head_contains(head, "var ") ||
                          head_contains(head, "\nlet ") || head_contains(head, "\nconst ") ||
                          head_contains(head, " => ") || head_contains(head, "=>");
    if (looks_js) {
        return Language::JavaScript;
    }

    // Genuinely ambiguous — keep the .h-as-C++ default rather than drop
    // the file entirely.
    return tentative;
}

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
    }
    catch (...) { // NOLINT(bugprone-empty-catch)
        // Intentional swallow: detect_language is a noexcept query used
        // on every scanned file. Any filesystem / allocation hiccup
        // falls through to Unknown so the scan can keep going; the
        // caller will simply skip that file as unsupported.
    }
    return Language::Unknown;
}

std::string_view language_name(Language language) noexcept
{
    switch (language) {
    case Language::Python:
        return "Python";
    case Language::JavaScript:
        return "JavaScript";
    case Language::TypeScript:
        return "TypeScript";
    case Language::C:
        return "C";
    case Language::Cpp:
        return "C++";
    case Language::Rust:
        return "Rust";
    case Language::Java:
        return "Java";
    case Language::CSharp:
        return "C#";
    case Language::Go:
        return "Go";
    case Language::Ruby:
        return "Ruby";
    case Language::Php:
        return "PHP";
    case Language::Sql:
        return "SQL";
    case Language::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

Language language_from_name(std::string_view name) noexcept
{
    if (name == "Python") {
        return Language::Python;
    }
    if (name == "JavaScript") {
        return Language::JavaScript;
    }
    if (name == "TypeScript") {
        return Language::TypeScript;
    }
    if (name == "C") {
        return Language::C;
    }
    if (name == "C++") {
        return Language::Cpp;
    }
    if (name == "Rust") {
        return Language::Rust;
    }
    if (name == "Java") {
        return Language::Java;
    }
    if (name == "C#") {
        return Language::CSharp;
    }
    if (name == "Go") {
        return Language::Go;
    }
    if (name == "Ruby") {
        return Language::Ruby;
    }
    if (name == "PHP") {
        return Language::Php;
    }
    if (name == "SQL") {
        return Language::Sql;
    }
    return Language::Unknown;
}

} // namespace vectis::code
