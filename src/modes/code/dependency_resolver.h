#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "modes/code/code_index.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"

namespace vectis::modes::code {

/// Per-file bundle of raw imports and namespace declarations collected
/// by the scanner during the main scan pass. The resolver consumes a
/// vector of these and turns every import into a `Dependency` edge on
/// the code index.
///
/// `declared_namespaces` feeds a namespace → file-ids map the resolver
/// builds before resolution begins, used for C# `using X.Y;` and PHP
/// `use X\Y;` statements where one using resolves to every file
/// declaring that namespace.
struct FileImports {
    std::int64_t             file_id  = 0;
    Language                 language = Language::Unknown;
    std::filesystem::path    relative_path;     ///< relative to project root
    std::vector<RawImport>   imports;
    std::vector<std::string> declared_namespaces;
};

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
///      `foo/bar/__init__.py`, try each extension.
///
/// Anything that doesn't resolve is added to the index with
/// `target_file_id = 0` and the raw import string preserved — it
/// shows up in `dependencies_of(file_id)` but not in any
/// `dependents_of(...)` result.
void resolve_all(
    CodeIndex&                             index,
    const std::filesystem::path&           project_root,
    const std::vector<FileImports>&        per_file);

} // namespace vectis::modes::code
