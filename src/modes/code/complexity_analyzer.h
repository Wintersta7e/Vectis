#pragma once

#include <tree_sitter/api.h>

#include "modes/code/language.h"

namespace vectis::modes::code {

/// Cyclomatic complexity for a single function / method node.
///
/// Walks the given tree-sitter subtree and counts decision-point
/// node types for the language, returning 1 + (number of branches,
/// loops, case labels, and short-circuit boolean operators found).
///
/// A trivial straight-line function returns 1. Adding one `if` takes
/// it to 2; adding a `for` inside takes it to 3, and so on.
///
/// Returns 1 for unknown / unsupported languages and for null nodes.
/// Never throws, never allocates.
[[nodiscard]] int
compute_cyclomatic_complexity(TSNode function_node, Language language) noexcept;

} // namespace vectis::modes::code
