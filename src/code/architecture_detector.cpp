#include "code/architecture_detector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/language.h"
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
};

[[nodiscard]] PathSignals collect_path_signals(const CodeIndex& index)
{
    PathSignals out;
    for (const FileEntry& file : index.snapshot_files()) {
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

        if (!under_non_source) {
            const std::string name = file.path_relative.filename().string();
            if (name.starts_with("main.") || name == "main") {
                ++out.main_count;
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
[[nodiscard]] std::size_t count_dotted_project_dirs(const CodeIndex& index)
{
    std::unordered_set<std::string> dotted;
    for (const FileEntry& file : index.snapshot_files()) {
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
    for (const auto& top_entry : std::filesystem::directory_iterator(project_root, ec)) {
        if (ec) {
            return;
        }
        if (!top_entry.is_directory(ec) || ec) {
            continue;
        }
        const std::string top_name = top_entry.path().filename().string();
        if (skip(top_name)) {
            continue;
        }
        signals.top_level_dirs.insert(top_name);
        signals.segments.insert(top_name);
        for (const auto& sub_entry : std::filesystem::directory_iterator(top_entry.path(), ec)) {
            if (ec) {
                break;
            }
            if (!sub_entry.is_directory(ec) || ec) {
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

/// Detect a Cargo workspace: `Cargo.toml` at the project root containing
/// a `[workspace]` table header. Matches single-line `[workspace]` and
/// the uncommon variant with trailing content on the same line.
[[nodiscard]] std::optional<std::string>
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
            return std::string{"Cargo workspace (Rust multi-crate layout)"};
        }
    }
    return std::nullopt;
}

/// Detect an npm-family monorepo via one of the usual marker files at
/// the project root: `package.json` with a `"workspaces"` field,
/// `pnpm-workspace.yaml`, `lerna.json`, or `turbo.json`. Each hint is
/// reported with its own reasoning string so the digest tells you
/// which tool configured the monorepo.
[[nodiscard]] std::optional<std::string>
detect_npm_monorepo(const std::filesystem::path& project_root)
{
    std::error_code ec;
    if (std::filesystem::exists(project_root / "pnpm-workspace.yaml", ec) && !ec) {
        return std::string{"pnpm workspace (pnpm-workspace.yaml)"};
    }
    ec.clear();
    if (std::filesystem::exists(project_root / "lerna.json", ec) && !ec) {
        return std::string{"Lerna monorepo (lerna.json)"};
    }
    ec.clear();
    if (std::filesystem::exists(project_root / "turbo.json", ec) && !ec) {
        return std::string{"Turborepo (turbo.json)"};
    }

    const std::string pkg = read_text_capped(project_root / "package.json");
    if (!pkg.empty() && pkg.find("\"workspaces\"") != std::string::npos) {
        return std::string{"npm workspaces (package.json \"workspaces\")"};
    }
    return std::nullopt;
}

/// Detect a modern Python project layout. Two flavours:
///   * Multi-package workspace: `pyproject.toml` at root + multiple
///     subdirectories under `src/` that each contain `__init__.py`.
///   * Poetry / PEP 621 multi-package project (same, heuristically).
/// Returns a reasoning string if the signal fires, nullopt otherwise.
[[nodiscard]] std::optional<std::string>
detect_python_packages(const std::filesystem::path& project_root, const CodeIndex& index)
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
    for (const FileEntry& file : index.snapshot_files()) {
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
        return std::string{"pyproject.toml + src-layout with " +
                           std::to_string(src_packages.size()) + " packages"};
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
    static constexpr std::array<std::pair<std::string_view, Runtime>, 11> k_manifests = {{
        {"go.mod", Runtime::Go},
        {"composer.json", Runtime::Php},
        {"Gemfile", Runtime::Ruby},
        {"pyproject.toml", Runtime::Python},
        {"Cargo.toml", Runtime::Rust},
        {"pom.xml", Runtime::Java},
        {"build.gradle.kts", Runtime::Java},
        {"build.gradle", Runtime::Java},
        {"package.json", Runtime::NodeJs},
        {"CMakeLists.txt", Runtime::CCpp},
        {"setup.py", Runtime::Python},
    }};
    for (const auto& [filename, runtime] : k_manifests) {
        if (std::filesystem::exists(project_root / filename, ec) && !ec) {
            return ManifestInfo{runtime, std::string{filename}};
        }
        ec.clear();
    }

    // C# names are user-defined; .sln takes precedence over .csproj.
    for (const auto& entry : std::filesystem::directory_iterator(project_root, ec)) {
        if (ec) {
            break;
        }
        const auto ext = entry.path().extension().string();
        if (ext == ".sln") {
            return ManifestInfo{Runtime::DotNetSolution, entry.path().filename().string()};
        }
        if (ext == ".csproj") {
            return ManifestInfo{Runtime::CSharp, entry.path().filename().string()};
        }
    }
    return std::nullopt;
}

/// Library-detection result — reasoning + confidence per ecosystem.
struct LibraryHit
{
    std::string reasoning;
    std::uint8_t confidence = 0;
};

/// Node.js library: `package.json` declares an entry field (`main`,
/// `exports`, or `module`) — or, when those default to the conventional
/// `index.*` (express 5.x ships no `main`), there's a root `index.js`
/// or a `src/index.*` entry. Substring-match avoids dragging in a JSON
/// parser; the 64 KB cap on package.json is plenty for these checks.
[[nodiscard]] std::optional<std::string>
detect_node_library(const std::filesystem::path& project_root)
{
    const std::string pkg = read_text_capped(project_root / "package.json");
    if (pkg.empty()) {
        return std::nullopt;
    }
    // Apps in monorepos are commonly marked private to block accidental
    // publishes — a strong signal that this isn't a library.
    if (pkg.find("\"private\": true") != std::string::npos ||
        pkg.find("\"private\":true") != std::string::npos) {
        return std::nullopt;
    }
    const bool has_entry_field = pkg.find("\"main\"") != std::string::npos ||
                                 pkg.find("\"exports\"") != std::string::npos ||
                                 pkg.find("\"module\"") != std::string::npos;
    if (has_entry_field) {
        return std::string{"Node.js library (package.json with `main`/`exports`/`module` entry, "
                           "not marked private)"};
    }
    std::error_code ec;
    const auto file_exists = [&](const char* name) {
        ec.clear();
        return std::filesystem::exists(project_root / name, ec) && !ec;
    };
    static constexpr std::array<const char*, 4> k_root_index_names = {"index.js", "index.mjs",
                                                                      "index.cjs", "index.ts"};
    for (const char* name : k_root_index_names) {
        if (file_exists(name)) {
            return std::string{"Node.js library (package.json + root `"} + name +
                   "` entry, not marked private)";
        }
    }
    static constexpr std::array<const char*, 4> k_src_index_names = {
        "src/index.js", "src/index.mjs", "src/index.cjs", "src/index.ts"};
    for (const char* name : k_src_index_names) {
        if (file_exists(name)) {
            return std::string{"Node.js library (package.json + `"} + name +
                   "` entry, not marked private)";
        }
    }
    return std::nullopt;
}

/// PHP library: explicit `"type": "library"` is unambiguous; otherwise
/// composer.json with autoload declarations and no `index.php` entry
/// at the project root is the typical PSR-4 package shape.
[[nodiscard]] std::optional<std::string>
detect_php_library(const std::filesystem::path& project_root)
{
    const std::string composer = read_text_capped(project_root / "composer.json");
    if (composer.empty()) {
        return std::nullopt;
    }
    if (composer.find(R"("type": "library")") != std::string::npos ||
        composer.find(R"("type":"library")") != std::string::npos) {
        return std::string{R"(PHP library (composer.json `"type": "library"`))"};
    }
    std::error_code ec;
    const bool has_index_php = std::filesystem::exists(project_root / "index.php", ec) && !ec;
    ec.clear();
    const bool has_public_index =
        std::filesystem::exists(project_root / "public" / "index.php", ec) && !ec;
    if (!has_index_php && !has_public_index && composer.find("\"autoload\"") != std::string::npos) {
        return std::string{"PHP library (composer.json with autoload, no `index.php` entry)"};
    }
    return std::nullopt;
}

/// Ruby gem: presence of any `*.gemspec` at the project root. The
/// gemspec file itself is the canonical "this is a packagable gem"
/// marker; nothing else looks like one.
[[nodiscard]] std::optional<std::string>
detect_ruby_library(const std::filesystem::path& project_root)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(project_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path().extension().string() == ".gemspec") {
            return std::string{"Ruby gem (" + entry.path().filename().string() + ")"};
        }
    }
    return std::nullopt;
}

/// Python library: `pyproject.toml` or `setup.py` plus a discoverable
/// package directory (`src/<pkg>/__init__.py` or `<pkg>/__init__.py`
/// at depth 1). `main_count == 0` and the absence of `manage.py` /
/// `wsgi.py` / `asgi.py` rule out application shapes (Django, FastAPI
/// app skeletons) that would otherwise share the manifest.
[[nodiscard]] std::optional<std::string>
detect_python_library(const std::filesystem::path& project_root, const CodeIndex& index,
                      std::size_t main_count)
{
    std::error_code ec;
    const bool has_pyproject = std::filesystem::exists(project_root / "pyproject.toml", ec) && !ec;
    ec.clear();
    const bool has_setup = std::filesystem::exists(project_root / "setup.py", ec) && !ec;
    ec.clear();
    if (!has_pyproject && !has_setup) {
        return std::nullopt;
    }
    if (main_count > 0) {
        return std::nullopt;
    }
    bool has_app_entry = false;
    bool has_pkg_init = false;
    std::string sample_pkg;
    for (const FileEntry& file : index.snapshot_files()) {
        const auto& path = file.path_relative;
        const std::string fname = path.filename().string();
        if (fname == "manage.py" || fname == "wsgi.py" || fname == "asgi.py" ||
            fname == "__main__.py") {
            const auto depth = std::distance(path.begin(), path.end());
            if (depth <= 2) {
                has_app_entry = true;
            }
        }
        if (fname != "__init__.py") {
            continue;
        }
        auto it = path.begin();
        if (it == path.end()) {
            continue;
        }
        const std::string first = it->string();
        ++it;
        if (it == path.end()) {
            continue;
        }
        if (first == "src") {
            const std::string pkg = it->string();
            ++it;
            if (it != path.end() && it->string() == "__init__.py") {
                has_pkg_init = true;
                if (sample_pkg.empty()) {
                    sample_pkg = pkg;
                }
            }
        }
        else if (it->string() == "__init__.py") {
            // <first>/__init__.py at depth 1.
            has_pkg_init = true;
            if (sample_pkg.empty()) {
                sample_pkg = first;
            }
        }
    }
    if (!has_pkg_init || has_app_entry) {
        return std::nullopt;
    }
    const std::string manifest_name = has_pyproject ? "pyproject.toml" : "setup.py";
    return std::string{"Python library (" + manifest_name + " + `" + sample_pkg +
                       "/__init__.py`, no app entry)"};
}

/// Dispatch on manifest runtime to pick the right ecosystem detector.
[[nodiscard]] std::optional<LibraryHit>
detect_ecosystem_library(const std::filesystem::path& project_root, const ManifestInfo& manifest,
                         const CodeIndex& index, std::size_t main_count)
{
    switch (manifest.runtime) {
    case Runtime::NodeJs:
        if (auto r = detect_node_library(project_root); r) {
            return LibraryHit{std::move(*r), 75};
        }
        break;
    case Runtime::Php:
        if (auto r = detect_php_library(project_root); r) {
            return LibraryHit{std::move(*r), 80};
        }
        break;
    case Runtime::Ruby:
        if (auto r = detect_ruby_library(project_root); r) {
            return LibraryHit{std::move(*r), 80};
        }
        break;
    case Runtime::Python:
        if (auto r = detect_python_library(project_root, index, main_count); r) {
            return LibraryHit{std::move(*r), 75};
        }
        break;
    case Runtime::Go:
    case Runtime::Java:
    case Runtime::CCpp:
    case Runtime::Rust:
    case Runtime::CSharp:
    case Runtime::DotNetSolution:
        break;
    }
    return std::nullopt;
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
    case ArchitectureLabel::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

ArchitectureDescription
detect_architecture(const CodeIndex& index, const std::filesystem::path& project_root,
                    const std::unordered_set<std::string>& exclude_dir_names)
{
    ArchitectureDescription out;

    const std::size_t file_count = index.file_count();
    if (file_count == 0) {
        out.label = ArchitectureLabel::Unknown;
        out.reasoning = "no source files found";
        out.confidence = 0;
        return out;
    }

    // --- Workspace-manifest indicators (highest-confidence monorepos)
    // Before the heuristic pass, consult concrete build-system manifests
    // at the project root. They are unambiguous signals — a project
    // with `Cargo.toml [workspace]` IS a Cargo workspace, regardless of
    // directory shape. Accuracy over guesswork.
    if (!project_root.empty()) {
        if (auto r = detect_rust_workspace(project_root); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(*r);
            out.confidence = 92;
            return out;
        }
        if (auto r = detect_npm_monorepo(project_root); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(*r);
            out.confidence = 90;
            return out;
        }
        if (auto r = detect_python_packages(project_root, index); r) {
            out.label = ArchitectureLabel::Monorepo;
            out.reasoning = std::move(*r);
            out.confidence = 85;
            return out;
        }
    }

    auto signals = collect_path_signals(index);
    augment_signals_from_disk(signals, project_root, exclude_dir_names);
    const auto& segments = signals.segments;
    const auto manifest = detect_root_manifest(project_root);
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
        // Classic layered
        "controllers", "services", "repositories", "models", "domain", "handlers", "dao", "routes",
        // Plugin / modular layering
        "core", "modes", "plugins", "ui", "platform",
        // Hexagonal / clean architecture
        "adapters", "ports", "infrastructure", "infra", "engine"};
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
        out.confidence = 85;
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
        out.confidence = 85;
        return out;
    }

    // --- .NET multi-project solution ---------------------------------
    // Several sibling directories with dotted names like
    // `FlowForge.UI` / `FlowForge.CLI` / `FlowForge.Core` strongly
    // suggest a `.sln` with one `.csproj` per dotted directory.
    if (count_dotted_project_dirs(index) >= 2) {
        out.label = ArchitectureLabel::DotNetSolution;
        out.reasoning = "multiple sibling directories with dotted "
                        "names (typical .NET multi-project solution)";
        out.confidence = 75;
        // A `.sln` at the project root corroborates the dotted-dir
        // signal — heuristic + manifest agreement is high-confidence.
        if (manifest && manifest->runtime == Runtime::DotNetSolution) {
            out.reasoning += " — confirmed by " + manifest->filename;
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
        out.confidence = 90;
        return out;
    }
    (void)has_any; // reserved for future layered heuristics

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
    if ((has_components && has_pages) || has_spa_config) {
        out.label = ArchitectureLabel::FrontendSpa;
        out.reasoning = has_spa_config ? "framework config file detected (next/vite/nuxt)"
                                       : "found `components/` and `pages/` directories";
        out.confidence = 80;
        return out;
    }

    // --- Layered fallback ------------------------------------------
    if (layered_matches >= 3) {
        out.label = ArchitectureLabel::Layered;
        out.reasoning = layered_reason;
        out.confidence = 70;
        return out;
    }

    // --- API backend indicators ------------------------------------
    // A handlers/, routes/, or routers/ directory plus no src/pages
    // and no frontend config implies an API backend. `routers/` is
    // the Beego/Casdoor convention in the Go ecosystem.
    const bool has_handlers = segments.contains("handlers");
    const bool has_routes = segments.contains("routes");
    const bool has_routers = segments.contains("routers");
    if ((has_handlers || has_routes || has_routers) && !has_pages && !has_spa_config) {
        out.label = ArchitectureLabel::ApiBackend;
        if (has_handlers) {
            out.reasoning = "found `handlers/` directory, no frontend config";
        }
        else if (has_routes) {
            out.reasoning = "found `routes/` directory, no frontend config";
        }
        else {
            out.reasoning = "found `routers/` directory, no frontend config";
        }
        out.confidence = 60;
        if (manifest) {
            out.reasoning += " (" + std::string{runtime_label(manifest->runtime)} + " project)";
            out.confidence = 75;
        }
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
        out.confidence = 75;
        if (manifest) {
            out.reasoning += " (" + std::string{runtime_label(manifest->runtime)} + " project)";
            out.confidence = 85;
        }
        return out;
    }

    // (2) Per-ecosystem signals for Node, PHP, Ruby, Python framework-
    //     libraries (express, Slim, sinatra, flask, …) that previously
    //     read 65% Monolith because they don't follow C/C++ layout.
    if (manifest) {
        if (auto hit = detect_ecosystem_library(project_root, *manifest, index, main_count); hit) {
            out.label = ArchitectureLabel::Library;
            out.reasoning = std::move(hit->reasoning);
            out.confidence = hit->confidence;
            return out;
        }
    }

    // --- Default: Monolith -----------------------------------------
    // A root build manifest pins higher confidence than "we have no
    // idea". A coherent codebase (manifest + ≥75% language match)
    // pins higher still.
    if (main_count <= 1 || layered_matches > 0) {
        out.label = ArchitectureLabel::Monolith;
        const bool coherent = manifest && runtime_pct >= 75;
        const std::string lbl{manifest ? runtime_label(manifest->runtime) : ""};
        const std::string manifest_suffix =
            manifest ? " — " + lbl + " (" + manifest->filename + ")" : std::string{};
        const std::string coherence_suffix =
            coherent ? ", " + std::to_string(runtime_pct) + "% " + lbl + "-dominant"
                     : std::string{};
        if (layered_matches > 0) {
            out.reasoning = "single entry point with partial layering (" + layered_reason + ")" +
                            manifest_suffix + coherence_suffix;
            int conf = 50;
            if (coherent) {
                conf = 70;
            }
            else if (manifest) {
                conf = 60;
            }
            out.confidence = conf;
        }
        else if (manifest) {
            out.reasoning = lbl + " project (" + manifest->filename + ")" + coherence_suffix +
                            ", no distinctive layout";
            out.confidence = coherent ? 65 : 55;
        }
        else {
            out.reasoning = "single entry point, no distinctive layout";
            out.confidence = 40;
        }
        return out;
    }

    // Fallback — nothing matched.
    out.label = ArchitectureLabel::Unknown;
    out.reasoning = "no distinctive architectural indicators found";
    out.confidence = 10;
    return out;
}

} // namespace vectis::code
