#include "code/gitignore.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

namespace vectis::code {

namespace {

[[nodiscard]] bool has_glob_meta(std::string_view s) noexcept
{
    return std::ranges::any_of(s, [](char c) { return c == '*' || c == '?' || c == '['; });
}

/// Reduce one gitignore line to a bare directory name, or return an
/// empty view if the pattern can't be represented by basename alone.
[[nodiscard]] std::string_view to_dir_name(std::string_view line) noexcept
{
    // Trim trailing whitespace + CR (CRLF-flavoured files survive a
    // text-mode read on some platforms with the \r intact).
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    // Trim leading whitespace.
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }

    if (line.empty()) {
        return {};
    }
    if (line.front() == '#') {
        return {};
    } // comment
    if (line.front() == '!') {
        return {};
    } // negation (unsupported)
    if (has_glob_meta(line)) {
        return {};
    } // wildcard (unsupported)

    // Strip leading `/` — in gitignore this anchors the pattern to the
    // repo root, but our basename-matching scanner ignores path depth
    // anyway. A leading-slash pattern still reduces to the name.
    if (line.front() == '/') {
        line.remove_prefix(1);
    }
    // Trailing `/` marks an explicit directory pattern.
    if (!line.empty() && line.back() == '/') {
        line.remove_suffix(1);
    }

    // Any remaining slash means the pattern addresses a path, not a
    // bare name. Skip — matching that would require multi-segment
    // logic the scanner doesn't have today.
    if (line.find('/') != std::string_view::npos) {
        return {};
    }

    return line;
}

} // namespace

std::unordered_set<std::string> read_gitignore_dir_patterns(const std::filesystem::path& root)
{
    std::unordered_set<std::string> result;

    std::ifstream in(root / ".gitignore");
    if (!in) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        const std::string_view name = to_dir_name(line);
        if (!name.empty()) {
            result.emplace(name);
        }
    }
    return result;
}

} // namespace vectis::code
