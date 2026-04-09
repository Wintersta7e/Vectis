#include "modes/code/dependency_resolver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "modes/code/code_index.h"
#include "modes/code/dependency.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"
#include "modes/code/symbol.h"

namespace vectis::modes::code {

namespace {

/// Extension candidates to try, in priority order, when resolving an
/// import that has no extension (e.g. TypeScript `./foo`).
[[nodiscard]] std::vector<std::string_view>
candidate_extensions(Language language) noexcept
{
    switch (language) {
        case Language::TypeScript: return {".ts", ".tsx", ".d.ts", ".js"};
        case Language::JavaScript: return {".js", ".mjs", ".cjs", ".jsx"};
        case Language::Python:     return {".py", ".pyi"};
        case Language::Cpp:        return {".hpp", ".h", ".hh", ".hxx", ".cpp", ".cxx", ".cc"};
        case Language::C:          return {".h", ".c"};
        case Language::Rust:       return {".rs"};
        default:                   return {};
    }
}

/// Lexical normalization without touching the filesystem: collapse
/// `./` and `../` segments in a relative path.
[[nodiscard]] std::filesystem::path
normalize_relative(const std::filesystem::path& in)
{
    std::filesystem::path out;
    for (const auto& segment : in) {
        const std::string s = segment.string();
        if (s == "." || s.empty()) {
            continue;
        }
        if (s == "..") {
            if (!out.empty() && out.filename().string() != "..") {
                out.remove_filename();
                // remove_filename leaves a trailing separator; walk up further
                if (!out.empty() && out.string().back() == '/') {
                    // Remove trailing slash for consistency
                    auto str = out.string();
                    if (str.size() > 1 && str.back() == '/') {
                        str.pop_back();
                        out = str;
                    }
                }
            } else {
                out /= "..";
            }
        } else {
            out /= s;
        }
    }
    return out;
}

/// True if `path_relative` equals `candidate_rel` (or `candidate_rel`
/// with a trailing `/index.<ext>` variant). Simple string match.
[[nodiscard]] bool path_equals(
    const std::filesystem::path& path_relative,
    const std::filesystem::path& candidate_rel) noexcept
{
    return path_relative.generic_string() == candidate_rel.generic_string();
}

/// True if a file's relative path ends with the given suffix. Used
/// for the "find any file whose path ends with the raw include"
/// fallback (step 3 of the resolution strategy).
[[nodiscard]] bool path_ends_with(
    const std::filesystem::path& path_relative,
    std::string_view             suffix) noexcept
{
    const std::string full = path_relative.generic_string();
    if (suffix.size() > full.size()) {
        return false;
    }
    // Either an exact-at-end match, or preceded by a path separator
    // so we don't accidentally match "bar.h" for "foobar.h".
    const std::size_t offset = full.size() - suffix.size();
    if (full.compare(offset, suffix.size(), suffix) != 0) {
        return false;
    }
    return offset == 0 || full[offset - 1] == '/';
}

/// Try to match `candidate_stem` (a stem like "foo/bar" with no
/// extension) against every file in the index using each extension
/// from `extensions`. Returns the matching file_id or 0.
[[nodiscard]] std::int64_t match_by_extension(
    const std::vector<FileEntry>& files,
    const std::filesystem::path&  candidate_stem,
    const std::vector<std::string_view>& extensions)
{
    for (const std::string_view ext : extensions) {
        std::filesystem::path candidate = candidate_stem;
        candidate += std::string{ext};
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, candidate)) {
                return file.id;
            }
        }

        // Also try `<stem>/index<ext>` for JS/TS style barrel files.
        candidate = candidate_stem / (std::string{"index"} + std::string{ext});
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, candidate)) {
                return file.id;
            }
        }
    }
    return 0;
}

/// Python-specific: convert `foo.bar` to `foo/bar` and try `.py`/.pyi,
/// or `foo/bar/__init__.py`.
[[nodiscard]] std::int64_t match_python_dotted(
    const std::vector<FileEntry>& files,
    std::string_view              dotted)
{
    std::filesystem::path stem;
    std::size_t start = 0;
    while (start < dotted.size()) {
        const std::size_t dot = dotted.find('.', start);
        const std::string segment{
            dotted.substr(start, dot == std::string_view::npos ? dot : dot - start)};
        if (!segment.empty() && segment != ".") {
            stem /= segment;
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    static const std::vector<std::string_view> k_py_ext = {".py", ".pyi"};
    const std::int64_t direct = match_by_extension(files, stem, k_py_ext);
    if (direct != 0) {
        return direct;
    }
    // `foo/bar/__init__.py`
    for (const std::string_view ext : k_py_ext) {
        const std::filesystem::path init_path = stem / (std::string{"__init__"} + std::string{ext});
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, init_path)) {
                return file.id;
            }
        }
    }
    return 0;
}

