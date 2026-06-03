#include "code/dependency_resolver.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"
#include "core/log.h"

namespace vectis::code {

namespace {

/// Extension candidates to try, in priority order, when resolving an
/// import that has no extension (e.g. TypeScript `./foo`).
[[nodiscard]] std::vector<std::string_view> candidate_extensions(Language language) noexcept
{
    switch (language) {
    case Language::TypeScript:
        return {".ts", ".tsx", ".d.ts", ".js"};
    case Language::JavaScript:
        return {".js", ".mjs", ".cjs", ".jsx"};
    case Language::Python:
        return {".py", ".pyi"};
    case Language::Cpp:
        return {".hpp", ".h", ".hh", ".hxx", ".cpp", ".cxx", ".cc"};
    case Language::C:
        return {".h", ".c"};
    case Language::Rust:
        return {".rs"};
    case Language::Java:
        return {".java"};
    case Language::Ruby:
        return {".rb"};
    case Language::Php:
        return {".php"};
    default:
        return {};
    }
}

/// True when files of this language contribute "endswith" entries to
/// `PathLookup::by_suffix`. Only the languages whose resolvers use
/// suffix matching are indexed (C/C++ `#include`, Java FQCN, PHP `use`,
/// Ruby `require`); indexing the others would pay for entries no
/// caller ever queries.
[[nodiscard]] bool uses_suffix_lookup(Language lang) noexcept
{
    switch (lang) {
    case Language::C:
    case Language::Cpp:
    case Language::Java:
    case Language::Php:
    case Language::Ruby:
        return true;
    default:
        return false;
    }
}

/// Lexical normalization without touching the filesystem: collapse
/// `./` and `../` segments in a relative path.
[[nodiscard]] std::filesystem::path normalize_relative(const std::filesystem::path& in)
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
                    auto str = out.string();
                    if (str.size() > 1 && str.back() == '/') {
                        str.pop_back();
                        out = str;
                    }
                }
            }
            else {
                out /= "..";
            }
        }
        else {
            out /= s;
        }
    }
    return out;
}

/// Exact-path → file_id lookup. Returns 0 when the path is not in the
/// index.
[[nodiscard]] std::int64_t lookup_exact(const PathLookup& lookup,
                                        const std::filesystem::path& candidate)
{
    const auto it = lookup.by_exact_path.find(candidate.generic_string());
    return it != lookup.by_exact_path.end() ? it->second : 0;
}

/// Suffix-bucket lookup. Returns `nullptr` when no file's path ends
/// (at a `/`-boundary) with `suffix`, otherwise a pointer to the
/// file_id vector in file-insertion order.
[[nodiscard]] const std::vector<std::int64_t>* lookup_suffix(const PathLookup& lookup,
                                                             std::string_view suffix)
{
    const auto it = lookup.by_suffix.find(std::string{suffix});
    return it != lookup.by_suffix.end() ? &it->second : nullptr;
}

/// First suffix candidate, or 0 if none. Replaces the old
/// `path_ends_with` linear scan with an O(1) hash lookup.
[[nodiscard]] std::int64_t lookup_first_by_suffix(const PathLookup& lookup, std::string_view suffix)
{
    const auto* hits = lookup_suffix(lookup, suffix);
    return (hits != nullptr && !hits->empty()) ? hits->front() : 0;
}

/// Try each extension against the precomputed path index. First hit
/// wins, including the JS/TS barrel-file `<stem>/index<ext>` variant.
[[nodiscard]] std::int64_t match_by_extension(const PathLookup& lookup,
                                              const std::filesystem::path& candidate_stem,
                                              const std::vector<std::string_view>& extensions)
{
    for (const std::string_view ext : extensions) {
        std::filesystem::path candidate = candidate_stem;
        candidate += std::string{ext};
        if (const std::int64_t id = lookup_exact(lookup, candidate); id != 0) {
            return id;
        }

        candidate = candidate_stem / (std::string{"index"} + std::string{ext});
        if (const std::int64_t id = lookup_exact(lookup, candidate); id != 0) {
            return id;
        }
    }
    return 0;
}

