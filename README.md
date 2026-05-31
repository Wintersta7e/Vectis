<div align="center">

# Vectis

**Portable structured digests of source-tree architecture.**

A single-binary C++20 CLI that maps a codebase's shape — symbols,
dependencies, architecture label, complexity hotspots — into a
token-efficient digest for external LLM agents (Claude Code, CI
pipelines, scripts).

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.25%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org)
[![tree-sitter](https://img.shields.io/badge/tree--sitter-12_languages-2B7489)](https://tree-sitter.github.io/tree-sitter/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)
[![Status](https://img.shields.io/badge/status-personal%20%C2%B7%20actively%20developed-brightgreen)](#status)
[![CI](https://github.com/Wintersta7e/Vectis/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Wintersta7e/Vectis/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/Wintersta7e/Vectis/graph/badge.svg)](https://codecov.io/gh/Wintersta7e/Vectis)

</div>

---

## Why

I built Vectis for myself — a way to hand an LLM agent an accurate,
token-cheap map of an unfamiliar repo instead of letting it burn
context on blind `grep` sweeps. It reads a source tree and emits the
structure that matters: what the symbols are, how the files depend on
each other, what architecture the layout implies, and where the
complexity concentrates.

The conversational layer isn't Vectis's job — agents provide that.
Vectis's job is to feed them accurate context, entirely locally, with
**no network calls** during digest production and **no modification**
of the code it reads.

It's a personal tool, not a product — but it's open source under MIT,
and if a fast structured repo-map looks useful to you, you're welcome
to clone it and give it a try.

## Status

Actively developed personal tool. The scanner, parser, dependency
resolver, and digest exporters are implemented and covered by a large
unit + integration suite under strict warnings-as-errors
(`-Wall -Wextra -Werror -Wpedantic`, MSVC `/W4 /WX /permissive-`).
The CLI surface is stable; internal APIs can still shift between
commits.

**Implemented:**
- `vectis digest <path>` — slim / full JSON / Markdown digests
- `vectis explain <path>` — plain-text narrative summary
- Tree-sitter parsing for 12 languages
- Cross-language dependency graph with namespace-aware resolution
- Architecture detection (11 labels, 0–100 confidence)
- Cycle detection (Tarjan SCC) + complexity hotspot ranking
- `.gitignore`-aware scanning, content-hash incremental rescans
- SQLite (WAL + FTS5) cache, portable Windows static build

**Not done yet:** shared `ScanOptions` across subcommands, more
monorepo manifests (Bazel / Nx / Rush), a published binary install
path.

## What it does

- **12 languages** — Python, JavaScript, TypeScript, C, C++, Rust,
  Java, C#, Go, Ruby, PHP, SQL.
- **Manifest-file dependency graph** — module / parent / dependency
  / managed-dependency / BOM edges from Maven `pom.xml` files;
  project / package / import / solution edges from `.csproj` /
  `.fsproj` / `.vbproj` / `.sln` / `.slnx` with Central Package
  Management resolution via nearest-ancestor `Directory.Packages.props`;
  `spring-bean` / `spring-import` / `spring-component-scan` edges
  from Spring `<beans>` XML (`classpath:` resolution + Java FQCN
  candidates); `properties-include` edges from Java `.properties`
  files. Spring `applicationContext.xml` and `application.properties`
  at canonical locations raise architecture signals.
- **11 architecture labels** with 0–100 confidence — Monolith,
  Layered, MVC, MVVM, Clean Architecture, Monorepo, Frontend SPA,
  API Backend, .NET Solution, Library, Electron. The first ten are
  calibrated against a 33-project reference set; Electron is
  unit-tested but not yet on it.
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

## Stack

| Layer | Choice | Notes |
|---|---|---|
| Language | C++20 | concepts, ranges, `std::format`, structured bindings |
| Build | [CMake][cmake] 3.25+ | dual-mode `find_package`: system apt by default, vcpkg for portable builds |
| Parsing | [tree-sitter][ts] core + 12 grammars | pinned via `FetchContent`, statically bundled |
| Storage | [SQLite][sqlite] (WAL + FTS5) | prepared statements, RAII transaction guard |
| JSON | [nlohmann/json][json] | digest serialisation |
| Config | [toml++][toml] | `vectis.toml` alongside the binary |
| Logging | [spdlog][spdlog] | rotating file + stderr |
| Errors | [tl::expected][expected] | `Result<T>` over exceptions in hot paths |
| Tests | [GoogleTest][gtest] | unit + integration + fixture suites |

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
  manifest pass (pom.xml, .csproj, .sln/.slnx, .props/.targets,
                 Spring XML, .properties)
        │  + maven / csproj / sln / spring-* / properties-include edges
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

## Design principles

1. **Read-only, always.** Vectis never modifies the code it scans —
   no refactor, no autofix, no rewrite. It is a structured-data
   producer, not an editor.
2. **Local and offline.** Digest production makes no network calls.
   Everything runs from user space — no admin, no installer, copy the
   binary and go.
3. **Agent-first output.** The primary consumer is an LLM agent, not
   a human reader; the slim digest is shaped for token efficiency, and
   `explain` exists for direct reading without JSON parsing.
4. **Single binary, zero runtime deps.** Tree-sitter grammars and all
   libraries are statically bundled at build time — no interpreters,
   no shared libs, no runtime plugins.
5. **Calibrated, not guessed.** Architecture detection is measured
   against a reference corpus rather than tuned by eye, and fixture
   subtrees are pruned so deep test data can't inject false signals.

## What it isn't

No GUI. No LSP server. No embedded LLM or chat UI. No code
modification — read-only, always. No network calls during digest
production.

## License

MIT. Third-party attribution in `NOTICES.md`.

---

<sub>Vectis is a personal tool — built for my own workflow, with no
telemetry and no network calls. Not chasing adoption, but if a fast
structured repo-map is useful to you, you're welcome to try it.</sub>

[cmake]:    https://cmake.org
[ts]:       https://tree-sitter.github.io/tree-sitter/
[sqlite]:   https://www.sqlite.org
[json]:     https://github.com/nlohmann/json
[toml]:     https://marzer.github.io/tomlplusplus/
[spdlog]:   https://github.com/gabime/spdlog
[expected]: https://github.com/TartanLlama/expected
[gtest]:    https://github.com/google/googletest
