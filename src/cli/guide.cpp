#include "cli/guide.h"

#include <ostream>

namespace vectis::cli {

namespace {

// Framework-neutral, self-contained. Reads cleanly both as plain text (piped
// to an agent via `vectis guide`) and as Markdown (the kit's SKILL.md body).
// Keep it self-sufficient for an agent that has only the binary.
constexpr const char* k_guide_text = R"GUIDE(# Vectis — codebase digest tool for agents

Vectis is a portable, read-only command-line tool that turns a source tree
into a compact, structured map an agent can consume in one shot: symbols, a
cross-file dependency graph, an architecture label, and complexity hotspots.
It runs fully locally, makes no network calls, and never modifies the code it
reads.

## When to use it

- Orient in an unfamiliar repository before opening files or running broad
  searches.
- Answer questions about architecture, module dependencies, dependency
  cycles, or complexity hotspots.
- Produce a structured snapshot for a CI step that watches for architectural
  drift or new dependency cycles between commits.

## How to run it (CLI)

    vectis explain <path>                                  # fast plain-text overview (start here)
    vectis digest  <path> --format slim                   # compact JSON for agent consumption
    vectis digest  <path> --format json --output out.json # full JSON (per-file symbols + hotspot excerpts)
    vectis digest  <path> --cache                         # incremental rescans (SQLite cache)

Start with `vectis explain`: it prints a ~20-line narrative (architecture,
scale, languages, top hotspots, dependency stats) and is usually enough to
orient. Pull the slim digest when you need to query specific edges or symbols.

## Reading the slim digest

Key top-level fields: `project`, `architecture`, `files`, `dependency_graph`,
`central_files`, `hotspots`, `fidelity_metadata`, plus the lookup tables
`kinds`, `languages`, and `refs` that the compact integer IDs index into.
`_schema` documents the encoding inline.

Dependency edges are compact 4-tuples:

    [source_file_id, target_file_id, kind_id, ref_id]

- `target_file_id` is `null` for an unresolved / external import (a library or
  stdlib module); `ref_id` then indexes `refs[]`, the raw import string.
- A non-null `target_file_id` is an internal edge to another file in the tree.
- `kind_id` indexes `kinds[]` (the edge kind, e.g. `import`); each entry in
  `files` carries a `lang` that indexes `languages[]`.
- `file_id` is a stable identifier, NOT an array offset into `files[]`.

`fidelity_metadata` reports the resolver's calibrated per-language precision,
so you know how much to trust each class of edge. `central_files` and
`hotspots` rank the highest-leverage files and functions.

## Using it as an MCP server

For agents that speak the Model Context Protocol, expose vectis as a tool
server instead of shelling out:

    vectis mcp   # stdio MCP server exposing the `digest` and `explain` tools

Register it with your MCP client; the tools mirror the CLI commands above. A
typical client config entry runs the command `vectis` with the argument `mcp`.

## Prerequisite

The `vectis` binary must be on your `PATH`. If the `VECTIS` environment
variable is set, use its value as the executable path in place of `vectis` in
every command above.

## Deeper reference

If you have the agent kit alongside the binary, `docs/OUTPUT.md` documents the
full digest schema and `docs/CLI.md` the complete flag reference.
)GUIDE";

} // namespace

void print_guide(std::ostream& out)
{
    out << k_guide_text;
}

} // namespace vectis::cli
