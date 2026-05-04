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
- **10 architecture labels** with 0–100 confidence — Monolith,
  Layered, MVC, MVVM, Clean Architecture, Monorepo, Frontend SPA,
  API Backend, .NET Solution, Library. **Calibrated against a
  33-project reference corpus at 100% precision/recall per class.**
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
unfamiliar repo. Sample output (Flask):

```
flask — Library (75% confidence)
Architecture: Python library (pyproject.toml + `flask/__init__.py`,
              no app entry).
Scale: 85 files, 1622 symbols, 613 dependency edges.
Languages: Python (98%, 83 files), SQL (2%, 2 files).
API surface: 1575 public / 47 private.

Top hotspots (by cyclomatic complexity):
  src/flask/sansio/blueprints.py:273  register   [function, complexity 22]
  src/flask/app.py:1224  make_response             [function, complexity 17]
  ...

Decorators (top 5 over 657 decorated symbols): @app.route("/") (99),
  @setupmethod (43), @t.overload (18), @pytest.fixture (17),
  @app.teardown_request (14).

Dependency graph: 171 internal edges, 1 cycle.
External imports (top 5): flask (78), pytest (23), werkzeug.exceptions
  (23), werkzeug.routing (19), os (15).
```

Slim JSON for pipelines (excerpt):

```json
{
  "architecture": {
    "confidence": 75, "label": "Library",
    "reasoning": "Python library (pyproject.toml + `flask/__init__.py`, …)"
  },
  "symbols": [
    { "name": "register", "kind": "function",
      "path": "src/flask/sansio/blueprints.py", "line": 273,
      "visibility": "public", "decorators": ["setupmethod"] }
  ],
  "dependency_graph": { "edges": [/* … */],
                        "stats": { "internal_edges": 171 } },
  "hotspots": [ /* top 10, no body excerpts */ ],
  "project": { "file_count": 85, "symbol_count": 1622 }
}
```

A vcpkg path is wired for Windows / portable static builds; see
`CMakeLists.txt`.

## Subcommands and formats

| Command                       | Output   | Use case                                                  |
|---|---|---|
| `vectis explain`              | text     | Narrative summary for humans / LLM agents                 |
| `vectis digest --format slim` | JSON     | Token-efficient structured map for agent context          |
| `vectis digest --format json` | JSON     | Full per-file symbols, hotspot excerpts, flat `symbols[]` |
| `vectis digest --format md`   | Markdown | PR-style review output                                    |

Common flags (`--cache`, `--cache-dir`, `--output`, `-q` / `-v`)
work on both subcommands. `vectis --help` lists everything.

## Pipeline

```
  scan tree (.gitignore + built-in excludes)
        │
        ▼
  parse files (tree-sitter, 12 grammars)
        │  symbols + imports + namespaces
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
  emit digest  (slim JSON · full JSON · Markdown)
```

State persists in `<project>/vectis-data/vectis.db` (SQLite WAL +
FTS5) when `--cache` is used.

## What it isn't

No GUI. No LSP server. No embedded LLM or chat UI. No code
modification — read-only, always. No network calls during digest
production.

## License

MIT. Third-party attribution in `NOTICES.md`.
