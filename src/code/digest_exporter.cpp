#include "code/digest_exporter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "code/architecture_detector.h"
#include "code/code_index.h"
#include "code/dependency.h"
#include "code/dependency_graph.h"
#include "code/hotspot_detector.h"
#include "code/pagerank.h"
#include "code/language.h"
#include "code/symbol.h"
#include "core/log.h"
#include "core/result.h"
#include "platform/file_io.h"

namespace vectis::code {

namespace {

constexpr const char* k_vectis_version = "0.1.0";

/// Current UTC timestamp formatted as RFC 3339, e.g. "2026-04-09T12:34:56Z".
///
/// Uses `std::gmtime` + `strftime` rather than `std::format` with
/// chrono types — libstdc++'s chrono-format support is still patchy
/// across the GCC versions Vectis targets.
[[nodiscard]] std::string current_utc_rfc3339()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::array<char, 32> buffer{};
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — we pass a thread-local tm
    const std::size_t written =
        std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string{buffer.data(), written};
}

/// Sorted unique list of language display names for every file in
/// the index. `Unknown` is excluded.
[[nodiscard]] std::vector<std::string> distinct_language_names(const std::vector<FileEntry>& files)
{
    std::set<std::string> names;
    for (const FileEntry& file : files) {
        if (file.language == Language::Unknown) {
            continue;
        }
        names.emplace(language_name(file.language));
    }
    return {names.begin(), names.end()};
}

/// Derive the effective project name: explicit option takes priority,
/// otherwise fall back to the project root's filename, and if that
/// too is empty, use the literal "project".
[[nodiscard]] std::string effective_project_name(const ExportOptions& options)
{
    if (!options.project_name.empty()) {
        return options.project_name;
    }
    std::string derived = options.project_root.filename().string();
    if (!derived.empty()) {
        return derived;
    }
    return "project";
}

/// Serialize one file entry to JSON. `include_details` controls
/// whether per-file `size`, `lines`, and `symbols` are written — the
/// slim format skips them.
[[nodiscard]] nlohmann::json file_to_json(const FileEntry& file, const CodeIndex& index,
                                          bool include_details)
{
    nlohmann::json node = {
        // `generic_string()` forces forward slashes on Windows so the
        // digest stays cross-platform portable.
        {"path", file.path_relative.generic_string()},
        {"language", std::string{language_name(file.language)}},
    };

    if (!include_details) {
        return node;
    }

    node["size"] = file.size;
    node["lines"] = file.line_count;

    nlohmann::json symbols_array = nlohmann::json::array();
    for (const Symbol& sym : index.symbols_in_file(file.id)) {
        nlohmann::json symbol_node = {
            {"name", sym.name},
            {"kind", std::string{symbol_kind_name(sym.kind)}},
            {"line", sym.line_start},
        };
        if (!sym.signature.empty()) {
            symbol_node["signature"] = sym.signature;
        }
        if (!sym.members.empty()) {
            // Members are a flat array of strings: enum values for
            // `enum` symbols, public field names for `struct` symbols.
            symbol_node["members"] = sym.members;
        }
        if (sym.complexity > 0) {
            symbol_node["complexity"] = sym.complexity;
        }
        if (sym.visibility != Visibility::Unknown) {
            symbol_node["visibility"] = visibility_name(sym.visibility);
        }
        if (!sym.decorators.empty()) {
            symbol_node["decorators"] = sym.decorators;
        }
        symbols_array.push_back(std::move(symbol_node));
    }
    node["symbols"] = std::move(symbols_array);

    return node;
}

/// Map a file_id to its relative path using a precomputed lookup.
/// Used by the dep-graph and hotspot serializers so we don't
/// re-walk the file list on every lookup.
using FileIdToPath = std::unordered_map<std::int64_t, std::string>;

[[nodiscard]] FileIdToPath build_file_id_to_path(const std::vector<FileEntry>& files)
{
    FileIdToPath out;
    out.reserve(files.size());
    for (const FileEntry& file : files) {
        out.emplace(file.id, file.path_relative.generic_string());
    }
    return out;
}

[[nodiscard]] std::string path_for(const FileIdToPath& lookup, std::int64_t file_id)
{
    const auto it = lookup.find(file_id);
    return it == lookup.end() ? std::string{} : it->second;
}

/// Build the dependency_graph JSON block.
/// - `edges`: array of `{source, target, kind}` where source/target
///   are relative paths. External (unresolved) edges use a target of
///   null with the raw import_string carried on the edge.
/// - `cycles`: array of arrays of paths, one per detected cycle.
/// - `stats`: totals for quick scanning.
[[nodiscard]] nlohmann::json build_dependency_graph_json(const CodeIndex& index,
                                                         const FileIdToPath& lookup,
                                                         bool include_externals)
{
    nlohmann::json graph;
    nlohmann::json edges_array = nlohmann::json::array();

    std::size_t internal_count = 0;
    std::size_t external_count = 0;

    for (const Dependency& dep : index.all_dependencies()) {
        if (dep.target_file_id == 0) {
            ++external_count;
            if (!include_externals) {
                continue;
            }
            nlohmann::json edge;
            edge["source"] = path_for(lookup, dep.source_file_id);
            edge["target"] = nullptr;
            edge["target_external"] = dep.import_string;
            edge["kind"] = dep.kind;
            edges_array.push_back(std::move(edge));
        }
        else {
            ++internal_count;
            nlohmann::json edge;
            edge["source"] = path_for(lookup, dep.source_file_id);
            edge["target"] = path_for(lookup, dep.target_file_id);
            edge["kind"] = dep.kind;
            edges_array.push_back(std::move(edge));
        }
    }
    graph["edges"] = std::move(edges_array);

    // Cycles — only emitted in the full format.
    if (include_externals) {
        nlohmann::json cycles_array = nlohmann::json::array();
        for (const DependencyCycle& cycle : detect_cycles(index)) {
            nlohmann::json cycle_json = nlohmann::json::array();
            for (const std::int64_t id : cycle.file_ids) {
                cycle_json.push_back(path_for(lookup, id));
            }
            cycles_array.push_back(std::move(cycle_json));
        }
        graph["cycles"] = std::move(cycles_array);
    }

    nlohmann::json stats;
    stats["total_edges"] = internal_count + external_count;
    stats["internal_edges"] = internal_count;
    stats["external_edges"] = external_count;
    graph["stats"] = std::move(stats);

    return graph;
}

/// Read the first `max_lines` lines of `abs_path` (trimmed). Returns an
/// empty string on any I/O error — hotspot excerpts are an
/// "if-you-can" enrichment and should never fail a digest.
[[nodiscard]] std::string read_hotspot_excerpt(const std::filesystem::path& abs_path,
                                               std::size_t max_lines)
{
    std::error_code ec;
    if (!std::filesystem::exists(abs_path, ec) || ec) {
        return {};
    }
    std::ifstream in(abs_path);
    if (!in) {
        return {};
    }
    std::string out;
    std::string line;
    std::size_t count = 0;
    // Cap bytes too so a minified single-line file can't dominate the
    // digest (~4 KiB per hotspot is plenty for orientation).
    constexpr std::size_t k_max_bytes = static_cast<std::size_t>(4) * 1024;
    while (count < max_lines && std::getline(in, line)) {
        if (out.size() + line.size() + 1 > k_max_bytes) {
            break;
        }
        out.append(line);
        out.push_back('\n');
        ++count;
    }
    return out;
}

/// Build an excerpt string for one hotspot. For function-level
/// hotspots with a known symbol range, use the function body; for
/// file-level hotspots use the first 30 lines of the file.
[[nodiscard]] std::string build_hotspot_excerpt(const CodeIndex& index, const Hotspot& h,
                                                const std::filesystem::path& project_root,
                                                const FileIdToPath& lookup)
{
    const std::string rel_path_str = path_for(lookup, h.file_id);
    if (rel_path_str.empty() || project_root.empty()) {
        return {};
    }
    const std::filesystem::path abs_path = project_root / rel_path_str;

    // Function-level: try to pull the symbol's line range.
    if (h.symbol_id != 0) {
        const auto symbols = index.symbols_in_file(h.file_id);
        for (const Symbol& s : symbols) {
            if (s.id == h.symbol_id && s.line_start > 0 && s.line_end >= s.line_start) {
                std::ifstream in(abs_path);
                if (!in) {
                    break;
                }
                std::string line;
                int n = 1;
                std::string out;
                // Cap body excerpt at 60 lines / 4 KiB so a pathological
                // mega-function doesn't bloat the digest.
                constexpr std::size_t k_max_bytes = static_cast<std::size_t>(4) * 1024;
                const int end_line = std::min(s.line_end, s.line_start + 60);
                while (std::getline(in, line)) {
                    if (n >= s.line_start && n <= end_line) {
                        if (out.size() + line.size() + 1 > k_max_bytes) {
                            break;
                        }
                        out.append(line);
                        out.push_back('\n');
                    }
                    if (n > end_line) {
                        break;
                    }
                    ++n;
                }
                if (!out.empty()) {
                    return out;
                }
            }
        }
    }

    // File-level fallback: first 30 lines.
    return read_hotspot_excerpt(abs_path, 30);
}

/// Serialize hotspots to JSON. `include_excerpts` controls whether
/// the relatively expensive file-body excerpts are attached (full
/// format only). `max_entries == 0` means "no cap"; slim callers pass
/// a small N (typically 10) to cap total size.
[[nodiscard]] nlohmann::json build_hotspots_json(const CodeIndex& index, const FileIdToPath& lookup,
                                                 const std::filesystem::path& project_root,
                                                 bool include_excerpts, std::size_t max_entries = 0)
{
    nlohmann::json arr = nlohmann::json::array();
    const std::vector<Hotspot> all = detect_hotspots(index);
    const std::size_t limit = (max_entries == 0) ? all.size() : std::min(all.size(), max_entries);
    for (std::size_t i = 0; i < limit; ++i) {
        const Hotspot& h = all[i];
        nlohmann::json node;
        node["file"] = path_for(lookup, h.file_id);
        node["reason"] = h.reason;
        node["severity"] = h.severity;
        if (h.symbol_id != 0) {
            node["symbol_id"] = h.symbol_id;
        }
        if (include_excerpts) {
            std::string excerpt = build_hotspot_excerpt(index, h, project_root, lookup);
            if (!excerpt.empty()) {
                node["excerpt"] = std::move(excerpt);
            }
        }
        arr.push_back(std::move(node));
    }
    return arr;
}

/// Serialize the top-N most central files by PageRank. `max_entries`
/// caps the list (slim digest passes 10; full digest passes 0 = no
/// cap). Always emits the `score` rounded to 6 decimal places — small
/// enough to keep round-trip diffs stable but precise enough to break
/// ties between adjacent ranks.
[[nodiscard]] nlohmann::json build_central_files_json(const CodeIndex& index,
                                                      const FileIdToPath& lookup,
                                                      std::size_t max_entries)
{
    auto out = nlohmann::json::array();
    const std::vector<PageRankResult> ranked = compute_pagerank(index);
    const std::size_t cap = max_entries == 0 ? ranked.size() : std::min(max_entries, ranked.size());
    for (std::size_t i = 0; i < cap; ++i) {
        const PageRankResult& r = ranked[i];
        // Round to 6 dp via integer truncation to keep JSON stable.
        const double rounded = std::round(r.score * 1e6) / 1e6;
        out.push_back({
            {"file_id", r.file_id},
            {"path", path_for(lookup, r.file_id)},
            {"score", rounded},
        });
    }
    return out;
}

[[nodiscard]] nlohmann::json build_architecture_json(const CodeIndex& index,
                                                     const ExportOptions& options)
{
    const ArchitectureDescription desc =
        detect_architecture(index, options.project_root, options.exclude_dir_names);
    nlohmann::json node;
    node["label"] = std::string{architecture_label_name(desc.label)};
    node["reasoning"] = desc.reasoning;
    node["confidence"] = desc.confidence;
    return node;
}

/// Build the shared top-level JSON object that full and slim formats
/// both start from.
[[nodiscard]] nlohmann::json build_json(const CodeIndex& index, const ExportOptions& options,
                                        bool include_file_details)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    const FileIdToPath lookup = build_file_id_to_path(files);

