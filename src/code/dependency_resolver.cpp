#include "code/dependency_resolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"

namespace vectis::code {

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
        case Language::Java:       return {".java"};
        case Language::Ruby:       return {".rb"};
        case Language::Php:        return {".php"};
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

/// Split a dotted name like `foo.bar.Baz` into path segments `foo/bar/Baz`.
/// Empty segments (leading dot, consecutive dots) are skipped.
[[nodiscard]] std::filesystem::path
split_dotted(std::string_view dotted, char separator)
{
    std::filesystem::path stem;
    std::size_t start = 0;
    while (start < dotted.size()) {
        const std::size_t sep = dotted.find(separator, start);
        const std::string segment{
            dotted.substr(start, sep == std::string_view::npos ? sep : sep - start)};
        if (!segment.empty() && segment != ".") {
            stem /= segment;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        start = sep + 1;
    }
    return stem;
}

/// Look up a Python module rooted at `<stem>` — first as a `.py` /
/// `.pyi` file, then as a `<stem>/__init__.py(i)` package marker.
[[nodiscard]] std::int64_t match_python_module(
    const std::vector<FileEntry>& files,
    const std::filesystem::path&  stem)
{
    static const std::vector<std::string_view> k_py_ext = {".py", ".pyi"};
    if (const std::int64_t direct = match_by_extension(files, stem, k_py_ext);
        direct != 0) {
        return direct;
    }
    for (const std::string_view ext : k_py_ext) {
        const std::filesystem::path init_path =
            stem / (std::string{"__init__"} + std::string{ext});
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, init_path)) {
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
    return match_python_module(files, split_dotted(dotted, '.'));
}

/// Python relative imports: `from .x import y`, `from ..pkg.mod import z`,
/// `from . import x`. Tree-sitter's `relative_import` node captures the
/// entire pattern — dots and the optional dotted suffix — as one text
/// span, which arrives here as e.g. `".x"`, `"..pkg.mod"`, `"."`.
///
/// Semantics (PEP 328):
///   - 1 dot  = the current package (source file's directory)
///   - 2 dots = the parent package
///   - n dots = walk up (n-1) levels from source's directory
///
/// Without this, every `from ..something import x` is classified
/// external, and real Python projects — which lean heavily on relative
/// imports for package-internal references — end up with
/// `internal_edges: 0` despite having hundreds of intra-project
/// imports.
[[nodiscard]] std::int64_t match_python_relative(
    const std::vector<FileEntry>& files,
    const std::filesystem::path&  source_relative_path,
    std::string_view              with_dots)
{
    std::size_t n_dots = 0;
    while (n_dots < with_dots.size() && with_dots[n_dots] == '.') {
        ++n_dots;
    }
    const std::string_view remainder = with_dots.substr(n_dots);

    // Walk up (n_dots - 1) levels from source's directory. Bail if we
    // walk above the project root — the import refers to something
    // outside the scan.
    std::filesystem::path base = source_relative_path.parent_path();
    for (std::size_t i = 1; i < n_dots; ++i) {
        if (base.empty() || base == base.parent_path()) {
            return 0;
        }
        base = base.parent_path();
    }

    std::filesystem::path stem = base;
    if (!remainder.empty()) {
        stem /= split_dotted(remainder, '.');
    }
    return match_python_module(files, stem);
}

/// Java-specific: `foo.bar.Baz` → try `foo/bar/Baz.java` directly, then
/// fall back to an endswith match so projects rooted under `src/main/java`
/// still resolve without us parsing build descriptors.
[[nodiscard]] std::int64_t match_java_dotted(
    const std::vector<FileEntry>& files,
    std::string_view              dotted)
{
    const std::filesystem::path stem = split_dotted(dotted, '.');
    static const std::vector<std::string_view> k_java_ext = {".java"};
    const std::int64_t direct = match_by_extension(files, stem, k_java_ext);
    if (direct != 0) {
        return direct;
    }
    const std::string suffix = stem.generic_string() + ".java";
    for (const FileEntry& file : files) {
        if (path_ends_with(file.path_relative, suffix)) {
            return file.id;
        }
    }
    return 0;
}


/// PHP-specific: `Slim\Factory\Foo` → try `Slim/Factory/Foo.php`
/// directly, then fall back to a suffix match so projects rooted
/// under `src/` (the PSR-4 default) still resolve without us reading
/// `composer.json`.
[[nodiscard]] std::int64_t match_php_namespaced(
    const std::vector<FileEntry>& files,
    std::string_view              namespaced)
{
    const std::filesystem::path stem = split_dotted(namespaced, '\\');
    static const std::vector<std::string_view> k_php_ext = {".php"};
    const std::int64_t direct = match_by_extension(files, stem, k_php_ext);
    if (direct != 0) {
        return direct;
    }
    const std::string suffix = stem.generic_string() + ".php";
    for (const FileEntry& file : files) {
        if (path_ends_with(file.path_relative, suffix)) {
            return file.id;
        }
    }
    return 0;
}

/// Shared resolver context built once per `resolve_all` call, carrying
/// the namespace-to-files index for C#/PHP resolution and the Go module
/// prefix (if a `go.mod` was found at the project root). Packing these
/// into one argument keeps `resolve_one`'s signature from drifting
/// every time a new language-specific hint arrives.
struct ResolveCtx {
    const std::vector<FileEntry>&                                   files;
    const std::unordered_map<std::string, std::vector<std::int64_t>>& namespace_to_files;
    std::string                                                     go_module_prefix; ///< empty if no go.mod
};

/// Read the first `module <path>` line out of a Go `go.mod` file. Only
/// looks at the project root — monorepos with per-subdir go.mods aren't
/// handled here, which is consistent with how we treat other config
/// files (Cargo.toml, tsconfig.json) today.
[[nodiscard]] std::string
read_go_module_prefix(const std::filesystem::path& project_root)
{
    const std::filesystem::path mod_path = project_root / "go.mod";
    std::error_code ec;
    if (!std::filesystem::exists(mod_path, ec) || ec) {
        return {};
    }
    std::ifstream in(mod_path);
    if (!in) {
        return {};
    }
    std::string line;
    while (std::getline(in, line)) {
        // Strip leading whitespace — Go's formatter keeps things tidy
        // but handwritten go.mod files sometimes indent.
        std::size_t start = 0;
        while (start < line.size() &&
               std::isspace(static_cast<unsigned char>(line[start])))
        {
            ++start;
        }
        constexpr std::string_view k_prefix = "module ";
        if (line.compare(start, k_prefix.size(), k_prefix) == 0) {
            std::string path = line.substr(start + k_prefix.size());
            // Trim trailing comment / whitespace / quotes.
            while (!path.empty() &&
                   (std::isspace(static_cast<unsigned char>(path.back())) ||
                    path.back() == '"' || path.back() == '\''))
            {
                path.pop_back();
            }
            // Drop leading quote if the module path was quoted.
            if (!path.empty() && (path.front() == '"' || path.front() == '\'')) {
                path.erase(0, 1);
            }
            return path;
        }
    }
    return {};
}

/// Build the `namespace-string → [file_id, ...]` index. A single
/// namespace can be declared by many files (one `SampleApp.Models`
/// namespace typically contains several `.cs` files).
[[nodiscard]] std::unordered_map<std::string, std::vector<std::int64_t>>
build_namespace_index(const std::vector<FileImports>& per_file)
{
    std::unordered_map<std::string, std::vector<std::int64_t>> out;
    for (const FileImports& source : per_file) {
        for (const std::string& ns : source.declared_namespaces) {
            out[ns].push_back(source.file_id);
        }
    }
    return out;
}

/// Resolve one raw import to a set of file_ids in the index. Most
/// imports resolve to at most one file (returned as a single-element
/// vector). C# `using X.Y;` and Go `import "x/y"` are different — a
/// namespace or package maps to every file declaring it, so the
/// result can be a vector of several ids. An empty vector means the
/// import is external / unresolved, and a single external edge is
/// recorded with `target_file_id = 0`.
[[nodiscard]] std::vector<std::int64_t> resolve_one(
    const FileImports&  source,
    const RawImport&    raw,
    const ResolveCtx&   ctx)
{
    const std::vector<FileEntry>& files = ctx.files;
    const std::vector<std::string_view> exts = candidate_extensions(source.language);

    // --- 1. Relative path (./ or ../) ------------------------------
    const bool is_relative_prefix =
        raw.import_string.rfind("./", 0) == 0 ||
        raw.import_string.rfind("../", 0) == 0;

    if (is_relative_prefix && !exts.empty()) {
        const std::filesystem::path source_dir = source.relative_path.parent_path();
        const std::filesystem::path joined     = normalize_relative(
            source_dir / raw.import_string);

        if (!std::filesystem::path{raw.import_string}.extension().empty()) {
            for (const FileEntry& file : files) {
                if (path_equals(file.path_relative, joined)) {
                    return {file.id};
                }
            }
        }
        const std::int64_t hit = match_by_extension(files, joined, exts);
        if (hit != 0) {
            return {hit};
        }
    }

    // --- 2. C/C++ include path with an extension already in place --
    if ((source.language == Language::Cpp || source.language == Language::C) &&
        !std::filesystem::path{raw.import_string}.extension().empty())
    {
        const std::filesystem::path as_project_root{raw.import_string};
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, as_project_root)) {
                return {file.id};
            }
        }
        const std::filesystem::path joined = normalize_relative(
            source.relative_path.parent_path() / raw.import_string);
        for (const FileEntry& file : files) {
            if (path_equals(file.path_relative, joined)) {
                return {file.id};
            }
        }
        for (const FileEntry& file : files) {
            if (path_ends_with(file.path_relative, raw.import_string)) {
                return {file.id};
            }
        }
    }

    // --- 3. Python dotted name or relative import ------------------
    // Relative imports (leading `.`) resolve against the source file's
    // package; falling through to dotted-name resolution would wrongly
    // match a top-level module with the same basename (`..models` →
    // `models.py` at repo root rather than the parent package's
    // `models.py`), so relative imports go through their own path.
    if (source.language == Language::Python) {
        if (!raw.import_string.empty() && raw.import_string.front() == '.') {
            const std::int64_t hit = match_python_relative(
                files, source.relative_path, raw.import_string);
            return hit != 0 ? std::vector<std::int64_t>{hit}
                            : std::vector<std::int64_t>{};
        }
        const std::int64_t hit = match_python_dotted(files, raw.import_string);
        return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
    }

    // --- 4. TS/JS bare module name — external, no resolution.

    // --- 5. Rust use / mod -----------------------------------------
    if (source.language == Language::Rust) {
        if (raw.kind == "mod") {
            const std::filesystem::path source_dir = source.relative_path.parent_path();
            const std::filesystem::path stem = source_dir / raw.import_string;
            const std::int64_t hit = match_by_extension(files, stem, {".rs"});
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
        // `use x::y::z;` — Rust crate-style paths not resolved.
    }

    // --- 6. Java import --------------------------------------------
    // Specific imports (`import com.foo.Bar;`) hit the path-based
    // matcher; wildcards (`import com.foo.*;`) strip the asterisk at
    // capture time and so look like a bare package name here. When
    // the path match fails we fall back to the namespace index —
    // if `com.foo` is a known package, emit one edge per file in it.
    if (source.language == Language::Java) {
        const std::int64_t hit = match_java_dotted(files, raw.import_string);
        if (hit != 0) {
            return {hit};
        }
        const auto it = ctx.namespace_to_files.find(raw.import_string);
        if (it != ctx.namespace_to_files.end()) {
            return it->second;
        }
        return {};
    }

    // --- 7. Ruby require / require_relative ------------------------
    // `require_relative './foo'` resolves against the source file's
    // directory; `require 'sinatra/base'` is load-path style — neither
    // necessarily lives under the source's parent directory. The
    // suffix fallback uses the bare import string so a test file can
    // still resolve `require 'lib/x'` to `<root>/lib/x.rb`.
    if (source.language == Language::Ruby) {
        const std::filesystem::path source_dir = source.relative_path.parent_path();
        const std::filesystem::path stem = normalize_relative(
            source_dir / raw.import_string);
        const std::int64_t hit = match_by_extension(files, stem, {".rb"});
        if (hit != 0) {
            return {hit};
        }
        std::string suffix{raw.import_string};
        if (!suffix.ends_with(".rb")) {
            suffix += ".rb";
        }
        for (const FileEntry& file : files) {
            if (path_ends_with(file.path_relative, suffix)) {
                return {file.id};
            }
        }
    }

    // --- 8. PHP require / include / use ----------------------------
    if (source.language == Language::Php) {
        if (raw.kind == "require" || raw.kind == "include") {
            std::string path_str{raw.import_string};
            if (!path_str.empty() && path_str.front() == '/') {
                path_str.erase(0, 1);
            }
            const std::filesystem::path source_dir = source.relative_path.parent_path();
            const std::filesystem::path joined     = normalize_relative(
                source_dir / path_str);

            if (!std::filesystem::path{raw.import_string}.extension().empty()) {
                for (const FileEntry& file : files) {
                    if (path_equals(file.path_relative, joined)) {
                        return {file.id};
                    }
                }
                for (const FileEntry& file : files) {
                    if (path_ends_with(file.path_relative, path_str)) {
                        return {file.id};
                    }
                }
            }
            const std::int64_t hit = match_by_extension(files, joined, {".php"});
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
        if (raw.kind == "use") {
            // PSR-4 path-style match: `Slim\Factory\Foo` → `Slim/Factory/Foo.php`,
            // either at the project root or nested under `src/`. This is the
            // common case — a `use` clause names a class, not a namespace.
            const std::int64_t hit = match_php_namespaced(files, raw.import_string);
            if (hit != 0) {
                return {hit};
            }
            // Fallback: strip the trailing class component and look up the
            // declaring namespace. Catches projects that don't follow PSR-4
            // strictly (one file per class, path = namespace).
            const auto last_sep = raw.import_string.rfind('\\');
            if (last_sep != std::string::npos) {
                const std::string parent_ns = raw.import_string.substr(0, last_sep);
                const auto parent_it = ctx.namespace_to_files.find(parent_ns);
                if (parent_it != ctx.namespace_to_files.end()) {
                    return parent_it->second;
                }
            }
            // Last resort: full string as a namespace (rare but handles
            // `use Slim;` aliasing the namespace itself).
            const auto it = ctx.namespace_to_files.find(raw.import_string);
            if (it != ctx.namespace_to_files.end()) {
                return it->second;
            }
        }
    }

    // --- 9. C# `using` via namespace index -------------------------
    // `using SampleApp.Models;` → every file declaring
    // `namespace SampleApp.Models` gets an internal edge.
    if (source.language == Language::CSharp && raw.kind == "use") {
        const auto it = ctx.namespace_to_files.find(raw.import_string);
        if (it != ctx.namespace_to_files.end()) {
            return it->second;
        }
    }

    // --- 10. Go import via go.mod prefix ---------------------------
    // If a `go.mod` exists at the project root and the import starts
    // with its module path, strip the prefix and emit edges to every
    // `.go` file under the remaining directory (Go's import unit is
    // the package = directory). Standard-library and 3rd-party
    // imports fall through as external.
    if (source.language == Language::Go &&
        !ctx.go_module_prefix.empty())
    {
        const std::string& prefix = ctx.go_module_prefix;
        const std::string& imp    = raw.import_string;
        if (imp == prefix ||
            (imp.size() > prefix.size() + 1 &&
             imp.compare(0, prefix.size(), prefix) == 0 &&
             imp[prefix.size()] == '/'))
        {
            const std::string rel_dir = (imp.size() > prefix.size())
                ? imp.substr(prefix.size() + 1)
                : std::string{};
            std::vector<std::int64_t> hits;
            for (const FileEntry& file : files) {
                if (file.language != Language::Go) {
                    continue;
                }
                const std::string parent =
                    file.path_relative.parent_path().generic_string();
                if (parent == rel_dir) {
                    hits.push_back(file.id);
                }
            }
            return hits;
        }
    }

    return {}; // external / unresolved
}

} // namespace

