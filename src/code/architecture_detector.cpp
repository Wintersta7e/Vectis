#include "code/architecture_detector.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/symbol.h"

namespace vectis::code {

namespace {

/// Collect distinct directory segments from scanned files.
///
/// Walks each file's directory components top-down, but **stops** at
/// the first segment whose name is a known test / fixture / doc /
/// vendor root. The stop segment itself is included (so `tests` at
/// top-level still counts as a signal), but deeper segments under it
/// are not — otherwise a fixture path like
/// `tests/fixtures/code/sample-python/models/user.py` would inject a
/// bogus `"models"` signal into architecture detection, making Vectis
/// scanning itself mis-classify as classical layered (models/dao/etc.
/// coming from Java/Python test fixtures, not the real project).
///
/// Deep non-test subtrees are still walked in full — .NET
/// `src/App.UI/ViewModels/...` and monorepo
/// `packages/frontend/src/components/...` both need depth-3+ signals
/// and wouldn't detect correctly with a naive depth cap.
[[nodiscard]] std::unordered_set<std::string>
collect_directory_segments(const CodeIndex& index)
{
    // Names that end the descent. Keep conservative — anything
    // added here stops being a legitimate architecture signal for
    // everything under it.
    static const std::unordered_set<std::string> k_stop_names = {
        "tests", "test",
        "fixtures", "__fixtures__",
        "docs", "doc",
        "examples", "example",
        "vendor", "third_party",
    };

    std::unordered_set<std::string> segments;
    for (const FileEntry& file : index.snapshot_files()) {
        std::filesystem::path dir = file.path_relative;
        if (dir.has_filename()) {
            dir.remove_filename();
        }
        for (const auto& segment : dir) {
            const std::string s = segment.string();
            if (s.empty() || s == "/" || s == ".") {
                continue;
            }
            segments.insert(s);
            if (k_stop_names.contains(s)) {
                break;
            }
        }
    }
    return segments;
}

/// Count distinct top-level or second-level DIRECTORY leaves whose
/// name contains a `.` separator (e.g. `FlowForge.UI`, `Company.Core`)
/// — the hallmark of a .NET solution layout with one project per
/// dotted-name directory. The leaf filename is explicitly excluded
/// (otherwise every `main.py` or `vite.config.ts` would count).
[[nodiscard]] std::size_t
count_dotted_project_dirs(const CodeIndex& index)
{
    std::unordered_set<std::string> dotted;
    for (const FileEntry& file : index.snapshot_files()) {
        std::filesystem::path dir = file.path_relative;
        if (dir.has_filename()) {
            dir.remove_filename();
        }
        auto       it  = dir.begin();
        const auto end = dir.end();
        for (int depth = 0; depth < 2 && it != end; ++depth, ++it) {
            const std::string s = it->string();
            // Dotfiles (`.git`, `.vs`, `.worktrees`) don't count.
            if (s.size() > 2 && s.front() != '.' &&
                s.find('.') != std::string::npos)
            {
                dotted.insert(s);
            }
        }
    }
    return dotted.size();
}

/// True if any scanned file has `name` as its leaf filename. Used to
/// spot entry points (`main.*`) and framework config files
/// (`package.json`, `Cargo.toml`, `next.config.js`).
[[nodiscard]] bool any_file_named(
    const CodeIndex& index, std::string_view name)
{
    for (const FileEntry& file : index.snapshot_files()) {
        if (file.path_relative.filename().string() == name) {
            return true;
        }
    }
    return false;
}

/// Count files whose filename starts with `main.` (e.g. main.cpp,
/// main.rs, main.py, main.go). Used as a rough "entry-point count"
/// heuristic for microservice/monorepo detection.
[[nodiscard]] std::size_t count_main_files(const CodeIndex& index)
{
    std::size_t count = 0;
    for (const FileEntry& file : index.snapshot_files()) {
        const std::string name = file.path_relative.filename().string();
        if (name.rfind("main.", 0) == 0 || name == "main") {
            ++count;
        }
    }
    return count;
}

/// Read a text file (capped to 64 KiB so malformed huge files can't
/// stall the detector). Returns empty on any I/O error — all callers
/// treat an empty payload as "not present / not detected".
[[nodiscard]] std::string
read_text_capped(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return {};
    }
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    constexpr std::streamsize k_max = 64 * 1024;
    std::string               out;
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
    std::string        line;
    while (std::getline(in, line)) {
        std::size_t i = 0;
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])))
        {
            ++i;
        }
        if (i + 11 <= line.size() &&
            line.compare(i, 11, "[workspace]") == 0)
        {
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
    if (std::filesystem::exists(project_root / "pnpm-workspace.yaml", ec) &&
        !ec)
    {
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
    if (!pkg.empty() &&
        pkg.find("\"workspaces\"") != std::string::npos)
    {
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
detect_python_packages(const std::filesystem::path& project_root,
                       const CodeIndex&             index)
{
    std::error_code ec;
    const bool has_pyproject =
        std::filesystem::exists(project_root / "pyproject.toml", ec) && !ec;
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
                           std::to_string(src_packages.size()) +
                           " packages"};
    }
    return std::nullopt;
}

} // namespace