/// Rust module directory of a file: where its child modules live.
/// Entry-point files (lib.rs, main.rs, mod.rs) own their parent dir;
/// any other file `foo.rs` owns `<dir>/foo/`.
[[nodiscard]] std::filesystem::path rust_module_dir(const std::filesystem::path& rel)
{
    const std::string name = rel.filename().string();
    if (name == "lib.rs" || name == "main.rs" || name == "mod.rs") {
        return rel.parent_path();
    }
    return rel.parent_path() / rel.stem();
}

/// Resolve a Rust module path-stem to a file: `<stem>.rs` or
/// `<stem>/mod.rs`. If BOTH exist the module is ambiguous (Rust rejects
/// it) — return 0. Never probes `index.rs` (a JS/TS artifact).
[[nodiscard]] std::int64_t match_rust_module(const PathLookup& lookup,
                                             const std::filesystem::path& stem)
{
    std::filesystem::path as_file = stem;
    as_file += ".rs";
    const std::int64_t file_hit = lookup_exact(lookup, as_file);
    const std::int64_t dir_hit = lookup_exact(lookup, stem / "mod.rs");
    if (file_hit != 0 && dir_hit != 0) {
        return 0; // ambiguous -> fail closed
    }
    return file_hit != 0 ? file_hit : dir_hit;
}

/// Split a dotted name like `foo.bar.Baz` into path segments `foo/bar/Baz`.
/// Empty segments (leading dot, consecutive dots) are skipped.
[[nodiscard]] std::filesystem::path split_dotted(std::string_view dotted, char separator)
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

/// Split a `::`-separated Rust path into segments, preserving empties so a
/// leading `::` yields a leading empty segment the caller can reject.
/// `a::b::c` -> {"a","b","c"}; always returns at least one segment.
[[nodiscard]] std::vector<std::string> split_colons(std::string_view path)
{
    std::vector<std::string> segs;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t sep = path.find("::", pos);
        const std::size_t end = (sep == std::string_view::npos) ? path.size() : sep;
        segs.emplace_back(path.substr(pos, end - pos));
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 2;
    }
    return segs;
}

/// Look up a Python module rooted at `<stem>` — first as a `.py` /
/// `.pyi` file, then as a `<stem>/__init__.py(i)` package marker.
[[nodiscard]] std::int64_t match_python_module(const PathLookup& lookup,
                                               const std::filesystem::path& stem)
{
    static const std::vector<std::string_view> k_py_ext = {".py", ".pyi"};
    if (const std::int64_t direct = match_by_extension(lookup, stem, k_py_ext); direct != 0) {
        return direct;
    }
    for (const std::string_view ext : k_py_ext) {
        const std::filesystem::path init_path = stem / (std::string{"__init__"} + std::string{ext});
        if (const std::int64_t id = lookup_exact(lookup, init_path); id != 0) {
            return id;
        }
    }
    return 0;
}

/// Python-specific: convert `foo.bar` to `foo/bar` and try `.py`/.pyi,
/// or `foo/bar/__init__.py`.
[[nodiscard]] std::int64_t match_python_dotted(const PathLookup& lookup, std::string_view dotted)
{
    return match_python_module(lookup, split_dotted(dotted, '.'));
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
[[nodiscard]] std::int64_t match_python_relative(const PathLookup& lookup,
                                                 const std::filesystem::path& source_relative_path,
                                                 std::string_view with_dots)
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
    return match_python_module(lookup, stem);
}

/// PHP-specific: `Slim\Factory\Foo` → try `Slim/Factory/Foo.php`
/// directly, then fall back to a suffix match so projects rooted
/// under `src/` (the PSR-4 default) still resolve without us reading
/// `composer.json`.
[[nodiscard]] std::int64_t match_php_namespaced(const PathLookup& lookup,
                                                std::string_view namespaced)
{
    const std::filesystem::path stem = split_dotted(namespaced, '\\');
    static const std::vector<std::string_view> k_php_ext = {".php"};
    if (const std::int64_t direct = match_by_extension(lookup, stem, k_php_ext); direct != 0) {
        return direct;
    }
    const std::string suffix = stem.generic_string() + ".php";
    return lookup_first_by_suffix(lookup, suffix);
}

/// A detected Rust crate root: the lib.rs/main.rs file and its dir.
struct RustCrateRoot
{
    std::int64_t file_id = 0;
    std::filesystem::path root_dir;
};

