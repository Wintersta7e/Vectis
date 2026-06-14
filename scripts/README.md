# scripts/

Helper tools layered on top of the `vectis` binary. They are **not**
needed to use Vectis — the CLI is fully functional on its own. These
exist as starting points for downstream workflows that we want to
keep loose (shell + jq) until the patterns stabilise enough to
justify a binary subcommand.

## `vectis-diff`

Compare two slim digests and print added / removed files,
architecture-label changes, scale deltas, hotspot churn (newly
entered top 10, dropped, severity changes), and central-file churn.

```bash
vectis digest . --format slim --output baseline.slim.json
# … work …
vectis digest . --format slim --output current.slim.json
scripts/vectis-diff baseline.slim.json current.slim.json
```

`--json` emits a machine-readable object with the same delta data.

Requires `bash` and `jq`.

## `build-agent-kit.sh`

Build the static Vectis release binary, regenerate `examples/sample-digest.slim.json`,
and assemble a versioned agent-kit tarball (`vectis-agent-kit-<version>-linux-x64.tar.gz`
next to the repo root).

```bash
scripts/build-agent-kit.sh               # version from binary --version
scripts/build-agent-kit.sh --version 1.2.3
```

Requires CMake 3.25+, Ninja, and the system apt packages listed in `docs/BUILD.md`.

## `hooks/pre-push.sh`

Advisory pre-push hook for downstream repos that want a "what does
the architecture / hotspot picture look like before I push" snapshot
on every `git push`. Never blocks the push; logs to `.vectis/`.

Install in a downstream repo with one of:

```bash
# Symlink (preferred — picks up edits to the source of truth):
ln -s "$(pwd)/scripts/hooks/pre-push.sh" /path/to/other-repo/.git/hooks/pre-push

# Or copy:
cp scripts/hooks/pre-push.sh /path/to/other-repo/.git/hooks/pre-push
chmod +x /path/to/other-repo/.git/hooks/pre-push
```

The hook expects `vectis` on `PATH`. Override with the `VECTIS`
env var if the binary lives elsewhere.

It auto-adds `.vectis/` to the downstream repo's `.gitignore` on
first run so the artefacts are not accidentally committed.
