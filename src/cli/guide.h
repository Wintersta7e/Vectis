#pragma once

#include <ostream>

namespace vectis::cli {

/// Write the framework-neutral agent guide — when and how to use vectis, how
/// to read the digest output, and the MCP entry point — to `out`.
///
/// This text is the single source of truth for the agent kit's
/// `skill/vectis/SKILL.md`, which is generated from `vectis guide` output at
/// release time (see `scripts/build-agent-kit.sh`). A test guards against the
/// committed kit file drifting from this function.
void print_guide(std::ostream& out);

} // namespace vectis::cli
