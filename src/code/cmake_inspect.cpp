#include "code/cmake_inspect.h"

#include <cctype>
#include <string>
#include <string_view>

#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::cmake {

namespace {

/// Strip CMake line comments: a `#` outside any quoted string starts
/// a comment that runs to end-of-line. The parser only needs lexical
/// approximation — we don't tokenize escape sequences, just skip
/// double-quoted spans so a `#` inside `set(MSG "use #foo")` is left
/// alone. Returns a fresh string with comments removed.
[[nodiscard]] std::string strip_comments(std::string_view src)
{
    std::string out;
    out.reserve(src.size());
    bool in_string = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (in_string) {
            out.push_back(c);
            if (c == '"' && (i == 0 || src[i - 1] != '\\')) {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }
        if (c == '#') {
            // Skip to next newline. The newline itself is kept so line
            // numbers stay aligned for any future error reporting.
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
            if (i < src.size()) {
                out.push_back('\n');
            }
            continue;
        }
        out.push_back(c);
    }
    return out;
}

/// Identifier-ish predicate covering ASCII letters, digits, and `_`.
/// CMake target names and command names are restricted to this set;
/// allowing `-` and `.` would over-match.
[[nodiscard]] bool is_identifier_byte(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

/// Scan `src` for `<keyword>(<name> [maybe-form])` calls. Returns the
/// count of qualifying calls.
///
/// `excluded_second_token` lets the caller reject `add_library` calls
/// whose second token is `ALIAS` or `IMPORTED` (those don't define a
/// new buildable target — they reference an existing one or import
/// from outside the project). Pass an empty initialiser list to count
/// every call indiscriminately.
[[nodiscard]] std::size_t count_calls(std::string_view src, std::string_view keyword,
                                      std::initializer_list<std::string_view> excluded_second_token)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = src.find(keyword, pos)) != std::string::npos) {
        // Boundary check: the keyword must start at the file head or
        // after a non-identifier byte. Otherwise `qmake_add_library`
        // or `try_add_library` would match.
        const bool prefix_ok = pos == 0 || !is_identifier_byte(src[pos - 1]);
        std::size_t after = pos + keyword.size();
        if (!prefix_ok) {
            pos = after;
            continue;
        }
        // The keyword must be followed by `(` (with optional intervening
        // whitespace). CMake is whitespace-tolerant: `add_library (foo)`
        // is legal.
        while (after < src.size() && (src[after] == ' ' || src[after] == '\t')) {
            ++after;
        }
        if (after >= src.size() || src[after] != '(') {
            pos = after;
            continue;
        }
        ++after; // step past `(`
        // Skip whitespace, then capture the target name. CMake target
        // names may contain `::` (namespaced aliases like `Qt6::Core`),
        // so we read up to the next whitespace, `(`, or `)`.
        while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])) != 0) {
            ++after;
        }
        const std::size_t name_start = after;
        while (after < src.size() && src[after] != ' ' && src[after] != '\t' &&
               src[after] != '\r' && src[after] != '\n' && src[after] != ')') {
            ++after;
        }
        if (after == name_start) {
            pos = after;
            continue;
        }
        // Optionally read the second token (ALIAS / IMPORTED / INTERFACE / ...).
        std::size_t cursor = after;
        while (cursor < src.size() && (src[cursor] == ' ' || src[cursor] == '\t')) {
            ++cursor;
        }
        const std::size_t second_start = cursor;
        while (cursor < src.size() && is_identifier_byte(src[cursor])) {
            ++cursor;
        }
        const std::string_view second_token = src.substr(second_start, cursor - second_start);

        bool excluded = false;
        for (const auto excl : excluded_second_token) {
            if (second_token == excl) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            ++count;
        }
        pos = cursor;
    }
    return count;
}

} // namespace

std::optional<RootTargets> inspect_root(const std::filesystem::path& project_root)
{
    const auto cmake_path = project_root / "CMakeLists.txt";
    auto contents = vectis::platform::read_file(cmake_path);
    if (!contents) {
        return std::nullopt;
    }
    const std::string stripped = strip_comments(*contents);

    // ALIAS and IMPORTED libraries don't define a new buildable target,
    // so they shouldn't push us toward LibraryOnly. INTERFACE
    // (header-only) and OBJECT / STATIC / SHARED / MODULE all DO count
    // as project libraries.
    const std::size_t lib_count = count_calls(stripped, "add_library", {"ALIAS", "IMPORTED"});
    const std::size_t exe_count = count_calls(stripped, "add_executable", {"IMPORTED"});

    if (lib_count == 0 && exe_count == 0) {
        return std::nullopt;
    }
    if (lib_count > 0 && exe_count == 0) {
        return RootTargets::LibraryOnly;
    }
    if (lib_count == 0 && exe_count > 0) {
        return RootTargets::ExecutableOnly;
    }
    return RootTargets::Mixed;
}

} // namespace vectis::code::cmake
