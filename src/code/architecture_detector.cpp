#include "code/architecture_detector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "code/cmake_inspect.h"
#include "code/code_index.h"
#include "code/framework_hints.h"
#include "code/language.h"
#include "code/manifest_deps.h"
#include "code/symbol.h"

namespace vectis::code {

namespace {

/// Directory names that end an architecture-relevant descent. Anything
/// under `tests/`, `docs/`, `vendor/`, etc. is data, documentation, or
/// vendored code — its own internal layout (a fixture's `models/` and
/// `dao/`, a test harness's `main.cc`) is not architecturally
/// representative of the project.
const std::unordered_set<std::string> k_non_source_subtree_names = {
    "tests", "test",     "fixtures", "__fixtures__", "docs",
    "doc",   "examples", "example",  "vendor",       "third_party",
};

/// True if any segment of `path_relative` matches a stop-name above.
[[nodiscard]] bool is_under_non_source_subtree(const std::filesystem::path& path_relative)
{
    std::filesystem::path dir = path_relative;
    if (dir.has_filename()) {
        dir.remove_filename();
    }
    return std::ranges::any_of(dir, [](const auto& segment) {
        return k_non_source_subtree_names.contains(segment.string());
    });
}

struct PathSignals
{
    /// All directory segments seen in the index. The walk stops at
    /// the first test/fixture/doc/vendor root, so a fixture path like
    /// `tests/fixtures/.../models/user.py` doesn't inject `models`
    /// into the signal. Deep non-test subtrees walk in full, so
    /// `src/App.UI/ViewModels/...` etc. still surface their inner
    /// segments.
    std::unordered_set<std::string> segments;

    /// First-component directory names that have at least one nested
    /// file. Distinct from `segments` (which collapses every depth):
    /// a vendored `deps/<lib>/include/...` injects `include` into
    /// `segments` but only `deps` into `top_level_dirs`. Library
    /// detection requires `include`/`lib`/`src` at the project root.
    std::unordered_set<std::string> top_level_dirs;

    /// `main.*` files outside non-source subtrees (test harnesses
    /// and vendored copies don't count as application entry points).
    std::size_t main_count = 0;

    /// Per-language file counts plus a denominator for share %.
    /// Files with `Language::Unknown` aren't counted.
    std::array<std::size_t, k_language_count + 1> language_counts{};
    std::size_t classified_files = 0;

