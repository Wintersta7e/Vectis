#pragma once

#include <cstdint>
#include <vector>

namespace vectis::code {

class CodeIndex;

/// One strongly-connected component of the dependency graph whose
/// size is > 1 (or size == 1 with a self-loop) — i.e. a genuine
/// circular dependency. Files are listed in the order they appear in
/// the SCC's discovery stack, which is consistent but not otherwise
/// meaningful.
struct DependencyCycle
{
    std::vector<std::int64_t> file_ids;
};

/// Run Tarjan's SCC on the dependency edges stored in `index` and
/// return every cycle found. Only internal edges
/// (`target_file_id != 0`) are considered — externals don't create
/// cycles. Singleton SCCs (a file with no self-loop) are omitted
/// from the result.
///
/// Thread-safety: snapshots the index once at the top of the call,
/// so concurrent mutation won't corrupt the traversal, but may cause
/// the returned cycles to reflect a slightly stale state.
[[nodiscard]] std::vector<DependencyCycle> detect_cycles(const CodeIndex& index);

} // namespace vectis::code
