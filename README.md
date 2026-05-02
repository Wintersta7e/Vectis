# Vectis

Portable C++20 CLI that produces structured digests of source-tree
architecture — symbols, dependencies, architecture label, complexity
hotspots — for consumption by external LLM agents (Claude Code, CI
pipelines, scripts).

Personal tool. Built to fit my own workflow first; functional and
tested, no support promise.

## Languages

Python · JavaScript · TypeScript · C · C++ · Rust · Java · C# · Go ·
Ruby · PHP · SQL.

## Architecture labels

Monolith · Layered · MVC · MVVM · Clean Architecture · Monorepo ·
Frontend SPA · API Backend · .NET Solution · Library.

Each detection comes with a 0–100 confidence so consumers can decide
how much to trust the label.

## Build

System packages (Ubuntu 24.04 / WSL2):

```bash
sudo apt install -y build-essential cmake ninja-build git pkg-config \
    libsqlite3-dev libspdlog-dev libfmt-dev nlohmann-json3-dev \
    libtomlplusplus-dev libgtest-dev

cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build
```

A vcpkg path is wired for Windows / portable static builds; see
`CMakeLists.txt`.

## Usage

```bash
vectis digest /path/to/project --format slim
```

Formats: `slim` (~1–2 KB JSON, agent-optimised), `json` (full,
includes per-file symbols and hotspot excerpts), `md` (markdown).
`--cache` enables incremental hash-based rescans. `vectis --help`
lists every option.

## What it isn't

No GUI. No LSP server. No embedded LLM or chat UI. No code
modification — read-only, always. No network calls during digest
production.

## License

MIT. Third-party attribution in `NOTICES.md`.
