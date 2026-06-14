# Binaries

Vectis binaries are **not committed** to this repository. They are distributed as release archives or built locally.

## Getting a binary

### Option 1 — GitHub Releases

Pre-built static binaries are attached to each release on the [GitHub Releases page](https://github.com/Wintersta7e/Vectis/releases). Download the archive for your platform, extract, and place `vectis` on your `PATH`.

### Option 2 — Build from source

See the project root `README.md` for full build instructions. The short version for Linux (requires CMake 3.25+, Ninja, system apt packages):

```bash
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# Binary: build/vectis
```

For a fully static portable binary (the same as what's in releases):

```bash
cmake -B build-static -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DVECTIS_STATIC_LINK=ON
cmake --build build-static --target vectis --parallel
# Binary: build-static/vectis
```

Use `scripts/build-agent-kit.sh` to build the static binary and assemble the full agent kit tarball in one step.

## Directory layout

When a binary is present, it lives at:

```
bin/<platform>/vectis
```

Where `<platform>` is one of:

| Platform | Directory |
|---|---|
| Linux x86-64 | `bin/linux-x64/` |
| Windows x86-64 | `bin/win-x64/` (binary: `vectis.exe`) |
| macOS arm64 | `bin/macos-arm64/` |

This layout is designed so additional platform binaries drop into their own subdirectory without conflicting. The `.gitignore` at the kit root excludes all binaries.

## Verifying a release binary

SHA-256 checksums are published alongside each release on the GitHub Releases page.
