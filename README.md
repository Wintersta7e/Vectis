# Vectis

Portable C++20 CLI that produces structured digests of source-tree
architecture — symbols, dependencies, architecture label, complexity
hotspots — for consumption by external LLM agents (Claude Code, CI
pipelines, scripts).

[![CI](https://github.com/Wintersta7e/Vectis/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Wintersta7e/Vectis/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](./LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C?logo=cmake)
![tree-sitter](https://img.shields.io/badge/tree--sitter-12_languages-2B7489)

> A tool I built for myself to suit my own workflow. If you find it
> useful, you're welcome to use it.

## What you get

- **12 languages** — Python, JavaScript, TypeScript, C, C++, Rust,
  Java, C#, Go, Ruby, PHP, SQL.
- **Manifest-file dependency graph** — module / parent / dependency
  / managed-dependency / BOM edges from Maven `pom.xml` files;
  project / package / import / solution edges from `.csproj` /
  `.fsproj` / `.vbproj` / `.sln` / `.slnx` with Central Package
  Management resolution via nearest-ancestor `Directory.Packages.props`.
  Spring `<beans>` XML and `.properties` are next.
- **11 architecture labels** with 0–100 confidence — Monolith,
  Layered, MVC, MVVM, Clean Architecture, Monorepo, Frontend SPA,
  API Backend, .NET Solution, Library, Electron. The first ten are
  **calibrated against a 33-project reference corpus at 100%
  precision/recall per class**; Electron is unit-tested but not yet
  corpus-calibrated.
- **Per-symbol API surface** — every symbol carries a `visibility`
  field (`public` / `private` / `protected` / `internal`) derived
  from each language's native idiom (Go capitalisation, Python
  underscore convention, Rust `pub` keyword, Java/C#/TypeScript
  modifiers).
- **Decorator / annotation capture** for Python, Java, C#, and
  Rust. The slim digest carries `@app.route(...)`, `@RestController`,
  `[HttpGet]`, `#[tokio::test]` etc. as structured strings — agents
  can find route handlers, tests, DI markers without re-parsing
  source.
- **Cross-file dependency graph** with namespace-aware resolution
  (Java/C#/PHP via namespace index, Go via `go.mod`, Python relative
  imports against the source package).
- **Cycle detection** (Tarjan iterative SCC) and complexity-based
  **hotspot** ranking with body excerpts in the full digest.
- **`vectis explain`** — a 10-line plain-text narrative summary
  consumed directly by humans / LLM agents.
- **`.gitignore`-aware scanning** plus an aggressive default exclude
  list so virtualenvs and build outputs never pollute the digest.
- **Incremental rescans** via `--cache` — content-hash diff, only
  re-parses changed files between runs.
- **Single binary**, zero runtime deps when statically linked. No
  network calls during digest production.

## Quick start

System packages (Ubuntu 24.04 / WSL2):

```bash
sudo apt install -y build-essential cmake ninja-build git pkg-config \
    libsqlite3-dev libspdlog-dev libfmt-dev nlohmann-json3-dev \
    libtomlplusplus-dev libgtest-dev

cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

Then:

```bash
./build/vectis explain /path/to/project        # narrative summary
./build/vectis digest  /path/to/project --format slim    # structured JSON
```

`vectis explain` is the fastest way to orient an agent in an
unfamiliar repo. Sample output (a Python library project):

```
sample-lib — Library (75% confidence)
Architecture: Python library (pyproject.toml + `sample_lib/__init__.py`,
              no app entry).
Scale: 85 files, 1622 symbols, 613 dependency edges.
Languages: Python (98%, 83 files), SQL (2%, 2 files).
API surface: 1575 public / 47 private.

Top hotspots (by cyclomatic complexity):
  src/sample_lib/scopes/registry.py:273  register      [function, complexity 22]
  src/sample_lib/app.py:1224             make_response [function, complexity 17]
  ...

Decorators (top 5 over 657 decorated symbols): @app.route("/") (99),
  @setupmethod (43), @t.overload (18), @fixture (17),
  @teardown_request (14).

Dependency graph: 171 internal edges, 1 cycle.
External imports (top 5): sample_lib (78), test-framework (23),
  http-lib.exceptions (23), http-lib.routing (19), os (15).
```

Slim JSON for pipelines (excerpt):

```json
{
  "architecture": {
    "confidence": 75, "label": "Library",
    "signals": ["layout:library", "manifest:pyproject.toml"]
  },
  "symbols": [
    { "name": "register", "kind": "function",
      "path": "src/sample_lib/scopes/registry.py", "line": 273,
      "visibility": "public", "decorators": ["setupmethod"] }
  ],
  "dependency_graph": {
    "edges": [
      { "source": "src/sample_lib/app.py", "target": "src/sample_lib/scopes/registry.py",
        "kind": "import", "import_ref": "scopes.registry" },
      { "source": "pom.xml", "target": "app/pom.xml",
        "kind": "maven-module" },
      { "source": "src/sample_lib/app.py", "target": null,
        "target_external": "requests", "kind": "import" }
    ],
    "stats": { "total_edges": 613, "internal_edges": 171, "external_edges": 442, "cycles": 1 }
  },
  "hotspots": [ /* top 10, no body excerpts */ ],
  "project": { "file_count": 85, "symbol_count": 1622 }
}
```

Edge schema: internal edges carry `target` (resolved file path) and
optionally `import_ref` (the source-level coordinate / FQCN — handy
for Maven / Spring / NuGet hops where the path alone hides intent).
External edges set `target: null` and carry `target_external` with
the unresolved import literal (e.g. `"react"`, `"requests"`).
`stats.cycles` is the count of dependency cycles detected; the full
JSON additionally lists each cycle as an array of paths.

A vcpkg path is wired for Windows / portable static builds; see
`CMakeLists.txt`.

## Subcommands and formats

| Command                       | Output | Use case                                                  |
|---|---|---|
| `vectis explain`              | text   | Narrative summary for humans / LLM agents                 |
| `vectis digest --format slim` | JSON   | Token-efficient structured map for agent context          |
| `vectis digest --format json` | JSON   | Full per-file symbols, hotspot excerpts, flat `symbols[]` |

Common flags (`--cache`, `--cache-dir`, `--output`, `-q` / `-v`)
work on both subcommands. `vectis --help` lists everything.

## Pipeline

```
  scan tree (.gitignore + built-in excludes)
        │
        ▼
  parse files (tree-sitter, 12 grammars)
        │  symbols + raw imports + namespaces
        ▼
  manifest pass (pom.xml, .csproj, .sln/.slnx, .props/.targets)
        │  + maven / csproj / sln edges
        ▼
  resolve dependencies (paths + namespace index + go.mod)
        │
        ▼
  ┌──────────────┬──────────────┬──────────────┐
  │   cycles     │   hotspots   │ architecture │
  │  (Tarjan)    │  (severity)  │   (label)    │
  └──────────────┴──────────────┴──────────────┘
        │
        ▼
  emit digest  (slim JSON · full JSON)
```

State persists in `<project>/vectis-data/vectis.db` (SQLite WAL +
FTS5) when `--cache` is used.

## What it isn't

No GUI. No LSP server. No embedded LLM or chat UI. No code
modification — read-only, always. No network calls during digest
production.

## License

MIT. Third-party attribution in `NOTICES.md`.