void resolve_all(CodeIndex&                       index,
                 const std::filesystem::path&     project_root,
                 const std::vector<FileImports>&  per_file)
{
    const ResolveCtx ctx{
        /* files               = */ index.snapshot_files(),
        /* namespace_to_files  = */ build_namespace_index(per_file),
        /* go_module_prefix    = */ read_go_module_prefix(project_root),
    };

    std::size_t resolved_count = 0;
    std::size_t external_count = 0;

    for (const FileImports& source : per_file) {
        for (const RawImport& raw : source.imports) {
            const std::vector<std::int64_t> targets = resolve_one(source, raw, ctx);
            if (targets.empty()) {
                Dependency dep;
                dep.source_file_id = source.file_id;
                dep.target_file_id = 0;
                dep.import_string  = raw.import_string;
                dep.kind           = raw.kind;
                index.add_dependency(std::move(dep));
                ++external_count;
                continue;
            }
            for (const std::int64_t target : targets) {
                if (target == source.file_id) {
                    continue; // don't record self-edges
                }
                Dependency dep;
                dep.source_file_id = source.file_id;
                dep.target_file_id = target;
                dep.import_string  = raw.import_string;
                dep.kind           = raw.kind;
                index.add_dependency(std::move(dep));
                ++resolved_count;
            }
        }
    }

    VECTIS_LOG_INFO(
        "Dependency resolution complete: {} resolved, {} external "
        "(go_module='{}', namespaces={})",
        resolved_count, external_count,
        ctx.go_module_prefix, ctx.namespace_to_files.size());
}

} // namespace vectis::code
