#include "modes/code/architecture_detector.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "modes/code/code_index.h"
#include "modes/code/symbol.h"

namespace vectis::modes::code {

namespace {

/// Collect every distinct directory segment at ANY depth from the
/// scanned files. The earlier top/second-level-only scan missed
/// patterns that live at deeper levels in modern multi-project
/// .NET solutions (`src/App.UI/ViewModels/...`) and monorepos
/// (`packages/frontend/src/components/...`).
[[nodiscard]] std::unordered_set<std::string>
collect_directory_segments(const CodeIndex& index)
{
    std::unordered_set<std::string> segments;
    for (const FileEntry& file : index.snapshot_files()) {
        auto       it  = file.path_relative.begin();
        const auto end = file.path_relative.end();
        // Skip the leaf filename — only interested in directory
        // components.
        std::filesystem::path dir = file.path_relative;
        if (dir.has_filename()) {
            dir.remove_filename();
        }
        for (const auto& segment : dir) {
            const std::string s = segment.string();
            if (!s.empty() && s != "/" && s != ".") {
                segments.insert(s);
            }
        }
        (void)it;
        (void)end;
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
                    const std::filesystem::path& /*project_root*/)
{
    ArchitectureDescription out;

    const std::size_t file_count = index.file_count();
    if (file_count == 0) {
        out.label      = ArchitectureLabel::Unknown;
        out.reasoning  = "no source files found";
        out.confidence = 0;
        return out;
    }

    const auto segments = collect_directory_segments(index);

    // --- Monorepo indicators ---------------------------------------
    // Top-level `packages/`, `apps/`, `libs/` plus multiple main.*
    // files typically mean an npm / cargo / bazel monorepo.
    const bool has_packages_top = segments.contains("packages") ||
                                  segments.contains("apps") ||
                                  segments.contains("libs");
    const std::size_t main_count = count_main_files(index);
    if (has_packages_top && main_count >= 2) {
        out.label      = ArchitectureLabel::Monorepo;
        out.reasoning  = "top-level `packages/` or `apps/` plus " +
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

} // namespace vectis::modes::code
