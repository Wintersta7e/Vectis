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

/// Collect every distinct top-level and two-level-deep directory
/// segment from the scanned files so we can test presence cheaply.
[[nodiscard]] std::unordered_set<std::string>
collect_directory_segments(const CodeIndex& index)
{
    std::unordered_set<std::string> segments;
    for (const FileEntry& file : index.snapshot_files()) {
        auto       it  = file.path_relative.begin();
        const auto end = file.path_relative.end();

        // First segment (top level).
        if (it != end) {
            segments.insert(it->string());
            ++it;
        }
        // Second segment (one level nested).
        if (it != end) {
            segments.insert(it->string());
        }
    }
    return segments;
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
        case ArchitectureLabel::Monolith:    return "Monolith";
        case ArchitectureLabel::Layered:     return "Layered";
        case ArchitectureLabel::Mvc:         return "MVC";
        case ArchitectureLabel::Monorepo:    return "Monorepo";
        case ArchitectureLabel::FrontendSpa: return "Frontend SPA";
        case ArchitectureLabel::ApiBackend:  return "API Backend";
        case ArchitectureLabel::Unknown:     return "Unknown";
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
    // Any of controllers/, services/, repositories/, models/,
    // domain/, handlers/, dao/, routes/ signals a layered design.
    const std::vector<std::string_view> layered_signals = {
        "controllers", "services", "repositories", "models",
        "domain", "handlers", "dao", "routes", "core", "platform"};
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