std::string_view architecture_label_name(ArchitectureLabel label) noexcept
{
    switch (label) {
        case ArchitectureLabel::Monolith:          return "Monolith";
        case ArchitectureLabel::Layered:           return "Layered";
        case ArchitectureLabel::Mvc:               return "MVC";
        case ArchitectureLabel::Monorepo:          return "Monorepo";
        case ArchitectureLabel::FrontendSpa:       return "Frontend SPA";
        case ArchitectureLabel::ApiBackend:        return "API Backend";
        case ArchitectureLabel::Mvvm:              return "MVVM";
        case ArchitectureLabel::CleanArchitecture: return "Clean Architecture";
        case ArchitectureLabel::DotNetSolution:    return ".NET Solution";
        case ArchitectureLabel::Unknown:           return "Unknown";
    }
    return "Unknown";
}

ArchitectureDescription
detect_architecture(const CodeIndex& index,
                    const std::filesystem::path& project_root)
{
    ArchitectureDescription out;

    const std::size_t file_count = index.file_count();
    if (file_count == 0) {
        out.label      = ArchitectureLabel::Unknown;
        out.reasoning  = "no source files found";
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
            out.label      = ArchitectureLabel::Monorepo;
            out.reasoning  = std::move(*r);
            out.confidence = 92;
            return out;
        }
        if (auto r = detect_npm_monorepo(project_root); r) {
            out.label      = ArchitectureLabel::Monorepo;
            out.reasoning  = std::move(*r);
            out.confidence = 90;
            return out;
        }
        if (auto r = detect_python_packages(project_root, index); r) {
            out.label      = ArchitectureLabel::Monorepo;
            out.reasoning  = std::move(*r);
            out.confidence = 85;
            return out;
        }
    }

    const auto segments = collect_directory_segments(index);

    // --- Monorepo indicators ---------------------------------------
    // Top-level `packages/`, `apps/`, `libs/` plus multiple main.*
    // files typically mean an npm / cargo / bazel monorepo.
    const bool has_packages = segments.contains("packages");
    const bool has_apps     = segments.contains("apps");
    const bool has_libs     = segments.contains("libs");
    const bool has_packages_top = has_packages || has_apps || has_libs;
    const std::size_t main_count = count_main_files(index);
    if (has_packages_top && main_count >= 2) {
        // Cite the directory that actually matched — a reasoning
        // string claiming "`packages/` or `apps/`" when only `libs/`
        // fired is exactly the template hallucination consumers
        // reported, and erodes trust in the whole classifier.
        const char* which = has_packages ? "packages"
                          : has_apps     ? "apps"
                          :                 "libs";
        out.label      = ArchitectureLabel::Monorepo;
        out.reasoning  = std::string{"top-level `"} + which + "/` plus " +
                         std::to_string(main_count) +
                         " entry points suggests a monorepo layout";
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
        "controllers", "services", "repositories", "models",
        "domain", "handlers", "dao", "routes",
        // Plugin / modular layering
        "core", "modes", "plugins", "ui", "platform",
        // Hexagonal / clean architecture
        "adapters", "ports", "infrastructure", "infra", "engine"};
    std::size_t layered_matches = 0;
    std::string layered_reason  = "found: ";
    bool        first_match     = true;
    for (const std::string_view needle : layered_signals) {
        if (segments.contains(std::string{needle})) {
            ++layered_matches;
            if (!first_match) { layered_reason += ", "; }
            layered_reason += std::string{needle};
            first_match = false;
        }
    }

    // --- Clean Architecture / onion / hexagonal ---------------------
    // Strong signal: at least two of Domain/Application/Infrastructure/
    // Presentation (case-insensitive is unnecessary — filesystems on
    // Windows are case-insensitive but the convention is PascalCase).
    const auto has_any = [&](std::initializer_list<std::string_view> names) {
        for (const std::string_view n : names) {
            if (segments.contains(std::string{n})) return true;
        }
        return false;
    };
    const int clean_hits = (segments.contains("Domain")         ? 1 : 0)
                         + (segments.contains("Application")    ? 1 : 0)
                         + (segments.contains("Infrastructure") ? 1 : 0)
                         + (segments.contains("Presentation")   ? 1 : 0);
    if (clean_hits >= 3) {
        out.label      = ArchitectureLabel::CleanArchitecture;
        out.reasoning  = "Domain/Application/Infrastructure/Presentation "
                         "layering (Clean / hexagonal architecture)";
        out.confidence = 85;
        return out;
    }

    // --- MVVM --------------------------------------------------------
    // ViewModels + Views together is the defining signature. Many
    // modern .NET UI frameworks (WPF, Avalonia, MAUI) put each in its
    // own folder inside a UI project.
    if (segments.contains("ViewModels") && segments.contains("Views")) {
        out.label      = ArchitectureLabel::Mvvm;
        out.reasoning  = "`ViewModels/` and `Views/` directories "
                         "(MVVM UI layer)";
        out.confidence = 85;
        return out;
    }

    // --- .NET multi-project solution ---------------------------------
    // Several sibling directories with dotted names like
    // `FlowForge.UI` / `FlowForge.CLI` / `FlowForge.Core` strongly
    // suggest a `.sln` with one `.csproj` per dotted directory.
    if (count_dotted_project_dirs(index) >= 2) {
        out.label      = ArchitectureLabel::DotNetSolution;
        out.reasoning  = "multiple sibling directories with dotted "
                         "names (typical .NET multi-project solution)";
        out.confidence = 75;
        return out;
    }

    // MVC is a stricter layered variant: it needs `models`,
    // `views`, and `controllers` (or equivalent) all present.
    const bool has_mvc_trio =
        segments.contains("models") &&
        segments.contains("views") &&
        segments.contains("controllers");
    if (has_mvc_trio) {
        out.label      = ArchitectureLabel::Mvc;
        out.reasoning  = "found `models/`, `views/`, and `controllers/` "
                         "directories (classic MVC layout)";
        out.confidence = 90;
        return out;
    }
    (void)has_any; // reserved for future layered heuristics

    // --- Frontend SPA indicators -----------------------------------
    // src/components/ + src/pages/, or the presence of common
    // framework config files.
    const bool has_components = segments.contains("components");
    const bool has_pages      = segments.contains("pages");
    const bool has_spa_config =
        any_file_named(index, "next.config.js") ||
        any_file_named(index, "vite.config.ts") ||
        any_file_named(index, "vite.config.js") ||
        any_file_named(index, "nuxt.config.ts");
    if ((has_components && has_pages) || has_spa_config) {
        out.label = ArchitectureLabel::FrontendSpa;
        out.reasoning = has_spa_config
            ? "framework config file detected (next/vite/nuxt)"
            : "found `components/` and `pages/` directories";
        out.confidence = 80;
        return out;
    }

    // --- Layered fallback ------------------------------------------
    if (layered_matches >= 3) {
        out.label      = ArchitectureLabel::Layered;
        out.reasoning  = layered_reason;
        out.confidence = 70;
        return out;
    }

    // --- API backend indicators ------------------------------------
    // A handlers/ or routes/ directory plus no src/pages or frontend
    // config files implies an API backend.
    if ((segments.contains("handlers") || segments.contains("routes")) &&
        !has_pages && !has_spa_config)
    {
        out.label      = ArchitectureLabel::ApiBackend;
        out.reasoning  = segments.contains("handlers")
            ? "found `handlers/` directory, no frontend config"
            : "found `routes/` directory, no frontend config";
        out.confidence = 60;
        return out;
    }

    // --- Default: Monolith -----------------------------------------
    // Everything else for a single-entrypoint project ends up here.
    if (main_count <= 1 || layered_matches > 0) {
        out.label = ArchitectureLabel::Monolith;
        if (layered_matches > 0) {
            out.reasoning = "single entry point with partial layering (" +
                            layered_reason + ")";
            out.confidence = 50;
        } else {
            out.reasoning = "single entry point, no distinctive layout";
            out.confidence = 40;
        }
        return out;
    }

    // Fallback — nothing matched.
    out.label      = ArchitectureLabel::Unknown;
    out.reasoning  = "no distinctive architectural indicators found";
    out.confidence = 10;
    return out;
}

} // namespace vectis::code
