# Vectis Output Schema

This document describes the JSON shape produced by `vectis digest`. There are two formats:

- **slim** (`--format slim`) — token-efficient, designed for agent context windows. No source body excerpts. This is the default.
- **json** (`--format json`) — full format; adds per-file symbol arrays and source body excerpts on hotspot entries.

The two formats overlap but are not identical. Shared keys: `project`,
`architecture`, `files`, `dependency_graph`, `central_files`, `hotspots`,
`fidelity_metadata`, `vectis_version`. The lookup tables `_schema`,
`encoding`, `kinds`, `languages`, and `refs` appear **only in slim**; the full
format instead adds a top-level `symbols` array (plus per-file symbols and
hotspot excerpts). The keys documented below are the slim shape; full-only
differences are noted inline.

---

## Top-level keys

```json
{
  "_schema":          { ... },
  "vectis_version":   "0.1.0",
  "project":          { ... },
  "architecture":     { ... },
  "encoding":         { ... },
  "files":            [ ... ],
  "kinds":            [ ... ],
  "languages":        [ ... ],
  "refs":             [ ... ],
  "dependency_graph": { ... },
  "central_files":    [ ... ],
  "hotspots":         [ ... ],
  "fidelity_metadata": { ... }
}
```

---

### `_schema`

Describes the encoding conventions used in this document. Important keys:

- `name` — `"vectis.slim"` or `"vectis.full"` depending on format.
- `version` — schema version integer.
- `edge_tuple` — the order of fields in each edge tuple: `["source_file_id", "target_file_id|null", "kind_id", "ref_id|null"]`.
- `edge_semantics` — `target_file_id null` means unresolved/external; non-null means an internal edge. `ref_id` when present indexes into `refs[]`.
- `file_id_semantics` — `file_id` is a stable identifier, **not** an array offset into `files[]`.
- `cycle_semantics` — each cycle array repeats the first `file_id` at the end to close the loop.

---

### `project`

```json
{
  "name": "my-project",
  "root": "<project-path>",
  "file_count": 42,
  "symbol_count": 315,
  "dependency_count": 128
}
```

---

### `architecture`

```json
{
  "label": "Layered",
  "confidence": 82,
  "signals": ["layout:layered", "dir:service", "dir:repository", "dir:api"]
}
```

- `label` — one of: `Monolith`, `Layered`, `MVC`, `MVVM`, `Monorepo`, `Frontend SPA`, `API Backend`, `Clean Architecture`, `.NET Solution`, `Library`, `Electron`, `Integration Framework` (or `Unknown` when no pattern matches).
- `confidence` — integer 0–100. Higher is more certain.
- `signals` — list of evidence strings (directory names, manifest files, entry-point counts) that drove the label.

---

### `encoding`

Index counts that define array bounds for the lookup tables below.

```json
{
  "files": 3,
  "kinds": 1,
  "languages": 1,
  "refs": 3
}
```

---

### `files`

Compact file index. Each entry has a stable `id`, a `lang` (index into `languages[]`), and a relative `path` from the project root.

```json
[
  { "id": 1, "lang": 0, "path": "main.py" },
  { "id": 2, "lang": 0, "path": "models/user.py" },
  { "id": 3, "lang": 0, "path": "utils/helpers.py" }
]
```

---

### `kinds` and `languages`

String lookup tables for the integer IDs used in edges and files.

```json
"kinds":     ["import"],
"languages": ["Python"]
```

---

### `refs`

The raw import strings (or manifest coordinates / FQCNs for Maven, NuGet, Spring) referenced by edges. Indexed by `ref_id` in edge tuples.

```json
["dataclasses", "models.user", "utils.helpers"]
```

---

### `dependency_graph`

```json
{
  "edges": [
    [1, 2, 0, 1],
    [1, 3, 0, 2],
    [2, null, 0, 0],
    [3, 2, 0, 1]
  ],
  "cycles": [],
  "stats": {
    "by_kind": { "import": 4 },
    "total_edges": 4,
    "internal_edges": 3,
    "external_edges": 1,
    "cycles": 0
  }
}
```

Each edge is a 4-tuple `[source_file_id, target_file_id|null, kind_id, ref_id|null]` (see `_schema.edge_tuple`). A `null` target means the import could not be resolved to a file in the project — it is an external dependency (library, stdlib, etc.).

`cycles` is an array of file-id arrays; each cycle repeats its first element at the end. Empty when no cycles exist.

---

### `central_files`

Top files by centrality score (fan-in weighted). Useful for identifying core modules.

```json
[
  { "file": "models/user.py", "file_id": 2, "score": 0.52 }
]
```

---

### `hotspots`

Top 10 complexity hotspots (slim format; full format adds `excerpt` and `context`).

```json
[
  {
    "file": "src/service/order.ts",
    "line": 88,
    "symbol": "processOrder",
    "kind": "function",
    "complexity": 34,
    "fan_in": 12,
    "fan_out": 7,
    "severity": "high"
  }
]
```

`severity` is one of `low`, `medium`, `high`, `critical`.

---

### `fidelity_metadata`

Per-language calibration data from the dependency resolver. Each entry describes the expected precision of the resolver for that language, measured against a reference corpus offline.

```json
{
  "caveat": "distribution-level expected reliability ...",
  "languages": {
    "python": {
      "scope": "python-import-edges",
      "version": "py-import-2026-06-15",
      "provisional": false,
      "corpus": { "projects": 9, "labeled_edges": 136 },
      "method": "per-strategy precision vs a hand-labeled stratified sample ...",
      "expected_precision": {
        "dotted-module": 0.95,
        "dotted-package": 0.95,
        "external-dotted": 0.95,
        "relative-module": 0.95,
        "relative-package": 0.95,
        "external-relative": 0.90
      }
    }
  }
}
```

Key points:

- Each edge in `dependency_graph.edges` is produced by a specific resolver stratum (e.g., `"dotted-module"`, `"go-internal"`, `"rust-use-internal-resolved"`). The expected precision for that stratum is the calibrated probability that the edge is correct.
- `provisional: true` means the calibration was measured on a smaller corpus or with less rigorous methodology; results may be updated as more data is collected.
- `caveat` reminds agents that this is a distribution-level estimate, not a per-repo guarantee.

**Languages with calibrated fidelity (as of 2026-06-15):** Python, JavaScript, TypeScript, C/C++, Rust, Java, C#, Go, Ruby, PHP (10 languages). All import-resolution strata are covered.

---

## `vectis explain` output

`vectis explain` is plain text, not JSON. It is intended for direct reading by humans or agents without JSON parsing. The format is approximately:

```
<project-name> — <architecture-label> (<confidence>% confidence)
Architecture: <one-line narrative>
Scale: <N> files, <M> symbols, <K> dependency edges.
Languages: <lang> (<pct>%, <N> files), ...
API surface: <pub> public / <priv> private.

Top hotspots (by cyclomatic complexity):
  <file>:<line>  <symbol>  [<kind>, complexity <N>]
  ...

Decorators (top 5 over <N> decorated symbols): <list>

Dependency graph: <internal> internal edges, <N> cycles.
External imports (top 5): <name> (<count>), ...
```