    nlohmann::json root;
    root["vectis_version"] = k_vectis_version;
    root["generated_at"] = current_utc_rfc3339();

    nlohmann::json project;
    project["name"] = effective_project_name(options);
    project["root"] = options.project_root.generic_string();
    project["file_count"] = files.size();
    project["symbol_count"] = index.symbol_count();
    project["dependency_count"] = index.dependency_count();
    project["languages"] = distinct_language_names(files);
    root["project"] = std::move(project);

    // Walk every file once and reuse the per-file `symbols` array to
    // build the flat top-level `symbols[]`. Re-querying the index per
    // file would double the shared-mutex acquisitions and the symbol
    // copies for no extra information.
    nlohmann::json files_array = nlohmann::json::array();
    nlohmann::json symbols_flat = nlohmann::json::array();
    for (const FileEntry& file : files) {
        nlohmann::json file_node = file_to_json(file, index, include_file_details);
        if (include_file_details) {
            const std::string path = path_for(lookup, file.id);
            for (const auto& sym : file_node["symbols"]) {
                nlohmann::json flat = {
                    {"name", sym["name"]},
                    {"kind", sym["kind"]},
                    {"path", path},
                    {"line", sym["line"]},
                };
                if (sym.contains("visibility")) {
                    flat["visibility"] = sym["visibility"];
                }
                if (sym.contains("decorators")) {
                    flat["decorators"] = sym["decorators"];
                }
                symbols_flat.push_back(std::move(flat));
            }
        }
        files_array.push_back(std::move(file_node));
    }
    root["files"] = std::move(files_array);