/// Per-crate module graph, built once from resolved `mod` declarations.
/// Keys are `"<root_id>\x1f<a::b>"` so the same module path under two
/// crate roots never collides. `contexts` maps a file to every
/// (root, module-path) pair it is reachable as (`self`/`super` anchors);
/// `owning_roots` maps a file to the crate root(s) that own it (the
/// `crate::` anchor), derived from mod-reachability so a lib+bin crate
/// sharing `src/` never cross-resolves. Orphan files no `mod` reaches
/// fall back to single-root directory ancestry.
struct RustModuleIndex
{
    std::unordered_map<std::string, std::int64_t> path_to_file;
    std::unordered_map<std::int64_t, std::vector<std::pair<std::int64_t, std::string>>> contexts;
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> owning_roots;
};

[[nodiscard]] std::string rust_key(std::int64_t root_id, std::string_view module_path)
{
    return std::to_string(root_id) + '\x1f' + std::string{module_path};
}

[[nodiscard]] std::vector<RustCrateRoot>
detect_rust_crate_roots(const std::vector<FileEntry>& files)
{
    std::vector<RustCrateRoot> roots;
    for (const FileEntry& f : files) {
        if (f.language != Language::Rust) {
            continue;
        }
        const std::string name = f.path_relative.filename().string();
        if (name == "lib.rs" || name == "main.rs") {
            roots.push_back(RustCrateRoot{f.id, f.path_relative.parent_path()});
        }
    }
    return roots;
}

/// Shared resolver context. All members are owned values — building
/// them inside the brace-init avoids dangling-reference questions and
/// keeps the resolver call chain free of context-rebuilds.
struct ResolveCtx
{
    std::unordered_map<std::string, std::vector<std::int64_t>> namespace_to_files;
    PathLookup path_lookup;
    std::string go_module_prefix;                 ///< empty if no go.mod
    std::vector<std::string> python_import_roots; ///< package-tree boundary roots
    std::unordered_map<std::string, std::vector<std::int64_t>> go_dir_to_files;
    RustModuleIndex rust_modules;
};

