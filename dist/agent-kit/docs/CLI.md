# Vectis CLI Reference

```
vectis — portable developer intelligence tool
```

---

## Subcommands

### `vectis digest <path> [options]`

Scan `<path>` and emit a structured digest of the source tree.

**Options:**

| Flag | Default | Description |
|---|---|---|
| `--format slim\|json` | `slim` | Output format. Both are JSON; `slim` is token-efficient (no body excerpts), `json` adds per-file symbols and hotspot source excerpts. |
| `--output <file>\|-` | stdout | Write output to `<file>`, or `-` for stdout. |
| `--cache` | off | Reuse SQLite state between runs at `<path>/vectis-data/vectis.db`. First call populates; subsequent calls do a content-hash incremental scan. |
| `--cache-dir <dir>` | — | Override cache location (implies `--cache`). Useful for read-only project directories. |
| `-q`, `--quiet` | off | Suppress non-error output on stderr. |
| `-v`, `--verbose` | off | Print scan stats (files, symbols, edges, elapsed ms) to stderr. |

**Examples:**

```bash
vectis digest ./project                              # Slim JSON to stdout
vectis digest ./project --format json               # Full JSON with excerpts
vectis digest ./project --cache                     # Cached slim JSON
vectis digest ./project --cache-dir /tmp/vc --format slim   # Custom cache dir
vectis digest ./project --format slim --output -    # Explicit stdout
vectis digest ./project --format json --output digest.json  # File output
```

---

### `vectis explain <path> [options]`

Scan `<path>` and print a short plain-text narrative summary (~20 lines). Covers architecture label, scale, languages, API surface, top hotspots, decorator distribution, and dependency graph stats.

Supports `--cache`, `--cache-dir`, and `-q`.

**Example:**

```bash
vectis explain ./project
```

**Sample output:**

```
my-project — Layered (82% confidence)
Architecture: Layered service with a clear separation between api/, service/, and repository/ layers.
Scale: 142 files, 2847 symbols, 1104 dependency edges.
Languages: TypeScript (89%, 126 files), SQL (11%, 16 files).
API surface: 2310 public / 537 private.

Top hotspots (by cyclomatic complexity):
  src/service/order.ts:88   processOrder   [function, complexity 34]
  src/api/router.ts:22      dispatch       [function, complexity 18]
  ...

Dependency graph: 884 internal edges, 3 cycles.
External imports (top 5): express (47), pg (23), zod (18), date-fns (12), uuid (9).
```

---

### `vectis guide`

Print a complete, framework-neutral agent guide to stdout and exit — when and how to use vectis, how to read the digest output, and the MCP entry point. This is the zero-setup way to teach any agent to use vectis: run it and feed the output into the agent's context. The kit's `skill/vectis/SKILL.md` is generated from this command.

```bash
vectis guide
```

---

### `vectis mcp`

Start a Model Context Protocol server on stdio. Exposes the `digest` and `explain` tools to MCP clients such as Claude Code, Codex, or Cursor. The server reads requests from stdin and writes responses to stdout following the MCP protocol.

**Example integration (MCP client `settings.json`, e.g. Claude Code):**

```json
{
  "mcpServers": {
    "vectis": {
      "command": "vectis",
      "args": ["mcp"]
    }
  }
}
```

---

### `vectis --version`

Print `vectis <version>` to stdout and exit.

```bash
vectis --version   # also: vectis version
```

---

### `vectis --help`

Print the full usage summary and exit.

---

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Usage / argument error |
| `2` | Scan, export, or I/O failure |

---

## Notes

- **`--cache` is additive**: the first run fully scans and stores; subsequent runs diff content hashes and only re-parse changed files. Removing `--cache` from a subsequent call reverts to a full in-memory scan without touching the cache.
- **Large repos**: pipe through `--output` to a file and parse offline rather than holding the full JSON string in memory.
- **Quiet in CI**: combine `-q` with `--output <file>` so only actual failures write to stderr.