    // Dependency graph: full format includes externals + cycles;
    // slim includes only resolved edges (no cycles, no externals).
    root["dependency_graph"] = build_dependency_graph_json(index, lookup, include_file_details);

    // Architecture is cheap (~150 bytes) and the single highest-value
    // orientation signal — worth emitting in both slim and full.
    root["architecture"] = build_architecture_json(index, options);
    if (include_file_details) {
        root["hotspots"] = build_hotspots_json(index, lookup, options.project_root,
                                               /*include_excerpts=*/true,
                                               /*max_entries=*/0);
        root["central_files"] = build_central_files_json(index, lookup,
                                                         /*max_entries=*/0);
        root["symbols"] = std::move(symbols_flat);
    }
    else {
        constexpr std::size_t k_slim_hotspot_cap = 10;
        constexpr std::size_t k_slim_central_files_cap = 10;
        root["hotspots"] = build_hotspots_json(index, lookup, options.project_root,
                                               /*include_excerpts=*/false,
                                               /*max_entries=*/k_slim_hotspot_cap);
        root["central_files"] = build_central_files_json(index, lookup,
                                                         k_slim_central_files_cap);
    }

    return root;
}

/// Build the Markdown digest as a single string.
[[nodiscard]] std::string build_markdown(const CodeIndex& index, const ExportOptions& options)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    const std::vector<std::string> langs = distinct_language_names(files);
    const std::string proj_name = effective_project_name(options);
    const std::string timestamp = current_utc_rfc3339();

    std::ostringstream out;
    out << "# " << proj_name << "\n\n";
    out << "_Generated " << timestamp << " by Vectis " << k_vectis_version << "_\n\n";

    out << "## Overview\n\n";
    out << "- Root: `" << options.project_root.generic_string() << "`\n";
    out << "- Files: " << files.size() << "\n";
    out << "- Symbols: " << index.symbol_count() << "\n";
    out << "- Dependencies: " << index.dependency_count() << "\n";
    out << "- Languages: ";
    for (std::size_t i = 0; i < langs.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << langs[i];
    }
    out << "\n\n";

    // --- Architecture ---------------------------------------------
    const ArchitectureDescription arch =
        detect_architecture(index, options.project_root, options.exclude_dir_names);
    out << "## Architecture\n\n";
    out << "**" << architecture_label_name(arch.label) << "** " << "(confidence "
        << static_cast<int>(arch.confidence) << "/100)  \n";
    if (!arch.reasoning.empty()) {
        out << "_" << arch.reasoning << "_\n";
    }
    out << "\n";

    // --- Hotspots --------------------------------------------------
    const auto hotspots = detect_hotspots(index);
    out << "## Hotspots\n\n";
    if (hotspots.empty()) {
        out << "_(none — nothing crossed a complexity, size, or fan-out threshold)_\n\n";
    }
    else {
        out << "| Severity | File | Reason |\n";
        out << "|---|---|---|\n";
        const FileIdToPath lookup = build_file_id_to_path(files);
        for (const auto& h : hotspots) {
            out << "| " << h.severity << " | `" << path_for(lookup, h.file_id) << "`" << " | "
                << h.reason << " |\n";
        }
        out << "\n";
        // Per-hotspot body excerpts below the table, fenced and
        // language-hinted. Gives an agent enough shape-context to
        // understand WHY the file is a hotspot without chasing the
        // path manually.
        for (const auto& h : hotspots) {
            const std::string excerpt =
                build_hotspot_excerpt(index, h, options.project_root, lookup);
            if (excerpt.empty()) {
                continue;
            }
            const std::string rel_path = path_for(lookup, h.file_id);
            std::string_view lang_hint;
            for (const FileEntry& f : files) {
                if (f.id == h.file_id) {
                    lang_hint = language_name(f.language);
                    break;
                }
            }
            out << "### `" << rel_path << "` (severity " << h.severity << ")\n\n```" << lang_hint
                << "\n"
                << excerpt << "```\n\n";
        }
    }

    // --- Central files (PageRank) ---------------------------------
    // Top 10 most central files by PageRank over the dep graph. This
    // surfaces the structural backbone of the project — the headers /
    // modules everyone else imports — so an agent reading the digest
    // knows what's worth opening first.
    {
        const std::vector<PageRankResult> ranked = compute_pagerank(index);
        if (!ranked.empty()) {
            const FileIdToPath lookup = build_file_id_to_path(files);
            out << "## Central Files\n\n";
            out << "_Top 10 by PageRank over the dependency graph._\n\n";
            const std::size_t cap = std::min<std::size_t>(10, ranked.size());
            for (std::size_t i = 0; i < cap; ++i) {
                const std::string path = path_for(lookup, ranked[i].file_id);
                if (path.empty()) {
                    continue;
                }
                const double rounded = std::round(ranked[i].score * 1e6) / 1e6;
                out << "- `" << path << "` (";
                out << std::fixed;
                out.precision(6);
                out << rounded << ")\n";
                out.unsetf(std::ios::fixed);
                out.precision(6);
            }
            out << "\n";
        }
    }

    // --- Dependency graph ------------------------------------------
    out << "## Dependency Graph\n\n";
    {
        const auto all_deps = index.all_dependencies();
        std::size_t internal_count = 0;
        std::size_t external_count = 0;
        for (const auto& d : all_deps) {
            if (d.target_file_id == 0) {
                ++external_count;
            }
            else {
                ++internal_count;
            }
        }
        const auto cycles = detect_cycles(index);
        out << "- Edges: " << all_deps.size() << " (" << internal_count << " internal, "
            << external_count << " external)\n";
        out << "- Cycles: " << cycles.size() << "\n";
        if (!cycles.empty()) {
            const FileIdToPath lookup = build_file_id_to_path(files);
            out << "\n**Detected cycles:**\n\n";
            for (std::size_t i = 0; i < cycles.size(); ++i) {
                out << (i + 1) << ". ";
                const auto& file_ids = cycles[i].file_ids;
                for (std::size_t j = 0; j < file_ids.size(); ++j) {
                    if (j > 0) {
                        out << " ↔ ";
                    }
                    out << "`" << path_for(lookup, file_ids[j]) << "`";
                }
                out << "\n";
            }
        }
        out << "\n";
    }

    out << "## Files\n\n";
    if (files.empty()) {
        out << "_(no source files found)_\n";
        return out.str();
    }

    for (const FileEntry& file : files) {
        out << "### " << file.path_relative.generic_string() << "  _("
            << language_name(file.language) << ")_\n\n";

        const std::vector<Symbol> symbols = index.symbols_in_file(file.id);
        if (symbols.empty()) {
            out << "_(no symbols extracted)_\n\n";
            continue;
        }
        for (const Symbol& sym : symbols) {
            // Prefer the full signature as the displayed label for
            // functions and methods — it's far more useful to an LLM
            // or reader than the bare name. Everything else uses the
            // plain name.
            const std::string& label = sym.signature.empty() ? sym.name : sym.signature;
            out << "- `" << label << "` (" << symbol_kind_name(sym.kind);
            if (sym.visibility != Visibility::Unknown) {
                out << ", " << visibility_name(sym.visibility);
            }
            out << ") — line " << sym.line_start << "\n";

            // Indented sub-bullet for enum values / struct fields,
            // comma-separated and inline-code-formatted.
            if (!sym.members.empty()) {
                out << "    - ";
                for (std::size_t i = 0; i < sym.members.size(); ++i) {
                    if (i > 0) {
                        out << ", ";
                    }
                    out << "`" << sym.members[i] << "`";
                }
                out << "\n";
            }

            // Decorator / annotation list as a separate sub-bullet so a
            // reader (and the JSON↔MD round-trip) sees them adjacent to
            // the symbol they decorate. Stored without the leading `@`;
            // re-attached here for parity with the source idiom.
            if (!sym.decorators.empty()) {
                out << "    - decorators: ";
                for (std::size_t i = 0; i < sym.decorators.size(); ++i) {
                    if (i > 0) {
                        out << ", ";
                    }
                    out << "`@" << sym.decorators[i] << "`";
                }
                out << "\n";
            }
        }
        out << "\n";
    }

    return out.str();
}

} // namespace

