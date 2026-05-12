#include "code/gitignore.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

namespace vectis::code {

namespace {

[[nodiscard]] bool has_glob_meta(std::string_view s) noexcept
{
    return std::ranges::any_of(s, [](char c) { return c == '*' || c == '?'; });
}

/// Categorise one trimmed gitignore line into either an exact directory
/// name, a glob pattern, or "skip".
struct Categorised
{
    enum class Kind : std::uint8_t
    {
        Skip,
        Exact,
        Glob,
    };
    Kind kind = Kind::Skip;
    std::string_view value;
};

[[nodiscard]] Categorised categorise(std::string_view line) noexcept
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

    // Bracket expressions (`[abc]`) are still unsupported by `wildcard_match`,
    // so we drop those rather than silently mis-matching.
    if (line.find('[') != std::string_view::npos) {
        return {};
    }

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

    if (line.empty()) {
        return {};
    }

    if (has_glob_meta(line)) {
        return {Categorised::Kind::Glob, line};
    }
    return {Categorised::Kind::Exact, line};
}

} // namespace

bool wildcard_match(std::string_view pattern, std::string_view name) noexcept
{
    // Iterative greedy matcher with single-star backtracking. Time
    // complexity is O(|pattern| * |name|) worst-case (consecutive `*`s
    // never backtrack past the most-recent `*`), and patterns/names at
    // call sites are directory basenames — never long.
    std::size_t pi = 0;
    std::size_t ni = 0;
    std::size_t star_pi = std::string_view::npos;
    std::size_t star_ni = 0;

    while (ni < name.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
            ++pi;
            ++ni;
        }
        else if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi;
            star_ni = ni;
            ++pi;
        }
        else if (star_pi != std::string_view::npos) {
            // Backtrack: re-anchor the most recent `*` over one more
            // character of `name`.
            pi = star_pi + 1;
            ++star_ni;
            ni = star_ni;
        }
        else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') {
        ++pi;
    }
    return pi == pattern.size();
}

GitignorePatterns read_gitignore_dir_patterns(const std::filesystem::path& root)
{
    GitignorePatterns result;

    std::ifstream in(root / ".gitignore");
    if (!in) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        const Categorised cat = categorise(line);
        switch (cat.kind) {
        case Categorised::Kind::Exact:
            result.exact_names.emplace(cat.value);
            break;
        case Categorised::Kind::Glob:
            result.glob_patterns.emplace_back(cat.value);
            break;
        case Categorised::Kind::Skip:
            break;
        }
    }
    return result;
}

bool is_excluded_basename(const std::filesystem::path& dir,
                          const std::unordered_set<std::string>& exact_names,
                          const std::vector<std::string>& glob_patterns)
{
    const std::string name = dir.filename().string();
    if (exact_names.contains(name)) {
        return true;
    }
    return std::ranges::any_of(glob_patterns,
                               [&](const std::string& glob) { return wildcard_match(glob, name); });
}

} // namespace vectis::code
