#!/usr/bin/env bash
# Advisory pre-push hook — emits a Vectis architecture / hotspot
# summary on every push, never blocks the push itself.
#
# Install in a downstream repo with:
#   ln -s "$(realpath scripts/hooks/pre-push.sh)" .git/hooks/pre-push
# or copy it into .git/hooks/pre-push and chmod +x.
#
# Override the binary location with VECTIS=… (defaults to PATH lookup).
set -u

VECTIS="${VECTIS:-vectis}"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
[[ -n "$ROOT" ]] || exit 0
cd "$ROOT" || exit 0

VECTIS_DIR=".vectis"
EXPLAIN_FILE="$VECTIS_DIR/pre-push-explain.txt"
ERR_FILE="$VECTIS_DIR/pre-push-vectis.err"
GITIGNORE=".gitignore"

mkdir -p "$VECTIS_DIR/cache"

# Add the artefact dir to .gitignore on first run so the user does not
# accidentally commit it. Idempotent.
if [[ -f "$GITIGNORE" ]] && ! grep -qxF "$VECTIS_DIR/" "$GITIGNORE" &&
	! grep -qxF "$VECTIS_DIR" "$GITIGNORE"; then
	printf '\n# vectis pre-push hook artefacts\n%s/\n' "$VECTIS_DIR" >>"$GITIGNORE"
elif [[ ! -f "$GITIGNORE" ]]; then
	printf '# vectis pre-push hook artefacts\n%s/\n' "$VECTIS_DIR" >"$GITIGNORE"
fi

if ! command -v "$VECTIS" >/dev/null 2>&1; then
	echo "[vectis] advisory: '$VECTIS' not on PATH; skipping snapshot."
	exit 0
fi

# `--cache` keeps the second push and beyond fast (warm scans were
# ~3x faster than cold on the smartclient case study). Errors go to a
# log file; the hook never propagates them.
"$VECTIS" explain "$ROOT" --cache --cache-dir "$VECTIS_DIR/cache" \
	>"$EXPLAIN_FILE" 2>"$ERR_FILE" || true

if [[ ! -s "$EXPLAIN_FILE" ]]; then
	echo "[vectis] advisory: explain produced no output (see $ERR_FILE)"
	exit 0
fi

echo "[vectis] advisory snapshot:"
# First block of explain is architecture + scale + languages + API
# surface; cap defensively in case the format grows.
head -n 8 "$EXPLAIN_FILE"

# Hotspots block, when present.
HOTS=$(awk '/^Top hotspots/{flag=1; next} /^$/{flag=0} flag' "$EXPLAIN_FILE")
if [[ -n "$HOTS" ]]; then
	echo
	echo "[vectis] hotspots:"
	printf '%s\n' "$HOTS" | head -n 6
fi

exit 0
