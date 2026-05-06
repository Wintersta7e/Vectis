#pragma once

#include <string>
#include <unordered_set>

namespace vectis::code {

/// Canonical "directories the scanner refuses to walk into" list.
/// Used both by the CLI when building a `ScanConfig` and by the
/// architecture detector's disk walk, so the two surfaces cannot
/// drift. .gitignore-derived names are merged into the runtime
/// `ScanConfig::exclude_dir_names` and threaded back to the
/// detector via `ExportOptions`.
[[nodiscard]] const std::unordered_set<std::string>& default_scanner_exclude_dir_names();

/// Heuristic check for "this looks like a vendored / minified third-
/// party JS bundle the user did not write". Skips files matching
/// well-known names (jquery-1.x.js, prototype.js, require.js,
/// dhtmlx*.js, bootstrap.bundle.js, …) and any `*.min.js` bundle.
/// Filename comparison is ASCII-lowercased.
[[nodiscard]] bool looks_like_vendored_js(std::string_view filename) noexcept;

} // namespace vectis::code
