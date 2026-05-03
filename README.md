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
  API Backend, .NET Solution, Library.
- **Cross-file dependency graph** with namespace-aware resolution
  (Java/C#/PHP via namespace index, Go via `go.mod`, Python relative
  imports against the source package).
- **Cycle detection** (Tarjan iterative SCC) and complexity-based
  **hotspot** ranking with body excerpts in the full digest.
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
./build/vectis digest /path/to/project --format slim
```

Slim output looks like (excerpt):

```json
{
  "architecture": {
    "confidence": 70,
    "label": "Layered",
    "reasoning": "found: services, core, platform"
  },
  "dependency_graph": {
    "edges": [
      { "kind": "include", "source": "src/cli/cli_main.cpp",
        "target": "src/code/code_index.h" }
    ],
    "stats": { "internal_edges": 251, "external_edges": 36 }
  },
  "hotspots": [ /* top 10, no body excerpts */ ],
  "project": { "file_count": 124, "symbol_count": 853 }
}
```

A vcpkg path is wired for Windows / portable static builds; see
`CMakeLists.txt`.

## Output formats

| Format | Size | Use case |
|---|---|---|
| `slim` | ~1–2 KB JSON | Agent context, fast orientation |
| `json` | KB–MB JSON | Full per-file symbols, hotspot excerpts, flat top-level `symbols[]` |
| `md` | KB–MB Markdown | Human reading, PR-style review |

`vectis --help` lists every option (`--cache`, `--cache-dir`,
`--output`, `-q` / `-v`).

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
