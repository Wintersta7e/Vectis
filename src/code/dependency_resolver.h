#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "code/code_index.h"
#include "code/language.h"
#include "code/parser.h"

namespace vectis::code {

/// Per-file bundle of raw imports and namespace declarations collected
/// by the scanner during the main scan pass. The resolver consumes a
/// vector of these and turns every import into a `Dependency` edge on
/// the code index.
///
/// `declared_namespaces` feeds a namespace → file-ids map the resolver
/// builds before resolution begins, used for C# `using X.Y;` and PHP
/// `use X\Y;` statements where one using resolves to every file
/// declaring that namespace.
struct FileImports
{
    std::int64_t file_id = 0;
    Language language = Language::Unknown;
    std::filesystem::path relative_path; ///< relative to project root
    std::vector<RawImport> imports;
    std::vector<std::string> declared_namespaces;
};

/// Precomputed lookup tables over a file snapshot. Two indexes:
///
///   * `by_exact_path` — full `generic_string()` of every file's
///     relative path → its `file_id`. Used for relative-path resolution
///     (`./foo` joined with the source dir), C/C++ includes that name a
///     project-root-relative path, Python dotted-name → `.py` file,
///     etc. All exact-string matches.
///
///   * `by_suffix` — every "/-boundary tail" of every file's relative
///     path → the set of matching `file_ids`, in file-insertion order.
///     Used for endswith matching (Java FQCN → `com/example/Foo.java`,
///     C/C++ `#include "core/log.h"`, Ruby `require`, PHP `use`). One
///     file contributes `depth + 1` entries, but only files of
///     languages whose resolvers actually use suffix matching are
///     indexed.
///
/// Build once with `build_path_lookup(files)` and reuse across many
/// lookups against the same snapshot — that's what turns the old
/// O(imports × files) resolver into O(imports).
struct PathLookup
{
    std::unordered_map<std::string, std::int64_t> by_exact_path;
    std::unordered_map<std::string, std::vector<std::int64_t>> by_suffix;
};

[[nodiscard]] PathLookup build_path_lookup(const std::vector<FileEntry>& files);

/// Resolve every raw import in `per_file` to a `file_id` inside
/// `index` (or mark as external) and call `index.add_dependency(...)`
/// for each one. Idempotent — safe to call multiple times on the
/// same input, though it will create duplicate edges if you do.
///
/// Resolution strategy (in priority order for each raw import):
///
///   1. **Relative path** (string starts with `./` or `../`):
///      join against the importing file's directory, then try each
///      language-appropriate extension. First hit wins.
///
///   2. **Project-root-absolute**: treat the import as a path
///      relative to the project root, try the extensions.
///
///   3. **Endswith match** (C/C++ `#include "core/log.h"` where the
///      resolved path lives a few directories up): find any file
///      whose `path_relative` ends with the raw import string.
///
///   4. **Python dotted_name**: convert `foo.bar` to `foo/bar.py` or
///      `foo/bar/__init__.py`, try each extension. For "src-layout"
///      projects (importable package under `src/`, detected by
///      stat-ing `<project_root>/src/<pkg>/__init__.py`), an absolute
///      import that misses at the root is retried with the `src/`
///      prefix prepended, so `import flask` from `tests/` resolves to
///      `src/flask/__init__.py`. Relative imports keep their own path.
///
/// Anything that doesn't resolve is added to the index with
/// `target_file_id = 0` and the raw import string preserved — it
/// shows up in `dependencies_of(file_id)` but not in any
/// `dependents_of(...)` result.
void resolve_all(CodeIndex& index, const std::filesystem::path& project_root,
                 const std::vector<FileImports>& per_file);

/// Return every Java file_id whose `path_relative` could correspond
/// to the dotted name `dotted` (e.g. `com.example.Foo`). A direct
/// path-shape match (`com/example/Foo.java`) is listed first;
/// suffix matches elsewhere in the tree follow in file-insertion
/// order. Empty vector means no candidate exists.
///
/// Two consumers:
///   * Source-language Java import resolution — picks `candidates[0]`
///     to preserve first-match-wins.
///   * Spring `<bean class="X">` (Phase 3b) — exactly one candidate
///     resolves internally; zero or many resolve as external. The
///     uniqueness rule is what disambiguates Spring beans across
///     `src/main/java` and `src/test/java` roots without us reading
///     `pom.xml` / `build.gradle`.
///
/// The `PathLookup` overload is the hot path — Spring's bean loop and
/// the resolver itself amortize one `build_path_lookup` over many
/// queries. The `std::vector<FileEntry>` overload is a one-shot
/// convenience for tests and ad-hoc callers; it allocates a fresh
/// lookup internally.
[[nodiscard]] std::vector<std::int64_t> match_java_dotted_candidates(const PathLookup& lookup,
                                                                     std::string_view dotted);

[[nodiscard]] std::vector<std::int64_t>
match_java_dotted_candidates(const std::vector<FileEntry>& files, std::string_view dotted);

} // namespace vectis::code