/// Read the first `module <path>` line out of a Go `go.mod` file. Only
/// looks at the project root — monorepos with per-subdir go.mods aren't
/// handled here, which is consistent with how we treat other config
/// files (Cargo.toml, tsconfig.json) today.
[[nodiscard]] std::string read_go_module_prefix(const std::filesystem::path& project_root)
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
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])) != 0) {
            ++start;
        }
        constexpr std::string_view k_prefix = "module ";
        if (line.compare(start, k_prefix.size(), k_prefix) == 0) {
            std::string path = line.substr(start + k_prefix.size());
            // Drop the trailing `// comment` first — go.mod allows
            // inline comments on the `module` line and `//` cannot
            // appear in a module path, so the first occurrence safely
            // delimits where the path ends.
            if (const std::size_t comment = path.find("//"); comment != std::string::npos) {
                path.erase(comment);
            }
            // Trim trailing whitespace / quotes.
            while (!path.empty() && (std::isspace(static_cast<unsigned char>(path.back())) != 0 ||
                                     path.back() == '"' || path.back() == '\'')) {
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

/// Detect every Python "import root" in the file snapshot, excluding
/// the project root itself (which the resolver always tries first).
///
/// A directory `D` is an import root when it is a package-tree
/// boundary: it directly contains a package (a child dir with an
/// `__init__.py` / `__init__.pyi`) AND `D` is itself NOT a package
/// (no `__init__` marker at `D`). The project root and a `src/`
/// layout qualify; so do `examples/tutorial/` and `tests/test_apps/`
/// in projects that ship example/test package trees.
///
/// Returned roots are relative-path prefixes (generic-string form,
/// no trailing slash). The empty project root is intentionally
/// excluded — `resolve_one` resolves against it first and only retries
/// these roots on a miss. Files in the snapshot are already filtered
/// by the scanner's exclude list, so vendored / build dirs never
/// contribute candidate roots here.
[[nodiscard]] std::vector<std::string>
detect_python_import_roots(const std::vector<FileEntry>& files)
{
    // Directories that carry an `__init__.py(i)` marker — i.e. packages.
    std::unordered_set<std::string> package_dirs;
    for (const FileEntry& file : files) {
        if (file.language != Language::Python) {
            continue;
        }
        const std::filesystem::path& rel = file.path_relative;
        const std::string stem = rel.stem().string();
        if (stem != "__init__") {
            continue;
        }
        const std::string ext = rel.extension().string();
        if (ext != ".py" && ext != ".pyi") {
            continue;
        }
        package_dirs.insert(rel.parent_path().generic_string());
    }

    // A directory directly containing a package is a candidate root.
    // It qualifies only if it is not itself a package directory.
    // Skip the empty (project-root) candidate — resolved separately.
    std::unordered_set<std::string> roots;
    for (const std::string& pkg : package_dirs) {
        const std::string parent = std::filesystem::path{pkg}.parent_path().generic_string();
        if (parent.empty()) {
            continue;
        }
        if (package_dirs.contains(parent)) {
            continue; // parent is inside a package — not a boundary
        }
        roots.insert(parent);
    }

    // Deterministic order keeps the per-root retry stable across runs;
    // ambiguity (2+ roots match) is order-independent, but a sorted set
    // makes any future single-root logic reproducible.
    std::vector<std::string> out(roots.begin(), roots.end());
    std::ranges::sort(out);
    return out;
}

/// Retry an absolute Python dotted import against each detected import
/// root after a project-root miss. Mirrors the Spring-bean uniqueness
/// rule: take the target only when exactly ONE root yields a match;
/// two or more matching roots are ambiguous and stay external (we never
/// guess which `tests` / `app` package the import meant).
[[nodiscard]] std::int64_t match_python_dotted_in_roots(const PathLookup& lookup,
                                                        const std::vector<std::string>& roots,
                                                        std::string_view dotted)
{
    const std::filesystem::path tail = split_dotted(dotted, '.');
    if (tail.empty()) {
        return 0;
    }
    std::int64_t sole_hit = 0;
    std::size_t match_count = 0;
    for (const std::string& root : roots) {
        const std::int64_t hit = match_python_module(lookup, std::filesystem::path{root} / tail);
        if (hit != 0) {
            if (++match_count >= 2) {
                return 0; // ambiguous — bail without guessing
            }
            sole_hit = hit;
        }
    }
    return match_count == 1 ? sole_hit : 0;
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

/// Build the `parent-dir → [.go file_id, ...]` index. Replaces a linear
/// scan over every file (filtered to `Language::Go`) in the Go import
/// branch of `resolve_one`; the package = directory rule means one
/// directory's `.go` files form one package, so a bucket per dir is
/// the right shape.
[[nodiscard]] std::unordered_map<std::string, std::vector<std::int64_t>>
build_go_dir_index(const std::vector<FileEntry>& files)
{
    std::unordered_map<std::string, std::vector<std::int64_t>> out;
    for (const FileEntry& file : files) {
        if (file.language != Language::Go) {
            continue;
        }
        out[file.path_relative.parent_path().generic_string()].push_back(file.id);
    }
    return out;
}

/// Build the per-crate Rust module graph. Each crate root (lib.rs /
/// main.rs) is walked through its declared `mod` chain — resolved via the
/// `rust_module_dir` / `match_rust_module` helpers — so a
/// `use` path can only ever land on a genuinely declared module, never
/// a stray same-named file. `owning_roots` is derived from that same
/// reachability (a file's owning roots are the distinct roots of its
/// `contexts`), with a single-root directory-ancestry fallback for
/// orphans no `mod` reaches (e.g. `include!`d sources); `src/bin/*`
/// files are their own crates and never inherit the library root.
[[nodiscard]] RustModuleIndex build_rust_module_index(const std::vector<FileEntry>& files,
                                                      const PathLookup& lookup,
                                                      const std::vector<FileImports>& per_file)
{
    RustModuleIndex idx;
    const std::vector<RustCrateRoot> roots = detect_rust_crate_roots(files);
    if (roots.empty()) {
        return idx;
    }

    std::unordered_map<std::int64_t, std::filesystem::path> path_of;
    for (const FileEntry& f : files) {
        path_of.emplace(f.id, f.path_relative);
    }
    std::unordered_map<std::int64_t, std::vector<std::string>> mods_of;
    for (const FileImports& s : per_file) {
        if (s.language != Language::Rust) {
            continue;
        }
        for (const RawImport& raw : s.imports) {
            if (raw.kind == "mod") {
                mods_of[s.file_id].push_back(raw.import_string);
            }
        }
    }

    // Walk each crate root through its declared `mod` chain. A root file
    // is seeded into `contexts` as `{(self_root, "")}`, so reachability
    // alone tells us which crate(s) a file belongs to.
    for (const RustCrateRoot& r : roots) {
        struct Node
        {
            std::int64_t file_id;
            std::string module_path;
        };
        std::vector<Node> stack{{r.file_id, std::string{}}};
        std::unordered_set<std::int64_t> seen;
        idx.path_to_file[rust_key(r.file_id, "")] = r.file_id;
        idx.contexts[r.file_id].emplace_back(r.file_id, std::string{});
        while (!stack.empty()) {
            const Node cur = stack.back();
            stack.pop_back();
            if (!seen.insert(cur.file_id).second) {
                continue;
            }
            const auto mit = mods_of.find(cur.file_id);
            if (mit == mods_of.end()) {
                continue;
            }
            const std::filesystem::path base = rust_module_dir(path_of[cur.file_id]);
            for (const std::string& name : mit->second) {
                const std::int64_t child = match_rust_module(lookup, base / name);
                if (child == 0) {
                    continue;
                }
                const std::string child_path =
                    cur.module_path.empty() ? name : cur.module_path + "::" + name;
                idx.path_to_file[rust_key(r.file_id, child_path)] = child;
                idx.contexts[child].emplace_back(r.file_id, child_path);
                stack.push_back({child, child_path});
            }
        }
    }

    // `crate::` anchors at the file's owning crate root(s). Prefer
    // mod-reachability: a file's owning roots are the distinct roots of
    // its `contexts`. This keeps a lib+bin crate that shares `src/` from
    // cross-resolving — `lib.rs`->[lib], `main.rs`->[main], a module the
    // lib chain declares ->[lib].
    for (const auto& [file_id, ctxs] : idx.contexts) {
        std::vector<std::int64_t>& owners = idx.owning_roots[file_id];
        for (const auto& [root, mod_path] : ctxs) {
            if (std::ranges::find(owners, root) == owners.end()) {
                owners.push_back(root);
            }
        }
    }

    // Files no `mod` chain reaches (orphans — e.g. `include!`d sources)
    // have no `contexts`, so reachability can't place them. Fall back to
    // directory ancestry, but assign ONLY when a single crate root sits
    // at the deepest ancestor dir; a tie (lib+main sharing `src/`) leaves
    // the orphan unowned so `crate::` fails closed.
    const auto under_root = [](const std::string& filed, const std::string& rdir) -> bool {
        if (rdir.empty()) {
            return true;
        }
        return filed.size() > rdir.size() && filed.compare(0, rdir.size(), rdir) == 0 &&
               filed[rdir.size()] == '/';
    };
    const auto quarantined = [](const std::string& filed, const std::string& rdir) -> bool {
        const std::string rel = rdir.empty() ? filed : filed.substr(rdir.size() + 1);
        return rel.starts_with("bin/"); // src/bin/* are separate crates
    };
    std::vector<std::string> root_dirs;
    root_dirs.reserve(roots.size());
    for (const RustCrateRoot& r : roots) {
        root_dirs.push_back(r.root_dir.generic_string());
    }
    for (const FileEntry& f : files) {
        if (f.language != Language::Rust || idx.contexts.contains(f.id)) {
            continue; // non-Rust, or already placed by mod-reachability
        }
        const std::string filed = f.path_relative.generic_string();
        std::size_t best_len = std::string::npos;
        for (const std::string& rdir : root_dirs) {
            if (!under_root(filed, rdir) || quarantined(filed, rdir)) {
                continue;
            }
            if (best_len == std::string::npos || rdir.size() > best_len) {
                best_len = rdir.size();
            }
        }
        if (best_len == std::string::npos) {
            continue;
        }
        std::vector<std::int64_t> deepest;
        for (std::size_t ri = 0; ri < roots.size(); ++ri) {
            const std::string& rdir = root_dirs[ri];
            if (rdir.size() == best_len && under_root(filed, rdir) && !quarantined(filed, rdir)) {
                deepest.push_back(roots[ri].file_id);
            }
        }
        if (deepest.size() == 1) {
            idx.owning_roots[f.id] = std::move(deepest);
        }
    }
    return idx;
}

/// Resolve an in-crate Rust `use` path against the module graph. crate ->
/// crate root; self -> source module; super(*) -> ancestor module. std/core/
/// alloc, Self, unanchored first segment, ambiguity, and multi-crate
/// disagreement all -> 0 (external). Returns the deepest declared module's id.
[[nodiscard]] std::int64_t resolve_rust_use(const RustModuleIndex& idx, const FileImports& source,
                                            std::string_view import_string)
{
    const std::vector<std::string> segs = split_colons(import_string);
    const std::string& head = segs.front();
    if (head == "std" || head == "core" || head == "alloc" || head == "Self") {
        return 0;
    }

    std::vector<std::pair<std::int64_t, std::string>> starts;
    std::size_t first_mod_seg = 0;
    if (head == "crate") {
        const auto it = idx.owning_roots.find(source.file_id);
        if (it == idx.owning_roots.end()) {
            return 0;
        }
        for (const std::int64_t root : it->second) {
            starts.emplace_back(root, std::string{});
        }
        first_mod_seg = 1;
    }
    else if (head == "self" || head == "super") {
        const auto it = idx.contexts.find(source.file_id);
        if (it == idx.contexts.end()) {
            return 0;
        }
        std::size_t supers = 0;
        while (first_mod_seg < segs.size() &&
               (segs[first_mod_seg] == "self" || segs[first_mod_seg] == "super")) {
            if (segs[first_mod_seg] == "super") {
                ++supers;
            }
            ++first_mod_seg;
        }
        for (const auto& [root, mod_path] : it->second) {
            // Empty mod_path = crate root (no segments); split otherwise.
            std::vector<std::string> parts;
            if (!mod_path.empty()) {
                parts = split_colons(mod_path);
            }
            if (supers > parts.size()) {
                continue;
            }
            parts.resize(parts.size() - supers);
            std::string start;
            for (const auto& q : parts) {
                start += (start.empty() ? "" : "::") + q;
            }
            starts.emplace_back(root, start);
        }
    }
    else {
        return 0;
    }

    std::int64_t agreed = -1;
    bool any_miss = false;
    for (const auto& [root, start] : starts) {
        std::int64_t best = 0;
        std::string path = start;
        for (std::size_t i = first_mod_seg; i < segs.size(); ++i) {
            path += (path.empty() ? "" : "::") + segs[i];
            const auto hit = idx.path_to_file.find(rust_key(root, path));
            if (hit != idx.path_to_file.end()) {
                best = hit->second;
            }
        }
        if (best == 0) {
            any_miss = true;
            continue;
        }
        if (agreed == -1) {
            agreed = best;
        }
        else if (agreed != best) {
            return 0;
        }
    }
    // Fail closed when the path resolves in one candidate crate but not
    // another (a file genuinely shared by two crates): disagreement by
    // omission is still disagreement.
    if (agreed != -1 && any_miss) {
        return 0;
    }
    return agreed == -1 ? 0 : agreed;
}

/// Resolve one raw import to a set of file_ids in the index. Most
/// imports resolve to at most one file (returned as a single-element
/// vector). C# `using X.Y;` and Go `import "x/y"` are different — a
/// namespace or package maps to every file declaring it, so the
/// result can be a vector of several ids. An empty vector means the
/// import is external / unresolved, and a single external edge is
/// recorded with `target_file_id = 0`.
[[nodiscard]] std::vector<std::int64_t> resolve_one(const FileImports& source, const RawImport& raw,
                                                    const ResolveCtx& ctx)
{
    const PathLookup& lookup = ctx.path_lookup;
    const std::vector<std::string_view> exts = candidate_extensions(source.language);

    // --- 1. Relative path (./ or ../) ------------------------------
    const bool is_relative_prefix =
        raw.import_string.starts_with("./") || raw.import_string.starts_with("../");

    if (is_relative_prefix && !exts.empty()) {
        const std::filesystem::path source_dir = source.relative_path.parent_path();
        const std::filesystem::path joined = normalize_relative(source_dir / raw.import_string);

        if (!std::filesystem::path{raw.import_string}.extension().empty()) {
            if (const std::int64_t id = lookup_exact(lookup, joined); id != 0) {
                return {id};
            }
        }
        if (const std::int64_t id = match_by_extension(lookup, joined, exts); id != 0) {
            return {id};
        }
    }

    // --- 2. C/C++ include path with an extension already in place --
    if ((source.language == Language::Cpp || source.language == Language::C) &&
        !std::filesystem::path{raw.import_string}.extension().empty()) {
        if (const std::int64_t id = lookup_exact(lookup, std::filesystem::path{raw.import_string});
            id != 0) {
            return {id};
        }
        const std::filesystem::path joined =
            normalize_relative(source.relative_path.parent_path() / raw.import_string);
        if (const std::int64_t id = lookup_exact(lookup, joined); id != 0) {
            return {id};
        }
        if (const std::int64_t id = lookup_first_by_suffix(lookup, raw.import_string); id != 0) {
            return {id};
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
            const std::int64_t hit =
                match_python_relative(lookup, source.relative_path, raw.import_string);
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
        // Project-root resolution wins first. On a miss, retry against
        // each detected import root (src/, examples/<x>/, tests/<x>/);
        // an ambiguous hit across two+ roots stays external.
        if (const std::int64_t hit = match_python_dotted(lookup, raw.import_string); hit != 0) {
            return {hit};
        }
        const std::int64_t root_hit =
            match_python_dotted_in_roots(lookup, ctx.python_import_roots, raw.import_string);
        return root_hit != 0 ? std::vector<std::int64_t>{root_hit} : std::vector<std::int64_t>{};
    }

    // --- 4. TS/JS bare module name — external, no resolution.

    // --- 5. Rust use / mod -----------------------------------------
    if (source.language == Language::Rust) {
        if (raw.kind == "mod") {
            const std::filesystem::path base = rust_module_dir(source.relative_path);
            const std::int64_t hit = match_rust_module(lookup, base / raw.import_string);
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
        if (raw.kind == "use") {
            const std::int64_t hit = resolve_rust_use(ctx.rust_modules, source, raw.import_string);
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
    }

    // --- 6. Java import --------------------------------------------
    // Specific imports (`import com.foo.Bar;`) hit the path-based
    // matcher; wildcards (`import com.foo.*;`) strip the asterisk at
    // capture time and so look like a bare package name here. When
    // the path match fails we fall back to the namespace index —
    // if `com.foo` is a known package, emit one edge per file in it.
    if (source.language == Language::Java) {
        const auto candidates = match_java_dotted_candidates(lookup, raw.import_string);
        if (!candidates.empty()) {
            return {candidates.front()};
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
        const std::filesystem::path stem = normalize_relative(source_dir / raw.import_string);
        if (const std::int64_t hit = match_by_extension(lookup, stem, {".rb"}); hit != 0) {
            return {hit};
        }
        std::string suffix{raw.import_string};
        if (!suffix.ends_with(".rb")) {
            suffix += ".rb";
        }
        if (const std::int64_t hit = lookup_first_by_suffix(lookup, suffix); hit != 0) {
            return {hit};
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
            const std::filesystem::path joined = normalize_relative(source_dir / path_str);

            if (!std::filesystem::path{raw.import_string}.extension().empty()) {
                if (const std::int64_t id = lookup_exact(lookup, joined); id != 0) {
                    return {id};
                }
                if (const std::int64_t id = lookup_first_by_suffix(lookup, path_str); id != 0) {
                    return {id};
                }
            }
            const std::int64_t hit = match_by_extension(lookup, joined, {".php"});
            return hit != 0 ? std::vector<std::int64_t>{hit} : std::vector<std::int64_t>{};
        }
        if (raw.kind == "use") {
            // PSR-4 path-style match: `Slim\Factory\Foo` → `Slim/Factory/Foo.php`,
            // either at the project root or nested under `src/`. This is the
            // common case — a `use` clause names a class, not a namespace.
            if (const std::int64_t hit = match_php_namespaced(lookup, raw.import_string);
                hit != 0) {
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
    // with its module path, strip the prefix and look up every `.go`
    // file under the remaining directory (Go's import unit is the
    // package = directory). Standard-library and 3rd-party imports
    // fall through as external.
    if (source.language == Language::Go && !ctx.go_module_prefix.empty()) {
        const std::string& prefix = ctx.go_module_prefix;
        const std::string& imp = raw.import_string;
        if (imp == prefix ||
            (imp.size() > prefix.size() + 1 && imp.compare(0, prefix.size(), prefix) == 0 &&
             imp[prefix.size()] == '/')) {
            const std::string rel_dir =
                (imp.size() > prefix.size()) ? imp.substr(prefix.size() + 1) : std::string{};
            const auto it = ctx.go_dir_to_files.find(rel_dir);
            if (it != ctx.go_dir_to_files.end()) {
                return it->second;
            }
        }
    }

    return {}; // external / unresolved
}

} // namespace

PathLookup build_path_lookup(const std::vector<FileEntry>& files)
{
    PathLookup lookup;
    lookup.by_exact_path.reserve(files.size());

    for (const FileEntry& file : files) {
        const std::string path = file.path_relative.generic_string();
        lookup.by_exact_path.emplace(path, file.id);

        if (!uses_suffix_lookup(file.language)) {
            continue;
        }

        // Insert one suffix entry at each `/`-boundary of the path:
        // offset 0 (the whole path) and every position immediately
        // after a '/'. That mirrors the old `path_ends_with` semantics
        // — match either the whole path or a `/`-prefixed tail —
        // without paying for a per-candidate linear scan.
        std::size_t start = 0;
        while (start < path.size()) {
            if (start == 0 || path[start - 1] == '/') {
                lookup.by_suffix[path.substr(start)].push_back(file.id);
            }
            const std::size_t next = path.find('/', start);
            if (next == std::string::npos) {
                break;
            }
            start = next + 1;
        }
    }
    return lookup;
}

std::vector<std::int64_t> match_java_dotted_candidates(const PathLookup& lookup,
                                                       std::string_view dotted)
{
    std::vector<std::int64_t> result;

    const std::filesystem::path stem = split_dotted(dotted, '.');
    static const std::vector<std::string_view> k_java_ext = {".java"};

    // Direct path-shape match first so the wrapper's `front()` stays
    // first-match-wins.
    const std::int64_t direct = match_by_extension(lookup, stem, k_java_ext);
    if (direct != 0) {
        result.push_back(direct);
    }

    const std::string suffix = stem.generic_string() + ".java";
    if (const auto* hits = lookup_suffix(lookup, suffix); hits != nullptr) {
        for (const std::int64_t id : *hits) {
            if (id != direct) {
                result.push_back(id);
            }
        }
    }
    return result;
}

std::vector<std::int64_t> match_java_dotted_candidates(const std::vector<FileEntry>& files,
                                                       std::string_view dotted)
{
    return match_java_dotted_candidates(build_path_lookup(files), dotted);
}

void resolve_all(CodeIndex& index, const std::filesystem::path& project_root,
                 const std::vector<FileImports>& per_file)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    PathLookup lookup = build_path_lookup(files);
    RustModuleIndex rust_modules = build_rust_module_index(files, lookup, per_file);
    const ResolveCtx ctx{
        /* namespace_to_files = */ build_namespace_index(per_file),
        /* path_lookup        = */ std::move(lookup),
        /* go_module_prefix    = */ read_go_module_prefix(project_root),
        /* python_import_roots = */ detect_python_import_roots(files),
        /* go_dir_to_files     = */ build_go_dir_index(files),
        /* rust_modules        = */ std::move(rust_modules),
    };

    // Accumulate every resolved edge and flush as one batch — one
    // write-lock acquisition instead of one per import. For camel-
    // scale projects the import count runs into the hundreds of
    // thousands, so the per-edge lock overhead alone was material.
    std::vector<Dependency> pending;
    std::size_t resolved_count = 0;
    std::size_t external_count = 0;

    std::size_t reserve_hint = 0;
    for (const FileImports& source : per_file) {
        reserve_hint += source.imports.size();
    }
    pending.reserve(reserve_hint);

    for (const FileImports& source : per_file) {
        for (const RawImport& raw : source.imports) {
            const std::vector<std::int64_t> targets = resolve_one(source, raw, ctx);
            if (targets.empty()) {
                Dependency dep;
                dep.source_file_id = source.file_id;
                dep.target_file_id = 0;
                dep.import_string = raw.import_string;
                dep.kind = raw.kind;
                pending.push_back(std::move(dep));
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
                dep.import_string = raw.import_string;
                dep.kind = raw.kind;
                pending.push_back(std::move(dep));
                ++resolved_count;
            }
        }
    }

    index.add_dependencies(pending);

    VECTIS_LOG_INFO("Dependency resolution complete: {} resolved, {} external "
                    "(go_module='{}', namespaces={})",
                    resolved_count, external_count, ctx.go_module_prefix,
                    ctx.namespace_to_files.size());
}

} // namespace vectis::code
