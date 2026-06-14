# Third-Party Notices

Vectis statically bundles the following third-party software. Each entry
lists the upstream project, its license, and a short summary of how
Vectis uses it. Full license texts are preserved in the sources fetched
during the CMake build (under `build/_deps/<project>-src/`) and in the
vcpkg installation tree (`build-win/vcpkg_installed/.../share/`).

## Libraries linked into the Vectis binary

| Project | License | Upstream | Role |
|---|---|---|---|
| **tree-sitter** (core C library) | MIT — © Max Brunsfeld and contributors | <https://github.com/tree-sitter/tree-sitter> | Incremental parser framework |
| **tree-sitter grammars** | MIT (most) | <https://github.com/tree-sitter/> | Language grammars for Python, JS, TS, C, C++, Rust, Java, C#, Go, Ruby, PHP, SQL. Each grammar is a separate repository; see `CMakeLists.txt` for pinned commits. |
| **SQLite** | Public Domain | <https://sqlite.org> | Persistent code index, FTS5 full-text search |
| **spdlog** | MIT — © Gabi Melman & contributors | <https://github.com/gabime/spdlog> | Logging |
| **fmt** | MIT — © Victor Zverovich and contributors | <https://github.com/fmtlib/fmt> | Formatting (used via spdlog) |
| **tomlplusplus** | MIT — © Mark Gillard | <https://github.com/marzer/tomlplusplus> | TOML config parsing |
| **nlohmann/json** | MIT — © Niels Lohmann | <https://github.com/nlohmann/json> | JSON serialization for digest export |
| **tl::expected** | CC0-1.0 — © Simon Brand | <https://github.com/TartanLlama/expected> | `Result<T>` implementation for C++20 |

## Development-time only (not linked into release binaries)

| Project | License | Upstream | Role |
|---|---|---|---|
| **GoogleTest** | BSD-3-Clause — © Google, Inc. | <https://github.com/google/googletest> | Unit test framework |

## Notes

- Vectis itself is licensed under the **MIT License** (see `LICENSE`).
- When redistributing a Vectis binary, this `NOTICES.md` file should
  accompany it, or an equivalent attribution should be provided in
  application documentation / about screens.
- The tree-sitter grammars each carry their own license inside their
  upstream repository (all MIT at time of writing). If you rebuild
  Vectis with different grammar versions, re-check their LICENSE files.
- Vectis does not statically link any GPL- or LGPL-licensed software.