std::filesystem::path default_output_path(const std::filesystem::path& project_root,
                                          DigestFormat format)
{
    switch (format) {
    case DigestFormat::Json:
        return project_root / "vectis-digest.json";
    case DigestFormat::Markdown:
        return project_root / "vectis-digest.md";
    case DigestFormat::SlimJson:
        return project_root / "vectis-digest-slim.json";
    }
    return project_root / "vectis-digest.json";
}

std::string build_digest_string(const CodeIndex& index, const ExportOptions& options)
{
    switch (options.format) {
    case DigestFormat::Json: {
        const nlohmann::json root = build_json(index, options, /*include_file_details=*/true);
        return root.dump(2);
    }
    case DigestFormat::SlimJson: {
        const nlohmann::json root = build_json(index, options, /*include_file_details=*/false);
        return root.dump(2);
    }
    case DigestFormat::Markdown: {
        return build_markdown(index, options);
    }
    }
    return {};
}

vectis::core::Result<std::filesystem::path> export_digest(const CodeIndex& index,
                                                          const ExportOptions& options)
{
    if (options.project_root.empty()) {
        return vectis::core::make_error(vectis::core::ErrorKind::ConfigError,
                                        "export_digest called with empty project_root",
                                        "digest_exporter");
    }

    std::filesystem::path out_path = default_output_path(options.project_root, options.format);

    std::error_code ec;
    if (std::filesystem::exists(out_path, ec)) {
        VECTIS_LOG_INFO("Overwriting existing digest at '{}'", out_path.string());
    }

    std::string content;
    try {
        content = build_digest_string(index, options);
    }
    catch (const std::exception& e) {
        return vectis::core::make_error(vectis::core::ErrorKind::ParseError,
                                        std::string{"failed to serialize digest: "} + e.what(),
                                        out_path.string());
    }

    if (auto r = vectis::platform::write_file(out_path, content); !r) {
        return vectis::core::make_error(vectis::core::ErrorKind::IoError,
                                        std::string{"failed to write digest: "} + r.error().message,
                                        out_path.string());
    }

    VECTIS_LOG_INFO("Digest exported: {} ({} bytes)", out_path.string(), content.size());
    return out_path;
}

} // namespace vectis::code
