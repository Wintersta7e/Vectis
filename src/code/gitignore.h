#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vectis::code {

/// Patterns extracted from `<root>/.gitignore`, split by matching shape
/// so the scanner can dispatch each through its cheapest predicate:
///
///   - `exact_names` — bare directory basenames (`build`, `.venv`,
///     `target`); `unordered_set::contains(name)` is O(1).
///   - `glob_patterns` — directory-level patterns that use `*` or `?`
///     wildcards (`build-*`, `cmake-build-*`, `dist-?`); requires the
///     scanner to walk the list and test each via `wildcard_match`.
struct GitignorePatterns
{
    std::unordered_set<std::string> exact_names;
    std::vector<std::string> glob_patterns;
};

/// Read `<root>/.gitignore` and reduce each line to a directory-level
/// match form. Patterns plug directly into `ScanConfig::exclude_dir_names`
/// and `ScanConfig::exclude_dir_globs`, both matched against directory
/// basenames during the recursive walk.
///
/// Supported pattern shapes:
///
///   - `build/`              → exact name "build"
///   - `.venv`               → exact name ".venv"
///   - `/target/`            → exact name "target" (leading `/` stripped)
///   - `build-*/`            → glob "build-*"
///   - `*.egg-info`          → glob "*.egg-info"
///   - `cmake-build-?`       → glob "cmake-build-?"
///
/// Unsupported shapes (silently dropped):
///
///   - `docs/build/`         — path prefix, needs multi-segment match
///   - `!important/`         — negation
///   - `# comment`           — comment
///   - `[abc]/`              — character class (use `?` or full name)
///
/// Falling back to no-op on unsupported patterns is deliberate: a
/// false-negative (something the user wanted excluded isn't) is a
/// performance issue, whereas a false-positive (matching something we
/// shouldn't) would silently drop legitimate code from the digest.
///
/// Returns empty patterns if `<root>/.gitignore` is absent or unreadable.
[[nodiscard]] GitignorePatterns read_gitignore_dir_patterns(const std::filesystem::path& root);

/// Match `name` against a glob `pattern` containing `*` (any run of
/// chars) and `?` (single char) metacharacters. No bracket expressions
/// or path-segment globbing — this is a basename-only matcher. Greedy
/// backtracking implementation; pattern + name are both expected to be
/// short (directory basenames), so worst-case backtracking is fine.
[[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view name) noexcept;

/// Check whether a directory should be excluded by basename, given a
/// set of exact names and a list of glob patterns (same shape as
/// `ScanConfig::exclude_dir_names` / `exclude_dir_globs`). Returns
/// true on a hit. Used by the source scanner and by manifest
/// handlers — both observe the same exclude rules.
[[nodiscard]] bool is_excluded_basename(const std::filesystem::path& dir,
                                        const std::unordered_set<std::string>& exact_names,
                                        const std::vector<std::string>& glob_patterns);

} // namespace vectis::code
