#!/usr/bin/env bash
# build-agent-kit.sh — Build the static Vectis binary, regenerate the sample
# digest, and assemble a versioned agent-kit tarball.
#
# Usage: scripts/build-agent-kit.sh [--version <semver>]
#
# Outputs:
#   dist/agent-kit/bin/linux-x64/vectis
#   dist/agent-kit/examples/sample-digest.slim.json
#   vectis-agent-kit-<version>-linux-x64.tar.gz   (next to repo root)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KIT_DIR="$REPO_ROOT/dist/agent-kit"
FIXTURE_PATH="$REPO_ROOT/tests/fixtures/code/sample-python"

# ── parse args ───────────────────────────────────────────────────────────────
VERSION=""
while [[ $# -gt 0 ]]; do
	case "$1" in
	--version)
		VERSION="$2"
		shift 2
		;;
	*)
		echo "Unknown argument: $1" >&2
		exit 1
		;;
	esac
done

# ── build static release binary ──────────────────────────────────────────────
echo "==> Configuring static release build …"
cmake -B "$REPO_ROOT/build-static" -S "$REPO_ROOT" \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DVECTIS_STATIC_LINK=ON

echo "==> Building vectis …"
cmake --build "$REPO_ROOT/build-static" --target vectis --parallel

BINARY="$REPO_ROOT/build-static/vectis"
if [[ ! -x "$BINARY" ]]; then
	echo "ERROR: binary not found at $BINARY" >&2
	exit 2
fi

# ── resolve version ───────────────────────────────────────────────────────────
if [[ -z "$VERSION" ]]; then
	# `vectis --version` is the source of truth; fail loudly rather than
	# silently shipping a wrong version. Capture separately from grep so a
	# nonzero exit from the binary is not masked by the pipeline.
	if ! version_output="$("$BINARY" --version)"; then
		echo "ERROR: '$BINARY --version' failed; pass --version <semver>." >&2
		exit 2
	fi
	VERSION="$(printf '%s\n' "$version_output" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)" || true
	if [[ -z "$VERSION" ]]; then
		echo "ERROR: no version detected in '$BINARY --version' output." >&2
		echo "       Pass --version <semver> explicitly." >&2
		exit 2
	fi
fi
echo "==> Version: $VERSION"

# ── install binary into kit ───────────────────────────────────────────────────
BIN_OUT="$KIT_DIR/bin/linux-x64"
mkdir -p "$BIN_OUT"
cp "$BINARY" "$BIN_OUT/vectis"
chmod 755 "$BIN_OUT/vectis"
echo "==> Binary installed to $BIN_OUT/vectis"

# ── regenerate sample digest ──────────────────────────────────────────────────
SAMPLE_OUT="$KIT_DIR/examples/sample-digest.slim.json"
echo "==> Regenerating sample digest from fixture …"
"$BIN_OUT/vectis" digest "$FIXTURE_PATH" --format slim --output "$SAMPLE_OUT" -q
# Sanitise the local root path so the sample stays generic in the committed file.
ESCAPED_ROOT=$(printf '%s\n' "$FIXTURE_PATH" | sed 's/[\/&]/\\&/g')
sed -i "s|\"root\":\"$ESCAPED_ROOT\"|\"root\":\"<project-path>\"|g" "$SAMPLE_OUT"
echo "==> Sample digest written to $SAMPLE_OUT"

# ── regenerate agent skill from the binary's own guide ────────────────────────
# Single source of truth: `vectis guide`. The AgentKitSkill unit test fails if
# the committed SKILL.md body drifts from this output.
SKILL_OUT="$KIT_DIR/skill/vectis/SKILL.md"
echo "==> Regenerating agent skill …"
{
	printf -- '---\n'
	printf 'name: vectis\n'
	printf 'description: %s\n' "Generate a structured digest map of a codebase (symbols, dependency graph, architecture label, hotspots) using the vectis CLI, for agent orientation or context injection."
	printf -- '---\n\n'
	"$BIN_OUT/vectis" guide
} >"$SKILL_OUT"
echo "==> Agent skill written to $SKILL_OUT"

# ── assemble tarball ──────────────────────────────────────────────────────────
TARBALL="$REPO_ROOT/vectis-agent-kit-${VERSION}-linux-x64.tar.gz"
echo "==> Assembling tarball …"
tar -czf "$TARBALL" \
	--exclude='bin/**/vectis.exe' \
	-C "$REPO_ROOT" \
	dist/agent-kit

echo "==> Done: $TARBALL"
echo ""
echo "To verify: tar -tzf $TARBALL | head -20"