    /// Python library signals harvested during the same walk that
    /// builds the rest of this struct — `__init__.py` for either
    /// `src/<pkg>/` or `<pkg>/` at depth 1, plus presence of an app
    /// bootstrap (`manage.py` / `wsgi.py` / `asgi.py`) that would
    /// disqualify a library label.
    bool has_python_pkg_init = false;
    bool has_python_app_entry = false;
    std::string python_sample_pkg;
};

[[nodiscard]] PathSignals collect_path_signals(std::span<const FileEntry> files)
{
    PathSignals out;
    for (const FileEntry& file : files) {
        std::filesystem::path dir = file.path_relative;
        if (dir.has_filename()) {
            dir.remove_filename();
        }
        const bool under_non_source = is_under_non_source_subtree(file.path_relative);

        bool is_first = true;
        for (const auto& segment : dir) {
            const std::string s = segment.string();
            if (s.empty() || s == "/" || s == ".") {
                continue;
            }
            if (is_first) {
                out.top_level_dirs.insert(s);
                is_first = false;
            }
            out.segments.insert(s);
            if (k_non_source_subtree_names.contains(s)) {
                break;
            }
        }

        const std::string name = file.path_relative.filename().string();
        if (!under_non_source) {
            if (name.starts_with("main.") || name == "main") {
                ++out.main_count;
            }
            // Python library / app-entry detection — same single-pass
            // walk as the rest of PathSignals. Language gate so a shell
            // wrapper named `wsgi.py` can't disqualify a Python library.
            const bool is_python = file.language == Language::Python;
            const auto depth = std::distance(file.path_relative.begin(), file.path_relative.end());
            if (is_python &&
                (name == "manage.py" || name == "wsgi.py" || name == "asgi.py" ||
                 name == "__main__.py") &&
                depth <= 2) {
                out.has_python_app_entry = true;
            }
            if (is_python && name == "__init__.py") {
                auto it = file.path_relative.begin();
                if (it != file.path_relative.end()) {
                    const std::string first = it->string();
                    ++it;
                    if (it != file.path_relative.end()) {
                        if (first == "src") {
                            const std::string pkg = it->string();
                            ++it;
                            if (it != file.path_relative.end() && it->string() == "__init__.py") {
                                out.has_python_pkg_init = true;
                                if (out.python_sample_pkg.empty()) {
                                    out.python_sample_pkg = pkg;
                                }
                            }
                        }
                        else if (it->string() == "__init__.py") {
                            out.has_python_pkg_init = true;
                            if (out.python_sample_pkg.empty()) {
                                out.python_sample_pkg = first;
                            }
                        }
                    }
                }
            }
        }

        if (file.language != Language::Unknown) {
            ++out.language_counts[static_cast<std::size_t>(file.language)];
            ++out.classified_files;
        }
    }
    return out;
}

/// Count distinct top-level or second-level DIRECTORY leaves whose
/// name contains a `.` separator (e.g. `FlowForge.UI`, `Company.Core`)
/// — the hallmark of a .NET solution layout with one project per
/// dotted-name directory. The leaf filename is explicitly excluded
/// (otherwise every `main.py` or `vite.config.ts` would count).
[[nodiscard]] std::size_t count_dotted_project_dirs(std::span<const FileEntry> files)
{
    std::unordered_set<std::string> dotted;
    for (const FileEntry& file : files) {
        std::filesystem::path dir = file.path_relative;
        if (dir.has_filename()) {
            dir.remove_filename();
        }
        auto it = dir.begin();
        const auto end = dir.end();
        for (int depth = 0; depth < 2 && it != end; ++depth, ++it) {
            const std::string s = it->string();
            // Dotfiles (`.git`, `.vs`, `.worktrees`) don't count.
            if (s.size() > 2 && s.front() != '.' && s.find('.') != std::string::npos) {
                dotted.insert(s);
            }
        }
    }
    return dotted.size();
}

/// Root-scoped config files that mark an Electron desktop app even
/// when `package.json` is absent or doesn't carry the `electron`
/// dependency directly (e.g. monorepo packages that take the dep
/// transitively). Both Forge and Builder ship under several script
/// extensions / serialisation formats — we accept the common ones.
constexpr std::array<std::string_view, 8> k_electron_root_configs = {
    "forge.config.js",       "forge.config.ts",       "forge.config.cjs",
    "forge.config.mjs",      "electron-builder.yml",  "electron-builder.yaml",
    "electron-builder.json", "electron-builder.toml",
};

/// SPA framework config files: presence at the project root strongly
/// implies a frontend single-page app. Nested matches don't count —
/// backend frameworks ship one-off embedded mini-apps deep in their
/// tree (e.g. for exception-page rendering).
constexpr std::array<std::string_view, 4> k_spa_root_configs = {"next.config.js", "vite.config.ts",
                                                                "vite.config.js", "nuxt.config.ts"};

/// Walk up to two levels of directories under `project_root` and add
/// their names to the path signals. The index-based walk only sees
/// directories that contain a file vectis indexes, so layouts where
/// a defining directory holds non-source content (Rails `app/views/`
/// is `.html.erb` templates) get missed. Two levels deep is enough
/// for the canonical layouts (`app/views/`, `src/Project.UI/`,
/// top-level `controllers/`) without paying for a deep recursive walk.
///
/// `exclude_dir_names` is the same set the scanner used, so the disk
/// walk skips exactly what was filtered out of the index — no drift.
void augment_signals_from_disk(PathSignals& signals, const std::filesystem::path& project_root,
                               const std::unordered_set<std::string>& exclude_dir_names)
{
    if (project_root.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root, ec) || ec) {
        return;
    }
    const auto skip = [&exclude_dir_names](const std::string& name) {
        return name.empty() || name[0] == '.' || exclude_dir_names.contains(name) ||
               k_non_source_subtree_names.contains(name);
    };
    // Capture iterators separately so a constructor-time ec doesn't
    // run the loop body once with a sentinel value (matches the
    // pattern used by the ecosystem library detectors).
    std::filesystem::directory_iterator top_it(project_root, ec);
    if (ec) {
        return;
    }
    for (const auto& top_entry : top_it) {
        if (ec) {
            return;
        }
        if (!top_entry.is_directory(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string top_name = top_entry.path().filename().string();
        if (skip(top_name)) {
            continue;
        }
        signals.top_level_dirs.insert(top_name);
        signals.segments.insert(top_name);

        std::filesystem::directory_iterator sub_it(top_entry.path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        for (const auto& sub_entry : sub_it) {
            if (ec) {
                break;
            }
            if (!sub_entry.is_directory(ec) || ec) {
                ec.clear();
                continue;
            }
            const std::string sub_name = sub_entry.path().filename().string();
            if (!skip(sub_name)) {
                signals.segments.insert(sub_name);
            }
        }
    }
}

/// Read a text file (capped to 64 KiB so malformed huge files can't
/// stall the detector). Returns empty on any I/O error — all callers
/// treat an empty payload as "not present / not detected".
[[nodiscard]] std::string read_text_capped(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {};
    }
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    constexpr std::streamsize k_max = static_cast<std::streamsize>(64) * 1024;
    std::string out;
    out.resize(k_max);
    in.read(out.data(), k_max);
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

/// Workspace-helper return shape: prose for `vectis explain`, plus a
/// flat list of `category:value` tokens for the JSON `signals` array.
struct WorkspaceHit
{
    std::string reasoning;
    std::vector<std::string> signals;
};

/// Detect a Cargo workspace: `Cargo.toml` at the project root containing
/// a `[workspace]` table header. Matches single-line `[workspace]` and
/// the uncommon variant with trailing content on the same line.
[[nodiscard]] std::optional<WorkspaceHit>
detect_rust_workspace(const std::filesystem::path& project_root)
{
    const std::string body = read_text_capped(project_root / "Cargo.toml");
    if (body.empty()) {
        return std::nullopt;
    }
    // Match `[workspace]` as a TOML table header — must start at the
    // beginning of a line (after optional whitespace) and run to the
    // closing bracket. Avoids matching `[workspace.metadata]` etc.
    // which are sub-tables and don't imply a workspace on their own.
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0) {
            ++i;
        }
        if (i + 11 <= line.size() && line.compare(i, 11, "[workspace]") == 0) {
            return WorkspaceHit{"Cargo workspace (Rust multi-crate layout)",
                                {"workspace:cargo", "manifest:Cargo.toml"}};
        }
    }
    return std::nullopt;
}

/// Detect an npm-family monorepo via one of the usual marker files at
/// the project root: `package.json` with a `"workspaces"` field,
/// `pnpm-workspace.yaml`, `lerna.json`, or `turbo.json`. Each hint is
/// reported with its own reasoning string so the digest tells you
/// which tool configured the monorepo.
[[nodiscard]] std::optional<WorkspaceHit>
detect_npm_monorepo(const std::filesystem::path& project_root)
{
    std::error_code ec;
    if (std::filesystem::exists(project_root / "pnpm-workspace.yaml", ec) && !ec) {
        return WorkspaceHit{"pnpm workspace (pnpm-workspace.yaml)",
                            {"workspace:pnpm", "manifest:pnpm-workspace.yaml"}};
    }
    ec.clear();
    if (std::filesystem::exists(project_root / "lerna.json", ec) && !ec) {
        return WorkspaceHit{"Lerna monorepo (lerna.json)",
                            {"workspace:lerna", "manifest:lerna.json"}};
    }
    ec.clear();
    if (std::filesystem::exists(project_root / "turbo.json", ec) && !ec) {
        return WorkspaceHit{"Turborepo (turbo.json)",
                            {"workspace:turborepo", "manifest:turbo.json"}};
    }

    const std::string pkg = read_text_capped(project_root / "package.json");
    // Match `"workspaces":` (with optional whitespace) so a description
    // or keyword containing the bare token doesn't trigger a false hit.
    if (!pkg.empty() && (pkg.find(R"("workspaces":)") != std::string::npos ||
                         pkg.find(R"("workspaces" :)") != std::string::npos)) {
        return WorkspaceHit{R"(npm workspaces (package.json "workspaces"))",
                            {"workspace:npm", "manifest:package.json"}};
    }
    return std::nullopt;
}

/// Detect a modern Python project layout. Two flavours:
///   * Multi-package workspace: `pyproject.toml` at root + multiple
///     subdirectories under `src/` that each contain `__init__.py`.
///   * Poetry / PEP 621 multi-package project (same, heuristically).
/// Returns a reasoning string if the signal fires, nullopt otherwise.
[[nodiscard]] std::optional<WorkspaceHit>
detect_python_packages(const std::filesystem::path& project_root, std::span<const FileEntry> files)
{
    std::error_code ec;
    const bool has_pyproject = std::filesystem::exists(project_root / "pyproject.toml", ec) && !ec;
    if (!has_pyproject) {
        return std::nullopt;
    }

    // Count distinct `src/<pkg>/__init__.py` subpackages — a project
    // with >= 2 is almost always a multi-package layout (pyproject +
    // src-layout + multiple packages). One package is a normal library;
    // zero means the pyproject describes something else (tool config).
    std::unordered_set<std::string> src_packages;
    for (const FileEntry& file : files) {
        if (file.path_relative.filename().string() != "__init__.py") {
            continue;
        }
        auto it = file.path_relative.begin();
        if (it == file.path_relative.end() || it->string() != "src") {
            continue;
        }
        ++it;
        if (it == file.path_relative.end()) {
            continue;
        }
        src_packages.insert(it->string());
    }
    if (src_packages.size() >= 2) {
        return WorkspaceHit{"pyproject.toml + src-layout with " +
                                std::to_string(src_packages.size()) + " packages",
                            {"workspace:python", "manifest:pyproject.toml",
                             "package-count:" + std::to_string(src_packages.size())}};
    }
    return std::nullopt;
}

/// Runtime identified by a root build manifest. Single source of
/// truth: every runtime maps to a display label and the set of
/// indexed languages it accepts. Adding a new manifest below means
/// adding one enumerator + one switch arm in each table.
enum class Runtime : std::uint8_t
{
    Go,
    Python,
    Php,
    Ruby,
    NodeJs,
    Java,
    CCpp,
    Rust,
    CSharp,
    DotNetSolution,
};

[[nodiscard]] constexpr std::string_view runtime_label(Runtime r) noexcept
{
    switch (r) {
    case Runtime::Go:
        return "Go";
    case Runtime::Python:
        return "Python";
    case Runtime::Php:
        return "PHP";
    case Runtime::Ruby:
        return "Ruby";
    case Runtime::NodeJs:
        return "Node.js";
    case Runtime::Java:
        return "Java";
    case Runtime::CCpp:
        return "C/C++";
    case Runtime::Rust:
        return "Rust";
    case Runtime::CSharp:
        return "C#";
    case Runtime::DotNetSolution:
        return ".NET Solution";
    }
    return "?";
}

[[nodiscard]] std::span<const Language> runtime_languages(Runtime r) noexcept
{
    // Multi-language runtimes (Node.js → JS+TS, C/C++ → C+Cpp) are
    // why this isn't just one Language per Runtime.
    static constexpr std::array<Language, 1> k_go{Language::Go};
    static constexpr std::array<Language, 1> k_python{Language::Python};
    static constexpr std::array<Language, 1> k_php{Language::Php};
    static constexpr std::array<Language, 1> k_ruby{Language::Ruby};
    static constexpr std::array<Language, 2> k_node{Language::JavaScript, Language::TypeScript};
    static constexpr std::array<Language, 1> k_java{Language::Java};
    static constexpr std::array<Language, 2> k_ccpp{Language::C, Language::Cpp};
    static constexpr std::array<Language, 1> k_rust{Language::Rust};
    static constexpr std::array<Language, 1> k_csharp{Language::CSharp};
    switch (r) {
    case Runtime::Go:
        return k_go;
    case Runtime::Python:
        return k_python;
    case Runtime::Php:
        return k_php;
    case Runtime::Ruby:
        return k_ruby;
    case Runtime::NodeJs:
        return k_node;
    case Runtime::Java:
        return k_java;
    case Runtime::CCpp:
        return k_ccpp;
    case Runtime::Rust:
        return k_rust;
    case Runtime::CSharp:
    case Runtime::DotNetSolution:
        return k_csharp;
    }
    return {};
}

/// Share (0..100) of classified files matching `r`'s language family.
[[nodiscard]] int runtime_share(const PathSignals& signals, Runtime r) noexcept
{
    if (signals.classified_files == 0) {
        return 0;
    }
    std::size_t matching = 0;
    for (Language lang : runtime_languages(r)) {
        matching += signals.language_counts[static_cast<std::size_t>(lang)];
    }
    return static_cast<int>(100 * matching / signals.classified_files);
}

/// Build / runtime manifest at the project root, if any. Used to
/// upgrade confidence and reasoning for branches that otherwise fall
/// back to a generic "no distinctive layout" answer — knowing that a
/// project is e.g. a Go module or a PHP composer package is more
/// useful than just "Monolith (40%)".
struct ManifestInfo
{
    Runtime runtime;
    std::string filename;
    /// Direct dependency names extracted from the manifest, if the
    /// runtime has an extractor (`manifest_deps::extract_*`). Empty
    /// for runtimes without one yet, and for manifests that genuinely
    /// declare no deps. Names are verbatim from the file —
    /// `"react"`, `"@types/node"`, `"org.springframework.boot:spring-boot-starter-web"`
    /// — so downstream matchers can pick their own normalisation.
    std::vector<std::string> runtime_deps;
};

[[nodiscard]] std::optional<ManifestInfo>
detect_root_manifest(const std::filesystem::path& project_root)
{
    if (project_root.empty()) {
        return std::nullopt;
    }
    std::error_code ec;

    // Cargo.toml gets here only if the earlier workspace check didn't
    // fire — i.e. it's a single-crate Rust project.
    // Order matters: the first match wins. Native build manifests
    // (Makefile, configure.ac, meson.build) are the fallback for C/C++
    // projects without CMakeLists.txt — redis-style codebases. The
    // `extract` pointer is nullptr for ecosystems whose deps live in
    // the manifest scanner's graph edges (Maven via pom.xml /
    // build.gradle) or where no extractor exists yet (CMakeLists.txt,
    // meson.build, configure.ac, Makefile).
    using DepExtractor = std::vector<std::string> (*)(const std::filesystem::path&);
    struct ManifestEntry
    {
        std::string_view filename;
        Runtime runtime;
        DepExtractor extract;
    };
    static constexpr std::array<ManifestEntry, 14> k_manifests = {{
        {"go.mod", Runtime::Go, &deps::extract_go_mod},
        {"composer.json", Runtime::Php, &deps::extract_composer},
        {"Gemfile", Runtime::Ruby, &deps::extract_gemfile},
        {"pyproject.toml", Runtime::Python, &deps::extract_pyproject},
        {"Cargo.toml", Runtime::Rust, &deps::extract_cargo},
        {"pom.xml", Runtime::Java, nullptr},
        {"build.gradle.kts", Runtime::Java, nullptr},
        {"build.gradle", Runtime::Java, nullptr},
        {"package.json", Runtime::NodeJs, &deps::extract_npm},
        {"CMakeLists.txt", Runtime::CCpp, nullptr},
        {"setup.py", Runtime::Python, &deps::extract_setup_py},
        {"meson.build", Runtime::CCpp, nullptr},
        {"configure.ac", Runtime::CCpp, nullptr},
        {"Makefile", Runtime::CCpp, nullptr},
    }};
    for (const auto& [filename, runtime, extract] : k_manifests) {
        const auto manifest_path = project_root / filename;
        if (std::filesystem::exists(manifest_path, ec) && !ec) {
            std::vector<std::string> runtime_deps;
            if (extract != nullptr) {
                runtime_deps = extract(manifest_path);
            }
            return ManifestInfo{runtime, std::string{filename}, std::move(runtime_deps)};
        }
        ec.clear();
    }

    // C# names are user-defined; .sln takes precedence over .csproj.
    // Capture the iterator separately so a constructor-time ec on an
    // unreadable project root produces a clean nullopt rather than a
    // garbage first iteration (matches the pattern used by the
    // ecosystem library detectors).
    std::filesystem::directory_iterator it(project_root, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : it) {
        if (ec) {
            break;
        }
        const auto ext = entry.path().extension().string();
        if (ext == ".sln") {
            return ManifestInfo{Runtime::DotNetSolution, entry.path().filename().string(), {}};
        }
        if (ext == ".csproj") {
            return ManifestInfo{Runtime::CSharp, entry.path().filename().string(), {}};
        }
    }
    return std::nullopt;
}

/// `dir:<name>` token used in `ArchitectureDescription::signals`.
[[nodiscard]] inline std::string dir_signal(std::string_view dir_name)
{
    return "dir:" + std::string{dir_name};
}

/// For each name in `names` that appears in `segments`, append a
/// `dir:<name>` token to `signals`. Hides the `unordered_set::contains`
/// + `emplace_back` pair that recurs across each layered-shape branch.
template <typename Range>
void push_matching_dir_signals(std::vector<std::string>& signals,
                               const std::unordered_set<std::string>& segments, const Range& names)
{
    for (const std::string_view name : names) {
        if (segments.contains(std::string{name})) {
            signals.emplace_back(dir_signal(name));
        }
    }
}

/// Append `runtime:<label>` and `manifest:<filename>` tokens to
/// `signals`. Every branch that surfaces a manifest needs both, so the
/// pair lives in one place.
inline void push_manifest_signals(std::vector<std::string>& signals, const ManifestInfo& manifest)
{
    signals.emplace_back("runtime:" + std::string{runtime_label(manifest.runtime)});
    signals.emplace_back("manifest:" + manifest.filename);
}

/// Library-detection result — reasoning + confidence + structured
/// signal tokens per ecosystem. Same prose-vs-tokens split as
/// `WorkspaceHit` and `ArchitectureDescription`.
struct LibraryHit
{
    std::string reasoning;
    std::vector<std::string> signals;
    std::uint8_t confidence = 0;
};

/// Confidence for an unambiguous declaration (gemspec at root,
/// composer.json with `"type": "library"`).
constexpr std::uint8_t k_library_explicit_confidence = 85;

/// Confidence for a layout-inferred library (Node `main`/`index.*`
/// entry, Python `<pkg>/__init__.py` shape, PHP autoload without
/// `index.php`).
constexpr std::uint8_t k_library_inferred_confidence = 80;

/// Node.js library: substring-match `package.json` for `main`/`exports`/
/// `module` (avoids dragging in a JSON parser; the 64 KB cap is plenty),
/// and fall back to a conventional `index.*` at the root or under `src/`
/// when those defaults are implicit (express 5.x ships no `main`).
[[nodiscard]] std::optional<LibraryHit>
detect_node_library(const std::filesystem::path& project_root)
{
    const std::string pkg = read_text_capped(project_root / "package.json");
    if (pkg.empty()) {
        return std::nullopt;
    }
    // Apps in monorepos are commonly marked private to block accidental
    // publishes — a strong signal that this isn't a library.
    if (pkg.find(R"("private": true)") != std::string::npos ||
        pkg.find(R"("private":true)") != std::string::npos) {
        return std::nullopt;
    }
    const auto hit = [](std::string entry, std::string signal_value) {
        return LibraryHit{
            "Node.js library (package.json with " + std::move(entry) + ", not marked private)",
            {"library:node", "manifest:package.json", "node-entry:" + std::move(signal_value)},
            k_library_inferred_confidence};
    };
    // Match each entry field as a JSON key (followed by `:` with optional
    // whitespace) so a `"keywords": ["main", "module"]` array — common in
    // npm packages — doesn't trip the substring check and mislabel an
    // application as a library.
    const auto has_entry_key = [&pkg](std::string_view name) {
        const std::string with_colon = std::string{"\""} + std::string{name} + "\":";
        const std::string with_space = std::string{"\""} + std::string{name} + "\" :";
        return pkg.find(with_colon) != std::string::npos ||
               pkg.find(with_space) != std::string::npos;
    };
    if (has_entry_key("main")) {
        return hit("`main`/`exports`/`module` entry", "main");
    }
    if (has_entry_key("exports")) {
        return hit("`main`/`exports`/`module` entry", "exports");
    }
    if (has_entry_key("module")) {
        return hit("`main`/`exports`/`module` entry", "module");
    }
    static constexpr std::array<std::string_view, 8> k_index_paths = {
        "index.js",     "index.mjs",     "index.cjs",     "index.ts",
        "src/index.js", "src/index.mjs", "src/index.cjs", "src/index.ts",
    };
    std::error_code ec;
    for (const std::string_view rel : k_index_paths) {
        ec.clear();
        if (std::filesystem::exists(project_root / rel, ec) && !ec) {
            return hit(std::string{"`"} + std::string{rel} + "` entry", std::string{rel});
        }
    }
    return std::nullopt;
}

/// PHP library: explicit `"type": "library"` is unambiguous; otherwise
/// composer.json with autoload declarations and no `index.php` entry
/// at the project root is the typical PSR-4 package shape.
[[nodiscard]] std::optional<LibraryHit>
detect_php_library(const std::filesystem::path& project_root)
{
    const std::string composer = read_text_capped(project_root / "composer.json");
    if (composer.empty()) {
        return std::nullopt;
    }
    if (composer.find(R"("type": "library")") != std::string::npos ||
        composer.find(R"("type":"library")") != std::string::npos) {
        return LibraryHit{R"(PHP library (composer.json `"type": "library"`))",
                          {"library:php", "manifest:composer.json", "composer-type:library"},
                          k_library_explicit_confidence};
    }
    std::error_code ec;
    const bool has_index_php = std::filesystem::exists(project_root / "index.php", ec) && !ec;
    ec.clear();
    const bool has_public_index =
        std::filesystem::exists(project_root / "public" / "index.php", ec) && !ec;
    // Match `"autoload":` as a JSON key, not the bare token, so a
    // description like "PSR-4 autoload helper" can't trip the check.
    const bool has_autoload_key = composer.find(R"("autoload":)") != std::string::npos ||
                                  composer.find(R"("autoload" :)") != std::string::npos;
    if (!has_index_php && !has_public_index && has_autoload_key) {
        return LibraryHit{
            "PHP library (composer.json with autoload, no `index.php` entry)",
            {"library:php", "manifest:composer.json", "composer-autoload", "no-index-php"},
            k_library_inferred_confidence};
    }
    return std::nullopt;
}

/// Ruby gem: any `*.gemspec` at the project root. Nothing else looks
/// like one, and a gemspec is the canonical "this is a packagable gem"
/// declaration.
[[nodiscard]] std::optional<LibraryHit>
detect_ruby_library(const std::filesystem::path& project_root)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(project_root, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : it) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path().extension().string() == ".gemspec") {
            return LibraryHit{"Ruby gem (" + entry.path().filename().string() + ")",
                              {"library:ruby", "manifest:" + entry.path().filename().string()},
                              k_library_explicit_confidence};
        }
    }
    return std::nullopt;
}

