# Vectis Agent Kit

**A single-binary tool that produces structured digests of source-tree architecture for LLM agents and CI pipelines.**

Vectis reads a source tree and emits a token-efficient, machine-readable map: symbols, cross-file dependency graph, architecture label, and complexity hotspots. The output is JSON designed for external agents — something they can consume once and use to orient themselves in an unfamiliar repo without burning context on blind grep sweeps.

No network calls are made during digest production. Vectis never modifies the code it reads.

---

## Feeding this to an agent

Zero setup — nothing to copy, nothing to register. Point your agent at the binary's own guide:

```bash
vectis guide      # prints a complete, framework-neutral usage guide to stdout
```

Tell your agent to run `vectis guide`, or pipe its output into the agent's context. For MCP-capable agents, register `vectis mcp` as a server instead (see `docs/CLI.md`). `skill/vectis/SKILL.md` in this kit is the same guide with Claude-Code frontmatter, ready to drop into `.claude/skills/`.

---

## 60-second quickstart

Assuming you have the `vectis` binary on your PATH:

```bash
# Narrative plain-text summary (good first look)
vectis explain /path/to/your-project

# Token-efficient slim JSON digest (agent consumption)
vectis digest /path/to/your-project --format slim --output -

# Full JSON with per-file symbols and hotspot body excerpts
vectis digest /path/to/your-project --format json --output digest.json

# Cached incremental digest (fast on re-runs)
vectis digest /path/to/your-project --cache

# Agent integration guide (when/how to use vectis) — feed this to any agent
vectis guide

# MCP server (for any MCP-capable client)
vectis mcp
```

See `docs/CLI.md` for the full command reference, `docs/OUTPUT.md` for the output schema, and `examples/sample-digest.slim.json` for a concrete sample.

---

## When to use Vectis

**Good fit:**

- Giving an LLM agent a structured overview of a repo before asking architecture or refactoring questions.
- CI step that detects architecture drift, new dependency cycles, or hotspot regressions between commits (`scripts/vectis-diff`).
- Quick orientation pass on an unfamiliar codebase before opening any files.
- MCP integration with any MCP-capable client: register `vectis mcp` as a server. Claude Code users can also drop `skill/vectis/SKILL.md` into `.claude/skills/`.

**Not a fit:**

- You want prompt templating or markdown-rendered file dumps (see `code2prompt`, `repomix`).
- You need a language server / live diagnostics (Vectis is a one-shot batch tool, not an LSP).
- You want to search or chat over code in a web UI.
- You need to modify or refactor code — Vectis is read-only, always.

---

## What's in this kit

```
dist/agent-kit/
  README.md                   This file
  LICENSE                     MIT license
  NOTICES.md                  Third-party attribution
  .gitignore                  Excludes platform binaries
  bin/README.md               How to get / build the binary
  docs/CLI.md                 Full CLI reference
  docs/OUTPUT.md              Digest JSON schema reference
  skill/vectis/SKILL.md       Claude Code skill template
  examples/sample-digest.slim.json   Concrete slim JSON example
```

---

## Languages supported

Python · JavaScript · TypeScript · C · C++ · Rust · Java · C# · Go · Ruby · PHP · SQL

Dependency resolution is calibrated per language against real corpora; see `docs/OUTPUT.md → fidelity_metadata`.

---

## What makes Vectis different from similar tools

| Feature | Vectis | repomix / code2prompt | tree-sitter-cli | GitHub Semantic |
|---|---|---|---|---|
| Cross-language dependency graph | Yes | No | No | Partial |
| Namespace-aware resolution (C#, Java, PHP, Go, Python) | Yes | — | — | Partial |
| Architecture pattern detection | Yes | No | No | No |
| Calibrated per-edge fidelity model | Yes | — | — | No |
| Complexity hotspot ranking | Yes | No | No | No |
| Single static binary, zero runtime deps | Yes | No (Node) | No (Node) | No (JVM) |
| Incremental cached rescans | Yes | No | No | No |
| MCP server | Yes | No | No | No |

---

## License

MIT. See `LICENSE` and `NOTICES.md` for third-party attribution.
