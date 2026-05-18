#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vectis::code {

class CodeIndex;
struct Dependency;

/// One strongly-connected component of the dependency graph whose
/// size is > 1 (or size == 1 with a self-loop) — i.e. a genuine
/// circular dependency. Files are listed in the order they appear in
/// the SCC's discovery stack, which is consistent but not otherwise
/// meaningful.
struct DependencyCycle
{
    std::vector<std::int64_t> file_ids;
};

/// Run Tarjan's SCC on `deps` and return every cycle found. Only
/// internal edges (`target_file_id != 0`) are considered — externals
/// don't create cycles. Singleton SCCs (a file with no self-loop) are
/// omitted from the result.
[[nodiscard]] std::vector<DependencyCycle> detect_cycles(std::span<const Dependency> deps);

/// Wrapper that snapshots the index once and forwards to the span
/// overload. Callers that already hold an `all_dependencies()`
/// snapshot should pass it directly.
[[nodiscard]] std::vector<DependencyCycle> detect_cycles(const CodeIndex& index);

} // namespace vectis::code