/// Resolve one raw import to a file_id in the index, returning 0 if
/// the import is external or can't be found.
[[nodiscard]] std::int64_t resolve_one(
    const FileImports&            source,
    const RawImport&              raw,
    const std::vector<FileEntry>& files)
{
    const std::vector<std::string_view> exts = candidate_extensions(source.language);

    // --- 1. Relative path (./ or ../) ------------------------------
    const bool is_relative_prefix =
        raw.import_string.rfind("./", 0) == 0 ||
        raw.import_string.rfind("../", 0) == 0;

    if (is_relative_prefix && !exts.empty()) {
        const std::filesystem::path source_dir = source.relative_path.parent_path();
        const std::filesystem::path joined     = normalize_relative(
            source_dir / raw.import_string);

        // If the import already carries an extension, try exact match.
        if (!std::filesystem::path{raw.import_string}.extension().empty()) {
            for (const FileEntry& file : files) {
                if (path_equals(file.path_relative, joined)) {
                    return file.id;
                }
            }
        }
        // Otherwise try each candidate extension.
        const std::int64_t hit = match_by_extension(files, joined, exts);
        if (hit != 0) {
            return hit;
        }
    }

    // --- 2. C/C++ include path with an extension already in place --
    // #include "core/log.h" where the raw string IS a relative path.
    if ((source.language == Language::Cpp || source.language == Language::C) &&
        !std::filesystem::path{raw.import_string}.extension().empty())
    {
        // Try project-root-absolute first.
        const std::filesystem::path as_project_root{raw.import_string};
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, as_project_root)) {
                return file.id;
            }
        }
        // Then try relative to the source file's directory.
        const std::filesystem::path joined = normalize_relative(
            source.relative_path.parent_path() / raw.import_string);
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, joined)) {
                return file.id;
            }
        }
        // Then an endswith fallback — catches `#include "core/log.h"`
        // when the actual file is at `src/core/log.h` and we don't
        // know the include-path root.
        for (const FileEntry& file : files) {
            if (path_ends_with(file.path_relative, raw.import_string)) {
                return file.id;
            }
        }
    }

    // --- 3. Python dotted name -------------------------------------
    if (source.language == Language::Python) {
        return match_python_dotted(files, raw.import_string);
    }

    // --- 4. TS/JS bare module name (no relative prefix) ------------
    // These are almost always npm packages; treat as external.
    // (No resolution attempted.)

    // --- 5. Rust use / mod -----------------------------------------
    if (source.language == Language::Rust) {
        // `mod foo;` resolves to foo.rs in the same directory.
        if (raw.kind == "mod") {
            const std::filesystem::path source_dir = source.relative_path.parent_path();
            const std::filesystem::path stem = source_dir / raw.import_string;
            return match_by_extension(files, stem, {".rs"});
        }
        // `use x::y::z;` — we don't resolve Rust crate-style paths.
    }

    return 0;  // external / unresolved
}

} // namespace

void resolve_all(CodeIndex&                       index,
                 const std::filesystem::path&     /*project_root*/,
                 const std::vector<FileImports>&  per_file)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    std::size_t resolved_count = 0;
    std::size_t external_count = 0;

    for (const FileImports& source : per_file) {
        for (const RawImport& raw : source.imports) {
            Dependency dep;
            dep.source_file_id = source.file_id;
            dep.target_file_id = resolve_one(source, raw, files);
            dep.import_string  = raw.import_string;
            dep.kind           = raw.kind;

            if (dep.target_file_id != 0) {
                ++resolved_count;
            } else {
                ++external_count;
            }
            index.add_dependency(std::move(dep));
        }
    }

    VECTIS_LOG_INFO(
        "Dependency resolution complete: {} resolved, {} external",
        resolved_count, external_count);
}

} // namespace vectis::modes::code