/// Python library: `pyproject.toml` or `setup.py` at the root plus a
/// discoverable package — both signals collected during the same
/// `collect_path_signals` walk that the rest of the detector uses, so
/// this helper is just a fact-check against `signals`.
[[nodiscard]] std::optional<LibraryHit>
detect_python_library(const std::filesystem::path& project_root, const PathSignals& signals)
{
    std::error_code ec;
    const bool has_pyproject = std::filesystem::exists(project_root / "pyproject.toml", ec) && !ec;
    ec.clear();
    const bool has_setup = std::filesystem::exists(project_root / "setup.py", ec) && !ec;
    if (!has_pyproject && !has_setup) {
        return std::nullopt;
    }
    if (signals.main_count > 0 || signals.has_python_app_entry || !signals.has_python_pkg_init) {
        return std::nullopt;
    }
    const std::string manifest_name = has_pyproject ? "pyproject.toml" : "setup.py";
    return LibraryHit{"Python library (" + manifest_name + " + `" + signals.python_sample_pkg +
                          "/__init__.py`, no app entry)",
                      {"library:python", "manifest:" + manifest_name,
                       "python-package:" + signals.python_sample_pkg, "no-app-entry"},
                      k_library_inferred_confidence};
}

/// Read the first `package <name>` clause from a Go file, skipping
/// leading line comments, block comments, and whitespace. Returns
/// `nullopt` on read failure or when the first non-comment token isn't
/// `package`. Reads at most 4 KB — Go files declare their package
/// within the first handful of lines or not at all.
[[nodiscard]] std::optional<std::string> extract_go_package(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    // Stack buffer + string_view view = no zero-fill, no heap allocation
    // for a 4 KB peek. Called once per top-level .go file in
    // detect_go_library, so the saving compounds on libraries with many
    // root source files.
    constexpr std::size_t k_peek_bytes = 4096;
    std::array<char, k_peek_bytes> stack_buf{};
    in.read(stack_buf.data(), k_peek_bytes);
    const std::string_view buf{stack_buf.data(), static_cast<std::size_t>(in.gcount())};

    std::size_t i = 0;
    while (i < buf.size()) {
        const char c = buf[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        if (i + 1 < buf.size() && buf[i] == '/' && buf[i + 1] == '/') {
            while (i < buf.size() && buf[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (i + 1 < buf.size() && buf[i] == '/' && buf[i + 1] == '*') {
            i += 2;
            while (i + 1 < buf.size() && (buf[i] != '*' || buf[i + 1] != '/')) {
                ++i;
            }
            if (i + 1 < buf.size()) {
                i += 2;
            }
            continue;
        }
        constexpr std::string_view k_pkg{"package"};
        if (buf.compare(i, k_pkg.size(), k_pkg) != 0) {
            return std::nullopt;
        }
        i += k_pkg.size();
        if (i >= buf.size() || (buf[i] != ' ' && buf[i] != '\t')) {
            return std::nullopt;
        }
        while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < buf.size() &&
               (std::isalnum(static_cast<unsigned char>(buf[i])) != 0 || buf[i] == '_')) {
            ++i;
        }
        if (i == start) {
            return std::nullopt;
        }
        return std::string{buf.substr(start, i - start)};
    }
    return std::nullopt;
}

/// True if `cmd/` contains any non-test `.go` file (the canonical Go
/// CLI layout: one binary per `cmd/<name>/` subdir). Bounded recursive
/// walk — exits as soon as the first hit is found.
[[nodiscard]] bool has_cmd_binary(const std::filesystem::path& project_root)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path cmd_dir = project_root / "cmd";
    if (!fs::is_directory(cmd_dir, ec) || ec) {
        return false;
    }
    // Capture the iterator separately so a constructor failure (e.g.
    // EACCES on the directory itself) is observable — the for-range
    // form would silently treat it as empty and return false, causing
    // detect_go_library to mislabel a CLI as a library.
    fs::recursive_directory_iterator it(cmd_dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return false;
    }
    for (const auto& sub : it) {
        if (ec) {
            return false;
        }
        if (sub.path().extension() != ".go") {
            continue;
        }
        const std::string fname = sub.path().filename().string();
        if (fname.ends_with("_test.go")) {
            continue;
        }
        return true;
    }
    return false;
}

/// Go library: top-level `*.go` files declare a non-`main` package
/// (the public package the module exports), and there's no root
/// `package main` file or `cmd/<name>/` binary subtree. The check
/// runs only after API-Backend / MVC / Layered have already had a
/// chance to fire, so app-shaped Go projects with framework
/// directories don't reach here.
[[nodiscard]] std::optional<LibraryHit> detect_go_library(const std::filesystem::path& project_root)
{
    namespace fs = std::filesystem;
    if (has_cmd_binary(project_root)) {
        return std::nullopt;
    }
    bool saw_non_main_pkg = false;
    std::string sample_pkg;
    std::error_code ec;
    fs::directory_iterator it(project_root, ec);
    if (ec) {
        return std::nullopt;
    }
    for (const auto& entry : it) {
        if (ec) {
            // Mid-walk error — a Library hit drawn from partial evidence
            // would be unsafe (we may have missed a `main.go`). Refuse
            // to classify rather than guess.
            return std::nullopt;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto& path = entry.path();
        if (path.extension() != ".go") {
            continue;
        }
        const std::string fname = path.filename().string();
        if (fname.ends_with("_test.go")) {
            continue;
        }
        const auto pkg = extract_go_package(path);
        if (!pkg) {
            continue;
        }
        if (*pkg == "main") {
            return std::nullopt;
        }
        saw_non_main_pkg = true;
        if (sample_pkg.empty()) {
            sample_pkg = *pkg;
        }
    }
    if (!saw_non_main_pkg) {
        return std::nullopt;
    }
    return LibraryHit{"Go library (go.mod + root `package " + sample_pkg +
                          "`, no `main` package or `cmd/` binary)",
                      {"library:go", "manifest:go.mod", "go-package:" + sample_pkg,
                       "no-main-package", "no-cmd-binary"},
                      k_library_inferred_confidence};
}

/// Dispatch on manifest runtime to pick the right ecosystem detector.
[[nodiscard]] std::optional<LibraryHit>
detect_ecosystem_library(const std::filesystem::path& project_root, const ManifestInfo& manifest,
                         const PathSignals& signals)
{
    switch (manifest.runtime) {
    case Runtime::NodeJs:
        return detect_node_library(project_root);
    case Runtime::Php:
        return detect_php_library(project_root);
    case Runtime::Ruby:
        return detect_ruby_library(project_root);
    case Runtime::Python:
        return detect_python_library(project_root, signals);
    case Runtime::Go:
        return detect_go_library(project_root);
    case Runtime::Java:
    case Runtime::CCpp:
    case Runtime::Rust:
    case Runtime::CSharp:
    case Runtime::DotNetSolution:
        break;
    }
    return std::nullopt;
}

/// True if the index holds a Spring `<beans>` XML file (Language::
/// SpringXml) at the project root or under `src/main/resources/`.
/// Drives the `manifest:applicationContext.xml` architecture signal.
[[nodiscard]] bool has_spring_context_xml(std::span<const FileEntry> files)
{
    return std::ranges::any_of(files, [](const FileEntry& file) {
        if (file.language != Language::SpringXml) {
            return false;
        }
        const std::string rel = file.path_relative.generic_string();
        return rel.find('/') == std::string::npos || rel.starts_with("src/main/resources/");
    });
}

/// True if the index holds an `application.properties` file (Java
/// properties with that exact basename) at the project root or under
/// `src/main/resources/`. Drives the `manifest:application.properties`
/// architecture signal. The basename is intentionally specific — only
/// Spring Boot's canonical config file raises the signal; arbitrary
/// `.properties` files (logging configs, i18n bundles, etc.) do not.
[[nodiscard]] bool has_application_properties(std::span<const FileEntry> files)
{
    return std::ranges::any_of(files, [](const FileEntry& file) {
        if (file.language != Language::Properties) {
            return false;
        }
        if (file.path_relative.filename() != "application.properties") {
            return false;
        }
        const std::string rel = file.path_relative.generic_string();
        if (rel.find('/') == std::string::npos) {
            return true; // root-level
        }
        return rel.starts_with("src/main/resources/");
    });
}

} // namespace

std::string_view architecture_label_name(ArchitectureLabel label) noexcept
{
    switch (label) {
    case ArchitectureLabel::Monolith:
        return "Monolith";
    case ArchitectureLabel::Layered:
        return "Layered";
    case ArchitectureLabel::Mvc:
        return "MVC";
    case ArchitectureLabel::Monorepo:
        return "Monorepo";
    case ArchitectureLabel::FrontendSpa:
        return "Frontend SPA";
    case ArchitectureLabel::ApiBackend:
        return "API Backend";
    case ArchitectureLabel::Mvvm:
        return "MVVM";
    case ArchitectureLabel::CleanArchitecture:
        return "Clean Architecture";
    case ArchitectureLabel::DotNetSolution:
        return ".NET Solution";
    case ArchitectureLabel::Library:
        return "Library";
    case ArchitectureLabel::Electron:
        return "Electron";
    case ArchitectureLabel::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

// TU-local: the public detect_architecture wrapper below is the only
// entry point, so the manifest signal is never silently skipped.
// `manifest` is detected once in the wrapper and threaded through both
// the impl and the hint emitters — that avoids re-parsing the root
// manifest a second time inside emit_framework_hint_signals.
static ArchitectureDescription
detect_architecture_impl(std::span<const FileEntry> files,
                         const std::filesystem::path& project_root,
                         const std::optional<ManifestInfo>& manifest,
                         const std::unordered_set<std::string>& exclude_dir_names)
{
    ArchitectureDescription out;

    const std::size_t file_count = files.size();
    if (file_count == 0) {
        out.label = ArchitectureLabel::Unknown;
        out.reasoning = "no source files found";
        // Empty signals — nothing fired.
        out.confidence = 0;
        return out;
    }

    // --- Workspace-manifest indicators (highest-confidence monorepos)
    // Before the heuristic pass, consult concrete build-system manifests
    // at the project root. They are unambiguous signals — a project
    // with `Cargo.toml [workspace]` IS a Cargo workspace, regardless of
    // directory shape. Accuracy over guesswork.
    if (!project_root.empty()) {
        // Cargo.toml `[workspace]`, pnpm-workspace.yaml, lerna.json:
        // the manifest IS the declaration. Top of the rubric (95-100).
        if (auto r = detect_rust_workspace(project_root); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(r->reasoning);
            out.signals = std::move(r->signals);
            out.confidence = 95;
            return out;
        }
        if (auto r = detect_npm_monorepo(project_root); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(r->reasoning);
            out.signals = std::move(r->signals);
            out.confidence = 95;
            return out;
        }
        // pyproject.toml + 2+ src-layout packages — the manifest names
        // the project but the multi-package shape is *inferred* from
        // the filesystem rather than declared. Strong corroborated
        // (85-94) rather than fully deterministic.
        if (auto r = detect_python_packages(project_root, files); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(r->reasoning);
            out.signals = std::move(r->signals);
            out.confidence = 90;
            return out;
        }
    }

    auto signals = collect_path_signals(files);
    // Empty caller-supplied set means "no overrides" — fall back to the
    // canonical scanner list so the disk walk still skips noise dirs.
    const auto& effective_excludes =
        exclude_dir_names.empty() ? default_scanner_exclude_dir_names() : exclude_dir_names;
    augment_signals_from_disk(signals, project_root, effective_excludes);
    const auto& segments = signals.segments;
    const int runtime_pct = manifest ? runtime_share(signals, manifest->runtime) : 0;
    const std::size_t main_count = signals.main_count;

    // --- Monorepo indicators ---------------------------------------
    // Top-level `packages/`, `apps/`, `libs/` plus multiple main.*
    // files typically mean an npm / cargo / bazel monorepo.
    const bool has_packages = segments.contains("packages");
    const bool has_apps = segments.contains("apps");
    const bool has_libs = segments.contains("libs");
    const bool has_packages_top = has_packages || has_apps || has_libs;
    if (has_packages_top && main_count >= 2) {
        // Cite the directory that actually matched — a reasoning
        // string claiming "`packages/` or `apps/`" when only `libs/`
        // fired is exactly the template hallucination consumers
        // reported, and erodes trust in the whole classifier.
        const char* which = "libs";
        if (has_packages) {
            which = "packages";
        }
        else if (has_apps) {
            which = "apps";
        }
        out.label = ArchitectureLabel::Monorepo;
        out.reasoning = std::string{"top-level `"} + which + "/` plus " +
                        std::to_string(main_count) + " entry points suggests a monorepo layout";
        out.signals = {dir_signal(which), "entry-points:" + std::to_string(main_count)};
        out.confidence = 85;
        return out;
    }

    // --- Layered / MVC indicators ----------------------------------
    //
    // Any of these top-level/second-level directory names signals a
    // layered design. The list covers three patterns:
    //   - Classic "controllers/services/repositories/models" layering
    //   - Hexagonal / clean architecture ("adapters", "ports",
    //     "domain", "infrastructure")
    //   - Plugin / modular layering ("core", "modes", "plugins",
    //     "ui", "platform") — this is the shape Vectis itself has
    //     and which the original list was missing.
    const std::vector<std::string_view> layered_signals = {
        // Classic layered (plural — Rails, JS, modern Java)
        "controllers", "services", "repositories", "models", "handlers", "routes",
        // Singular variants (Spring Boot Java idiom: rest/, service/,
        // repository/, model/) so the calibration corpus's spring-
        // petclinic-rest doesn't fall through to Monolith.
        "controller", "service", "repository", "model", "handler", "route", "rest",
        // Domain / DAO
        "domain", "dao",
        // Plugin / modular layering
        "core", "modes", "plugins", "ui", "platform",
        // Hexagonal / clean architecture
        "adapters", "ports", "infrastructure", "infra", "engine",
        // Go Clean Architecture (bxcodec pattern)
        "usecase", "usecases", "delivery",
        // Mature C/C++ codebases (postgres has src/backend/+src/bin/+
        // src/include/+src/interfaces/; openssl has crypto/+ssl/+apps/
        // +engines/; sqlite has src/+tool/+ext/).
        "backend", "interfaces", "subsystems"};
    // NOTE: `apis` is intentionally NOT here — it's an ApiBackend
    // trigger, and ApiBackend takes precedence over the Layered
    // fallback below.
    std::size_t layered_matches = 0;
    std::string layered_reason = "found: ";
    bool first_match = true;
    for (const std::string_view needle : layered_signals) {
        if (segments.contains(std::string{needle})) {
            ++layered_matches;
            if (!first_match) {
                layered_reason += ", ";
            }
            layered_reason += std::string{needle};
            first_match = false;
        }
    }

    // --- Clean Architecture / onion / hexagonal ---------------------
    // Strong signal: at least two of Domain/Application/Infrastructure/
    // Presentation (case-insensitive is unnecessary — filesystems on
    // Windows are case-insensitive but the convention is PascalCase).
    const auto has_any = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::any_of(
            names, [&](std::string_view n) { return segments.contains(std::string{n}); });
    };
    const int clean_hits =
        (segments.contains("Domain") ? 1 : 0) + (segments.contains("Application") ? 1 : 0) +
        (segments.contains("Infrastructure") ? 1 : 0) + (segments.contains("Presentation") ? 1 : 0);
    if (clean_hits >= 3) {
        out.label = ArchitectureLabel::CleanArchitecture;
        out.reasoning = "Domain/Application/Infrastructure/Presentation "
                        "layering (Clean / hexagonal architecture)";
        out.signals = {"layout:clean-architecture"};
        push_matching_dir_signals(out.signals, segments,
                                  std::initializer_list<std::string_view>{
                                      "Domain", "Application", "Infrastructure", "Presentation"});
        out.confidence = 85;
        return out;
    }
    // Go Clean Architecture — lowercase domain/repository/usecase/
    // delivery layout from the bxcodec-go-clean-arch tradition, plus
    // its modern variants where `delivery` collapsed into `rest` and
    // `usecase` became `application` or `app`.
    const auto any_segment = [&](std::initializer_list<std::string_view> names) -> bool {
        return std::ranges::any_of(
            names, [&](std::string_view n) { return segments.contains(std::string{n}); });
    };
    const int clean_go_hits = (segments.contains("domain") ? 1 : 0) +
                              (any_segment({"repository", "repositories"}) ? 1 : 0) +
                              (any_segment({"usecase", "usecases", "application"}) ? 1 : 0) +
                              (any_segment({"delivery", "rest", "handlers"}) ? 1 : 0);
    if (clean_go_hits >= 3) {
        out.label = ArchitectureLabel::CleanArchitecture;
        out.reasoning = "domain/repository/usecase/delivery layering "
                        "(Go-style Clean Architecture)";
        out.signals = {"layout:clean-architecture-go"};
        push_matching_dir_signals(out.signals, segments,
                                  std::initializer_list<std::string_view>{
                                      "domain", "repository", "repositories", "usecase", "usecases",
                                      "application", "delivery", "rest", "handlers"});
        out.confidence = 80;
        return out;
    }

    // --- MVVM --------------------------------------------------------
    // ViewModels + Views together is the defining signature. Many
    // modern .NET UI frameworks (WPF, Avalonia, MAUI) put each in its
    // own folder inside a UI project.
    if (segments.contains("ViewModels") && segments.contains("Views")) {
        out.label = ArchitectureLabel::Mvvm;
        out.reasoning = "`ViewModels/` and `Views/` directories "
                        "(MVVM UI layer)";
        out.signals = {"layout:mvvm", "dir:ViewModels", "dir:Views"};
        out.confidence = 85;
        return out;
    }

    // --- .NET multi-project solution ---------------------------------
    // Several sibling directories with dotted names like
    // `FlowForge.UI` / `FlowForge.CLI` / `FlowForge.Core` strongly
    // suggest a `.sln` with one `.csproj` per dotted directory.
    if (count_dotted_project_dirs(files) >= 2) {
        out.label = ArchitectureLabel::DotNetSolution;
        out.reasoning = "multiple sibling directories with dotted "
                        "names (typical .NET multi-project solution)";
        out.signals = {"layout:dotnet-solution", "dotted-project-dirs"};
        out.confidence = 75;
        // A `.sln` at the project root corroborates the dotted-dir
        // signal — heuristic + manifest agreement is high-confidence.
        if (manifest && manifest->runtime == Runtime::DotNetSolution) {
            out.reasoning += " — confirmed by " + manifest->filename;
            out.signals.emplace_back("manifest:" + manifest->filename);
            out.confidence = 90;
        }
        return out;
    }

    // MVC is a stricter layered variant: it needs `models`,
    // `views`, and `controllers` (or equivalent) all present.
    const bool has_mvc_trio = segments.contains("models") && segments.contains("views") &&
                              segments.contains("controllers");
    if (has_mvc_trio) {
        out.label = ArchitectureLabel::Mvc;
        out.reasoning = "found `models/`, `views/`, and `controllers/` "
                        "directories (classic MVC layout)";
        out.signals = {"layout:mvc", "dir:models", "dir:views", "dir:controllers"};
        out.confidence = 90;
        return out;
    }
    (void)has_any; // reserved for future layered heuristics

    // --- Electron desktop app --------------------------------------
    // Shared with the SPA branch below: one read_text_capped call,
    // both substring probes share the buffer. Substring `"electron"`
    // (with both quotes) keeps the package.json check tight enough
    // not to fire on `"electron-builder"` / `"react-electron"` keys.
    const std::string pkg_json_text =
        project_root.empty() ? std::string{} : read_text_capped(project_root / "package.json");
    const bool has_electron_dep =
        !pkg_json_text.empty() && pkg_json_text.find(R"("electron")") != std::string::npos;
    const bool has_electron_config =
        !project_root.empty() &&
        std::ranges::any_of(k_electron_root_configs, [&](std::string_view n) {
            std::error_code ec;
            return std::filesystem::exists(project_root / n, ec) && !ec;
        });

    if (has_electron_dep || has_electron_config) {
        out.label = ArchitectureLabel::Electron;
        out.signals = {"layout:electron"};
        if (has_electron_dep && has_electron_config) {
            out.reasoning = "electron in package.json dependencies + forge/builder config";
            out.signals.emplace_back("dep:electron");
            out.signals.emplace_back("electron-config");
            out.signals.emplace_back("manifest:package.json");
            out.confidence = 90;
        }
        else if (has_electron_dep) {
            out.reasoning = "electron in package.json dependencies";
            out.signals.emplace_back("dep:electron");
            out.signals.emplace_back("manifest:package.json");
            out.confidence = 85;
        }
        else {
            out.reasoning = "electron-forge / electron-builder config at project root";
            out.signals.emplace_back("electron-config");
            out.confidence = 80;
        }
        return out;
    }

    // --- Frontend SPA indicators -----------------------------------
    // src/components/ + src/pages/, or a framework config at the
    // project root. Root scope on the config probe is load-bearing:
    // backends ship one-off embedded mini-apps deep in their tree
    // (exception-page renderers, etc.) and a whole-tree match would
    // mislabel them as SPA.
    const bool has_components = segments.contains("components");
    const bool has_pages = segments.contains("pages");
    const bool has_spa_config =
        !project_root.empty() && std::ranges::any_of(k_spa_root_configs, [&](std::string_view n) {
            std::error_code ec;
            return std::filesystem::exists(project_root / n, ec) && !ec;
        });
    // Older React apps (CRA, react-scripts) ship no framework config
    // file; the package.json's `react-scripts` dependency is the
    // closest stable marker. Only Node-runtime projects qualify.
    const bool has_cra_marker = manifest && manifest->runtime == Runtime::NodeJs &&
                                pkg_json_text.find(R"("react-scripts")") != std::string::npos;
    if ((has_components && has_pages) || has_spa_config || has_cra_marker) {
        out.label = ArchitectureLabel::FrontendSpa;
        if (has_spa_config) {
            out.reasoning = "framework config file detected (next/vite/nuxt)";
            out.signals = {"layout:frontend-spa", "spa-framework-config"};
        }
        else if (has_cra_marker) {
            out.reasoning = "react-scripts (Create React App) in package.json";
            out.signals = {"layout:frontend-spa", "react-scripts", "manifest:package.json"};
        }
        else {
            out.reasoning = "found `components/` and `pages/` directories";
            out.signals = {"layout:frontend-spa", "dir:components", "dir:pages"};
        }
        out.confidence = 80;
        return out;
    }

    // --- API backend indicators ------------------------------------
    // A handlers/, routes/, routers/, or apis/ directory plus no
    // src/pages and no frontend config implies an API backend.
    // `routers/` is the Beego/Casdoor convention; `apis/` is the
    // PocketBase / Go-microservice convention. ApiBackend runs
    // BEFORE the generic Layered fallback so an API project that
    // happens to have several layered-name directories (pocketbase
    // has core/+plugins/+ui/+models/) doesn't get the more generic
    // "Layered" label when the more specific one fits.
    const bool has_handlers = segments.contains("handlers");
    const bool has_routes = segments.contains("routes");
    const bool has_routers = segments.contains("routers");
    const bool has_apis = segments.contains("apis");
    if ((has_handlers || has_routes || has_routers || has_apis) && !has_pages && !has_spa_config) {
        out.label = ArchitectureLabel::ApiBackend;
        out.signals = {"layout:api-backend"};
        if (has_apis) {
            out.reasoning = "found `apis/` directory, no frontend config";
            out.signals.emplace_back("dir:apis");
        }
        else if (has_handlers) {
            out.reasoning = "found `handlers/` directory, no frontend config";
            out.signals.emplace_back("dir:handlers");
        }
        else if (has_routes) {
            out.reasoning = "found `routes/` directory, no frontend config";
            out.signals.emplace_back("dir:routes");
        }
        else {
            out.reasoning = "found `routers/` directory, no frontend config";
            out.signals.emplace_back("dir:routers");
        }
        out.confidence = 60;
        if (manifest) {
            out.reasoning += " (" + std::string{runtime_label(manifest->runtime)} + " project)";
            push_manifest_signals(out.signals, *manifest);
            out.confidence = 75;
        }
        return out;
    }

    // --- Layered fallback ------------------------------------------
    if (layered_matches >= 3) {
        out.label = ArchitectureLabel::Layered;
        out.reasoning = layered_reason;
        out.signals = {"layout:layered"};
        push_matching_dir_signals(out.signals, segments, layered_signals);
        out.confidence = 70;
        return out;
    }

    // --- Library indicators ----------------------------------------
    // (1) Canonical C/C++ shape: top-level `include/` (or `lib/`) +
    //     `src/` and no entry point. Top-level scope is load-bearing —
    //     a server vendoring `deps/<lib>/include/` would otherwise
    //     bubble up that nested `include/` and mislabel.
    const auto& top = signals.top_level_dirs;
    const bool has_top_include = top.contains("include");
    if ((has_top_include || top.contains("lib")) && top.contains("src") && main_count == 0) {
        out.label = ArchitectureLabel::Library;
        out.reasoning = has_top_include
                            ? "found `include/` + `src/` with no entry point — library layout"
                            : "found `lib/` + `src/` with no entry point — library layout";
        out.signals = {"layout:library", "dir:src", "no-entry-point"};
        out.signals.emplace_back(has_top_include ? "dir:include" : "dir:lib");
        out.confidence = 75;
        if (manifest) {
            out.reasoning += " (" + std::string{runtime_label(manifest->runtime)} + " project)";
            push_manifest_signals(out.signals, *manifest);
            out.confidence = 85;
        }
        return out;
    }

    // (2) Per-ecosystem signals for Node, PHP, Ruby, Python framework-
    //     libraries (express, Slim, sinatra, flask, …) that previously
    //     read 65% Monolith because they don't follow C/C++ layout.
    if (manifest) {
        if (auto hit = detect_ecosystem_library(project_root, *manifest, signals); hit) {
            out.label = ArchitectureLabel::Library;
            out.reasoning = std::move(hit->reasoning);
            out.signals = std::move(hit->signals);
            out.confidence = hit->confidence;
            return out;
        }
    }

    // --- Default: Monolith -----------------------------------------
    // A root build manifest pins higher confidence than "we have no
    // idea". A coherent codebase (manifest + ≥75% language match)
    // pins higher still.
    // Allow projects with up to 2 main entry points (a primary main
    // plus a single auxiliary tool / wasm variant — the watchtower
    // shape) to fall through to Monolith. Monorepos with more
    // entry points fire the Monorepo branch above. With a manifest
    // present we always commit to Monolith rather than Unknown,
    // since "I don't know what this project is" is more confusing
    // than "single Java/Go/etc. application without a recognised
    // layout" at low confidence.
    if (main_count <= 2 || layered_matches > 0 || manifest) {
        out.label = ArchitectureLabel::Monolith;
        const bool coherent = manifest && runtime_pct >= 75;
        const std::string lbl{manifest ? runtime_label(manifest->runtime) : ""};
        const std::string manifest_suffix =
            manifest ? " — " + lbl + " (" + manifest->filename + ")" : std::string{};
        const std::string coherence_suffix =
            coherent ? ", " + std::to_string(runtime_pct) + "% " + lbl + "-dominant"
                     : std::string{};
        out.signals = {"layout:monolith"};
        if (main_count > 0) {
            out.signals.emplace_back("entry-points:" + std::to_string(main_count));
        }
        if (manifest) {
            push_manifest_signals(out.signals, *manifest);
            if (coherent) {
                out.signals.emplace_back("runtime-coherent:" + std::to_string(runtime_pct));
            }
        }
        if (layered_matches > 0) {
            out.reasoning = "single entry point with partial layering (" + layered_reason + ")" +
                            manifest_suffix + coherence_suffix;
            out.signals.emplace_back("partial-layering");
            push_matching_dir_signals(out.signals, segments, layered_signals);
            int conf = 55;
            if (coherent) {
                conf = 80;
            }
            else if (manifest) {
                conf = 70;
            }
            out.confidence = conf;
        }
        else if (manifest) {
            out.reasoning = lbl + " project (" + manifest->filename + ")" + coherence_suffix +
                            ", no distinctive layout";
            out.confidence = coherent ? 75 : 60;
        }
        else {
            out.reasoning = "single entry point, no distinctive layout";
            out.confidence = 45;
        }
        return out;
    }

    // Fallback — nothing matched.
    out.label = ArchitectureLabel::Unknown;
    out.reasoning = "no distinctive architectural indicators found";
    // Empty signals — by definition, nothing fired.
    out.confidence = 10;
    return out;
}

namespace {

/// Map the root-manifest filename to the keyword-table ecosystem used
/// by `hints::match`. Returns nullopt for runtimes whose manifests
/// don't yet have an extractor (Maven's `build.gradle.kts` would
/// match, but the framework matcher has no Gradle dep data — empty
/// table lookups are a no-op so this is safe to over-include).
[[nodiscard]] std::optional<hints::Ecosystem>
ecosystem_for_manifest(std::string_view filename) noexcept
{
    if (filename == "package.json") {
        return hints::Ecosystem::Npm;
    }
    if (filename == "pyproject.toml" || filename == "setup.py") {
        return hints::Ecosystem::Pyproject;
    }
    if (filename == "Cargo.toml") {
        return hints::Ecosystem::Cargo;
    }
    if (filename == "go.mod") {
        return hints::Ecosystem::GoMod;
    }
    if (filename == "composer.json") {
        return hints::Ecosystem::Composer;
    }
    if (filename == "Gemfile") {
        return hints::Ecosystem::Gemfile;
    }
    if (filename == "pom.xml" || filename == "build.gradle" || filename == "build.gradle.kts") {
        return hints::Ecosystem::Maven;
    }
    if (filename.ends_with(".csproj") || filename.ends_with(".sln") ||
        filename.ends_with(".slnx") || filename.ends_with(".vbproj") ||
        filename.ends_with(".fsproj")) {
        return hints::Ecosystem::DotNet;
    }
    return std::nullopt;
}

/// For Java/.NET ecosystems, augment `deps` with the dep-edge coords
/// already in the index (the manifest scanner emits `maven*` and
/// `csproj-package` edges). For other ecosystems the file-based
/// extractor in `detect_root_manifest` already populated `deps`, so
/// this is a no-op.
void augment_with_index_deps(std::vector<std::string>& deps, hints::Ecosystem eco,
                             const CodeIndex& index)
{
    if (eco != hints::Ecosystem::Maven && eco != hints::Ecosystem::DotNet) {
        return;
    }
    for (const auto& edge : index.all_dependencies()) {
        const bool is_maven_kind =
            edge.kind == "maven" || edge.kind == "maven-managed" || edge.kind == "maven-bom";
        const bool is_dotnet_kind = edge.kind == "csproj-package";
        const bool keep = (eco == hints::Ecosystem::Maven && is_maven_kind) ||
                          (eco == hints::Ecosystem::DotNet && is_dotnet_kind);
        if (keep) {
            deps.push_back(edge.import_string);
        }
    }
}

/// Raise `out.confidence` based on corroborators. Framework hints
/// (manifest deps and annotation tallies) push web/UI labels into the
/// strong-corroborated band; a library-only root CMake target pushes
/// the C/C++ Library label into the deterministic-manifest band
/// because `add_library` IS the build-system saying "this is a
/// library". Each lift is capped per the rubric so a weak starting
/// value can't over-shoot.
void apply_hint_corroborator_lifts(ArchitectureDescription& out,
                                   const std::set<hints::FrameworkHint>& fired,
                                   std::optional<cmake::RootTargetShape> cmake_target)
{
    const bool web_backend = fired.contains(hints::FrameworkHint::WebBackend);
    const bool web_frontend = fired.contains(hints::FrameworkHint::WebFrontend);

    const auto lift = [&out](std::uint8_t cap) {
        // +15-point lift: a corroborator typically moves a value into
        // the next rubric band (60→75 weak→strong-single,
        // 75→90 strong-single→strong-corroborated). The cap stops a
        // low-starting value from over-shooting.
        constexpr std::uint8_t k_lift_step = 15;
        out.confidence = static_cast<std::uint8_t>(
            std::min<int>(cap, static_cast<int>(out.confidence) + k_lift_step));
    };

    switch (out.label) {
    case ArchitectureLabel::ApiBackend:
        if (web_backend) {
            lift(/*cap=*/90);
        }
        break;
    case ArchitectureLabel::FrontendSpa:
        if (web_frontend) {
            lift(/*cap=*/90);
        }
        break;
    case ArchitectureLabel::Layered:
        if (web_backend) {
            lift(/*cap=*/85);
        }
        break;
    case ArchitectureLabel::CleanArchitecture:
        if (web_backend) {
            lift(/*cap=*/90);
        }
        break;
    case ArchitectureLabel::Library:
        // `add_library` at the root with no `add_executable` is a
        // build-system declaration that this is a library. Lift into
        // the deterministic-manifest band (95-100) where the rubric
        // places explicit project-shape declarations.
        if (cmake_target == cmake::RootTargetShape::LibraryOnly) {
            lift(/*cap=*/95);
        }
        break;
    default:
        // Monolith, Monorepo, MVC, MVVM, DotNetSolution, Electron,
        // Unknown — either already at a high confidence, covered by
        // a more specific detector, or not corroborable in a way the
        // rubric permits.
        break;
    }
}

/// Tally framework hints from both the manifest-dep matcher and the
/// annotation matcher in a single pass. Returns the deduped set; the
/// caller emits one `hint:*` token per element and consults the same
/// set when deciding confidence lifts (no string round-trip through
/// `out.signals`).
[[nodiscard]] std::set<hints::FrameworkHint>
collect_framework_hints(const std::optional<ManifestInfo>& manifest, const CodeIndex& index)
{
    std::set<hints::FrameworkHint> fired;

    // Manifest-dep path: only meaningful if the root manifest was
    // detected AND the filename maps to a supported ecosystem.
    if (manifest) {
        if (const auto eco = ecosystem_for_manifest(manifest->filename); eco) {
            std::vector<std::string> all_deps = manifest->runtime_deps;
            augment_with_index_deps(all_deps, *eco, index);
            for (const auto h : hints::match(*eco, all_deps)) {
                fired.insert(h);
            }
        }
    }

    // Annotation path: independent of manifest, fires on the symbol
    // AST's decorator stream. Uses `for_each_symbol_decorator` so we
    // never materialise the full symbol snapshot.
    std::vector<std::string_view> annotations;
    annotations.reserve(256);
    index.for_each_symbol_decorator([&](std::string_view dec) { annotations.emplace_back(dec); });
    // match_annotations takes `span<const string>`; copy the string_view
    // batch into a string vector for the API. This is the only required
    // allocation on the annotation path — far cheaper than copying every
    // Symbol's full payload via snapshot_all_symbols.
    std::vector<std::string> owned_annotations;
    owned_annotations.reserve(annotations.size());
    for (const auto sv : annotations) {
        owned_annotations.emplace_back(sv);
    }
    for (const auto h : hints::match_annotations(owned_annotations)) {
        fired.insert(h);
    }

    return fired;
}

} // namespace

ArchitectureDescription
detect_architecture(const CodeIndex& index, const std::filesystem::path& project_root,
                    const std::unordered_set<std::string>& exclude_dir_names)
{
    // Snapshot once, fan out to every helper. snapshot_files() copies
    // the file list; without this, each helper paid for its own copy.
    const std::vector<FileEntry> files_owned = index.snapshot_files();
    const std::span<const FileEntry> files{files_owned};

    // Detect the root manifest once. Threaded through the impl AND
    // the hint matcher so we don't re-read + re-parse it.
    const auto manifest = detect_root_manifest(project_root);

    ArchitectureDescription out =
        detect_architecture_impl(files, project_root, manifest, exclude_dir_names);
    if (has_spring_context_xml(files)) {
        out.signals.emplace_back("manifest:applicationContext.xml");
    }
    if (has_application_properties(files)) {
        out.signals.emplace_back("manifest:application.properties");
    }

    const auto fired_hints = collect_framework_hints(manifest, index);
    for (const auto h : fired_hints) {
        out.signals.emplace_back(hints::hint_signal(h));
    }

    const auto cmake_target = cmake::inspect_root(project_root);
    if (cmake_target == cmake::RootTargetShape::LibraryOnly) {
        out.signals.emplace_back(cmake::signal_for(*cmake_target));
    }
    apply_hint_corroborator_lifts(out, fired_hints, cmake_target);
    return out;
}

} // namespace vectis::code
