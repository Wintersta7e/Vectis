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

} // namespace vectis::code
