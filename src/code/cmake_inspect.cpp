#include "code/cmake_inspect.h"

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::cmake {

namespace {

/// Strip CMake `#` line comments. Tracks double-quoted spans so a `#`
/// inside `"use #foo"` is preserved. Does **not** parse CMake bracket
/// arguments (`[[ … ]]` / `[=[ … ]=]`) or bracket comments (`#[[ … ]]`)
/// — those are rare at the root and would over-engineer this
/// corroborator-only lexer. Newlines are preserved so any future error
/// reporting can still cite source line numbers.
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

/// Scan `src` for `<keyword>(<name> [maybe-form])` calls. The caller
/// supplies `excluded_second_token` to reject calls whose second token
/// is e.g. `ALIAS` or `IMPORTED` — these reference an existing target
/// or import from outside the project, so they don't count as a new
/// declaration.
[[nodiscard]] std::size_t count_calls(std::string_view src, std::string_view keyword,
                                      std::initializer_list<std::string_view> excluded_second_token)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = vectis::core::find_word_boundary(src, keyword, pos)) != std::string_view::npos) {
        std::size_t after = pos + keyword.size();
        // CMake tolerates whitespace before `(`: `add_library (foo)`.
        while (after < src.size() && (src[after] == ' ' || src[after] == '\t')) {
            ++after;
        }
        if (after >= src.size() || src[after] != '(') {
            pos = after;
            continue;
        }
        ++after;
        while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])) != 0) {
            ++after;
        }
        // Target names can contain `::` (namespaced aliases like
        // `Qt6::Core`), so read up to the next whitespace or `)`.
        const std::size_t name_start = after;
        while (after < src.size() && src[after] != ' ' && src[after] != '\t' &&
               src[after] != '\r' && src[after] != '\n' && src[after] != ')') {
            ++after;
        }
        if (after == name_start) {
            pos = after;
            continue;
        }
        std::size_t cursor = after;
        while (cursor < src.size() && (src[cursor] == ' ' || src[cursor] == '\t')) {
            ++cursor;
        }
        const std::size_t second_start = cursor;
        while (cursor < src.size() && vectis::core::is_ascii_identifier_byte(src[cursor])) {
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

std::string_view signal_for(RootTargetShape shape) noexcept
{
    switch (shape) {
    case RootTargetShape::LibraryOnly:
        return "cmake:library-only";
    case RootTargetShape::ExecutableOnly:
        return "cmake:executable-only";
    case RootTargetShape::Mixed:
        return "cmake:mixed-targets";
    }
    return "cmake:unknown";
}

std::optional<RootTargetShape> inspect_root(const std::filesystem::path& project_root)
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
        return RootTargetShape::LibraryOnly;
    }
    if (lib_count == 0 && exe_count > 0) {
        return RootTargetShape::ExecutableOnly;
    }
    return RootTargetShape::Mixed;
}

} // namespace vectis::code::cmake
