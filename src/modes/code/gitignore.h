#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace vectis::modes::code {

/// Read `<root>/.gitignore` and return the subset of entries that can
/// be mapped to bare directory names. Those names plug directly into
/// `ScanConfig::exclude_dir_names`, which is matched against each
/// directory's basename during the recursive walk.
///
/// Only simple name-only patterns are picked up — the scanner's
/// exclude mechanism is basename-based and can't represent path
/// prefixes or glob wildcards without additional machinery. These
/// forms survive:
///
///   - `build/`     → "build"
///   - `.venv`      → ".venv"
///   - `/target/`   → "target"  (leading slash stripped, still a name)
///
/// These forms are ignored (returned set excludes them):
///
///   - `docs/build/`   — path prefix, needs two-level match
///   - `*.egg-info/`   — wildcard, needs glob
///   - `!important/`   — negation, not supported
///   - `# comment`     — comment
///
/// Falling back to no-op on unsupported patterns is deliberate: a
/// false-negative (something the user wanted excluded isn't) is a
/// performance issue, whereas a false-positive (matching something we
/// shouldn't) would silently drop legitimate code from the digest.
///
/// Returns an empty set if `<root>/.gitignore` is absent or unreadable.
[[nodiscard]] std::unordered_set<std::string>
read_gitignore_dir_patterns(const std::filesystem::path& root);

} // namespace vectis::modes::code
