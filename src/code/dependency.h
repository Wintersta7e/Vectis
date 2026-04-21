#pragma once

#include <cstdint>
#include <string>

namespace vectis::code {

/// One directed edge in the project dependency graph.
///
/// `source_file_id` references the file containing the import /
/// include / use / require statement. `target_file_id` references
/// the file the import resolves to — or is 0 if the import could
/// not be resolved to a file inside the scanned project (i.e. it's
/// an external library, stdlib header, or similar).
///
/// The raw `import_string` is preserved either way so consumers can
/// display unresolved externals or debug resolution failures.
struct Dependency {
    std::int64_t source_file_id = 0;
    std::int64_t target_file_id = 0;   ///< 0 if external / unresolved
    std::string  import_string;        ///< e.g. "./foo", "core/log.h"
    std::string  kind;                 ///< "include" | "import" | "use" | "require"
};

/// True if this dependency resolved to a file inside the project.
[[nodiscard]] inline bool is_resolved(const Dependency& dep) noexcept
{
    return dep.target_file_id != 0;
}

} // namespace vectis::code
