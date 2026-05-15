#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace vectis::code::deps {

/// Lightweight direct-dependency-name extraction from runtime manifests.
///
/// Each extractor reads one manifest file and returns the sorted, deduped
/// set of direct dependency names — no version strings, no transitive
/// resolution. Names are returned verbatim (e.g. `"react"`, `"@types/node"`,
/// `"org.springframework.boot:spring-boot-starter-web"`) so downstream
/// matchers can choose how strict to be about scopes / group-ids.
///
/// These functions are **not** a replacement for the manifest scanner's
/// graph-edge emission (which currently covers Java/Maven + .NET). They
/// exist to feed the architecture detector's framework-hint matcher,
/// which needs dep names for ecosystems the graph pass hasn't yet
/// touched (npm / pyproject / Cargo / go.mod / composer / Gemfile).
///
/// Failure mode: unreadable or malformed files return an empty vector
/// rather than an error. A best-effort hint pipeline shouldn't fail a
/// digest over a typo in someone's `Gemfile`.

/// Read `package.json` and return the deduped union of `dependencies`,
/// `devDependencies`, `peerDependencies`, and `optionalDependencies`
/// keys. Tolerates JSON-with-comments (some toolchains generate it).
[[nodiscard]] std::vector<std::string> extract_npm(const std::filesystem::path& package_json);

/// Read `pyproject.toml` and return the deduped union of dependency
/// names from PEP 621 `[project] dependencies` /
/// `optional-dependencies` AND Poetry's
/// `[tool.poetry.dependencies]` / `dev-dependencies` /
/// `group.<name>.dependencies`. PEP 508 specifiers
/// (`django>=4.0; python_version >= '3.8'`) are stripped down to the
/// package name. Poetry's `python` entry is excluded — it pins the
/// interpreter, not a dependency.
[[nodiscard]] std::vector<std::string>
extract_pyproject(const std::filesystem::path& pyproject_toml);

/// Best-effort dep extraction from `setup.py`. Scans for a literal
/// `install_requires=[...]` (or `requires=[...]`) assignment and
/// pulls each quoted PEP 508 spec out. Dynamic forms
/// (`install_requires=parse_requirements(...)`,
/// `install_requires=base + extras`) return empty — that's the cost
/// of not embedding a Python interpreter.
[[nodiscard]] std::vector<std::string> extract_setup_py(const std::filesystem::path& setup_py);

/// Read `Cargo.toml` and return the deduped union of keys from
/// `[dependencies]`, `[dev-dependencies]`, and `[build-dependencies]`.
/// Workspace deps (`{ workspace = true }`), path deps, git deps, and
/// feature tables are all flattened to the crate name — that's all
/// the framework matcher needs.
[[nodiscard]] std::vector<std::string> extract_cargo(const std::filesystem::path& cargo_toml);

/// Read `go.mod` and return the deduped set of module paths from
/// `require ( ... )` blocks and single-line `require <path> <version>`
/// statements. Module paths are returned verbatim (e.g.
/// `"github.com/gin-gonic/gin"`) — the matcher decides how to handle
/// hostname-prefixed names. `// indirect` comments are ignored.
[[nodiscard]] std::vector<std::string> extract_go_mod(const std::filesystem::path& go_mod);

/// Read `composer.json` and return the deduped union of `require` and
/// `require-dev` keys. PHP package names use the vendor/package shape
/// (e.g. `symfony/console`) and are returned verbatim.
[[nodiscard]] std::vector<std::string> extract_composer(const std::filesystem::path& composer_json);

/// Best-effort Gemfile parser. Scans for `gem '<name>'` / `gem "<name>"`
/// statements; the first quoted argument after `gem` is the gem name.
/// Group/source blocks are not interpreted — every gem found in the
/// file contributes, regardless of nesting. This matches the matcher's
/// use case (any framework gem is a hint, regardless of which group).
[[nodiscard]] std::vector<std::string> extract_gemfile(const std::filesystem::path& gemfile);

} // namespace vectis::code::deps
