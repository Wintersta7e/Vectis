#include "code/digest_exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
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
#include "code/fidelity.h"
#include "code/hotspot_detector.h"
#include "code/language.h"
#include "code/pagerank.h"
#include "code/symbol.h"
#include "core/log.h"
#include "core/result.h"
#include "platform/file_io.h"

namespace vectis::code {

namespace {

constexpr const char* k_vectis_version = "0.1.0";
/// Bump when edge_tuple arity/order or any other positional element changes.
constexpr int k_slim_schema_version = 2;
/// Positional-contract identifier for the edge encoding used in slim output.
/// Bump alongside k_slim_schema_version if the tuple layout changes.
constexpr const char* k_slim_edge_format = "tuple-v1";

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

/// Distinct language display names ordered by descending file count, with an
/// alphabetical tie-break for determinism. Used for the full digest's
/// `project.languages` so a top-N read leads with the dominant language
/// instead of whatever happens to sort first alphabetically. `Unknown` is
/// excluded, matching `distinct_language_names`.
[[nodiscard]] std::vector<std::string>
language_names_by_frequency(const std::vector<FileEntry>& files)
{
    std::unordered_map<std::string, std::size_t> counts;
    for (const FileEntry& file : files) {
        if (file.language == Language::Unknown) {
            continue;
        }
        ++counts[std::string{language_name(file.language)}];
    }
    std::vector<std::pair<std::string, std::size_t>> ranked(counts.begin(), counts.end());
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });
    std::vector<std::string> names;
    names.reserve(ranked.size());
    for (auto& entry : ranked) {
        names.push_back(std::move(entry.first));
    }
    return names;
}

/// Sorted unique list of non-empty values produced by `field` across `deps`.
/// Used to build the kinds[] and refs[] tables in the slim header.
template <typename FieldFn>
[[nodiscard]] std::vector<std::string> build_dep_table(std::span<const Dependency> deps,
                                                       FieldFn field)
{
    std::set<std::string> seen;
    for (const Dependency& d : deps) {
        const std::string& val = field(d);
        if (!val.empty()) {
            seen.emplace(val);
        }
    }
    return {seen.begin(), seen.end()};
}

/// Build a string-to-dense-int-index lookup over a sorted table.
/// The returned map is keyed by the same strings stored in `table`.
[[nodiscard]] std::unordered_map<std::string, int>
build_id_lookup(const std::vector<std::string>& table)
{
    std::unordered_map<std::string, int> out;
    out.reserve(table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
        out.emplace(table[i], static_cast<int>(i));
    }
    return out;
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

/// Serialize one file entry to JSON. `include_details` selects full vs slim v2
/// shape; `lang_lookup` maps language name to its index in the languages[] table
/// and is used only by the slim path.
[[nodiscard]] nlohmann::json file_to_json(const FileEntry& file, const CodeIndex& index,
                                          bool include_details,
                                          const std::unordered_map<std::string, int>& lang_lookup)
{
    nlohmann::json node;
    if (include_details) {
        // `generic_string()` forces forward slashes on Windows so the
        // digest stays cross-platform portable.
        node["path"] = file.path_relative.generic_string();
        node["language"] = std::string{language_name(file.language)};
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

    // Slim v2: short object with int lang index.
    node["id"] = file.id;
    node["path"] = file.path_relative.generic_string();
    const std::string lang_name{language_name(file.language)};
    const auto it = lang_lookup.find(lang_name);
    // Unknown language is excluded from `languages[]` by distinct_language_names;
    // those files still need a lang slot — represent as -1.
    node["lang"] = it == lang_lookup.end() ? -1 : it->second;
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
/// When `include_file_details` is true (full format):
/// - `edges`: object-shaped `{source, target, kind, …}` where source/target are
///   relative paths. External edges set `target` to null and carry the raw import
///   string in `target_external`. Internal edges with a non-empty `import_string`
///   also carry `import_ref` — the raw token the resolver consumed. Edges in a
///   calibrated language additionally carry `resolved_by` (reconstructed
///   strategy) and `confidence` (calibrated precision) — see code/fidelity.h.
/// - `cycles`: array of arrays of paths, one per detected cycle.
/// When `include_file_details` is false (slim format):
/// - `edges`: positional 4-tuple `[source_file_id, target_file_id|null,
///   kind_id, ref_id|null]` where kind_id indexes into kinds[] and ref_id
///   indexes into refs[].
/// - `cycles`: array of `{"file_ids": [int...]}` objects; first file_id
///   repeated at end to close the loop (matches _schema.cycle_semantics).
/// `stats` is emitted in both formats.
[[nodiscard]] nlohmann::json build_dependency_graph_json(std::span<const Dependency> deps_in,
                                                         const FileIdToPath& lookup,
                                                         bool include_file_details)
{
    nlohmann::json graph;
    nlohmann::json edges_array = nlohmann::json::array();

    std::size_t internal_count = 0;
    std::size_t external_count = 0;
    std::map<std::string, std::size_t> by_kind;

    // Sort canonically so cold and warm runs produce byte-identical
    // output regardless of the order edges were inserted into the
    // index. Source-language imports and manifest-pass edges land at
    // different points in the pipeline, so insertion order isn't
    // stable across the warm-cache path.
    std::vector<Dependency> deps(deps_in.begin(), deps_in.end());
    std::ranges::sort(deps, dependency_emission_less);

    if (include_file_details) {
        // Full format: object-shaped edges.
        for (const Dependency& dep : deps) {
            if (!dep.kind.empty()) {
                ++by_kind[dep.kind];
            }
            const std::string source_path = path_for(lookup, dep.source_file_id);
            const bool is_external = dep.target_file_id == 0;
            std::string target_path;
            nlohmann::json edge;
            edge["source"] = source_path;
            if (is_external) {
                ++external_count;
                edge["target"] = nullptr;
                edge["target_external"] = dep.import_string;
            }
            else {
                ++internal_count;
                target_path = path_for(lookup, dep.target_file_id);
                edge["target"] = target_path;
                if (!dep.import_string.empty()) {
                    edge["import_ref"] = dep.import_string;
                }
            }
            edge["kind"] = dep.kind;
            // Calibrated per-edge resolution fidelity, dispatched by language
            // (see code/fidelity.h). Computed purely from existing edge data;
            // uncalibrated (language, kind) pairs are left untouched.
            if (const auto fidelity = reconstruct_edge_fidelity(
                    source_path, dep.kind, dep.import_string, target_path, is_external)) {
                edge["resolved_by"] = fidelity->resolved_by;
                edge["confidence"] = fidelity->confidence;
            }
            edges_array.push_back(std::move(edge));
        }
    }
    else {
        // Slim format: positional tuple [source_id, target_id|null, kind_id, ref_id|null].
        // Build index lookups here — they're only needed by the slim branch.
        const std::unordered_map<std::string, int> kind_lookup = build_id_lookup(build_dep_table(
            deps_in, [](const Dependency& d) -> const std::string& { return d.kind; }));
        const std::unordered_map<std::string, int> ref_lookup = build_id_lookup(build_dep_table(
            deps_in, [](const Dependency& d) -> const std::string& { return d.import_string; }));
        for (const Dependency& dep : deps) {
            if (!dep.kind.empty()) {
                ++by_kind[dep.kind];
            }
            // Non-empty kind is always in kind_lookup: build_dep_table collected
            // every non-empty kind from the same deps_in span.
            const int kind_id = dep.kind.empty() ? -1 : kind_lookup.at(dep.kind);
            nlohmann::json edge = nlohmann::json::array();
            edge.push_back(dep.source_file_id);
            if (dep.target_file_id == 0) {
                ++external_count;
                edge.push_back(nullptr);
            }
            else {
                ++internal_count;
                edge.push_back(dep.target_file_id);
            }
            edge.push_back(kind_id);
            if (dep.import_string.empty()) {
                edge.push_back(nullptr);
            }
            else {
                // Non-empty import_string is always in ref_lookup: build_dep_table
                // collected every non-empty import_string from the same deps_in span.
                edge.push_back(ref_lookup.at(dep.import_string));
            }
            edges_array.push_back(std::move(edge));
        }
    }
    graph["edges"] = std::move(edges_array);

    // Cycle detection feeds three consumers: the full `cycles` array
    // (paths per cycle), the slim `cycles` array (objects of file_ids),
    // and the `stats.cycles` count carried by both formats. Feed it the
    // canonically-sorted `deps` (not `deps_in`): SCC member order tracks
    // input order, so unsorted edges make cold and warm caches disagree.
    const std::vector<DependencyCycle> cycles = detect_cycles(deps);
    if (include_file_details) {
        nlohmann::json cycles_array = nlohmann::json::array();
        for (const DependencyCycle& cycle : cycles) {
            nlohmann::json cycle_json = nlohmann::json::array();
            for (const std::int64_t id : cycle.file_ids) {
                cycle_json.push_back(path_for(lookup, id));
            }
            cycles_array.push_back(std::move(cycle_json));
        }
        graph["cycles"] = std::move(cycles_array);
    }
    else {
        nlohmann::json cycles_array = nlohmann::json::array();
        for (const DependencyCycle& cycle : cycles) {
            nlohmann::json cy;
            std::vector<std::int64_t> ids = cycle.file_ids;
            if (!ids.empty()) {
                ids.push_back(ids.front()); // closes-the-loop per _schema.cycle_semantics
            }
            cy["file_ids"] = std::move(ids);
            cycles_array.push_back(std::move(cy));
        }
        graph["cycles"] = std::move(cycles_array);
    }

    nlohmann::json stats;
    stats["total_edges"] = internal_count + external_count;
    stats["internal_edges"] = internal_count;
    stats["external_edges"] = external_count;
    stats["cycles"] = cycles.size();
    stats["by_kind"] = by_kind;
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
/// format only). `max_entries == 0` means "no cap" and preserves the
/// raw severity ordering; slim callers pass a small N (typically 10),
/// which triggers bucketed diversification so one trigger type cannot
/// dominate the top list.
[[nodiscard]] nlohmann::json build_hotspots_json(const CodeIndex& index,
                                                 std::span<const FileEntry> files,
                                                 const FileIdToPath& lookup,
                                                 const std::filesystem::path& project_root,
                                                 bool include_excerpts, std::size_t max_entries = 0)
{
    nlohmann::json arr = nlohmann::json::array();
    std::vector<Hotspot> all = detect_hotspots(index, files);
    const std::vector<Hotspot> selected =
        max_entries == 0 ? std::move(all) : diversify_top_n(std::move(all), max_entries);
    for (const Hotspot& h : selected) {
        nlohmann::json node;
        node["file_id"] = h.file_id;
        node["file"] = path_for(lookup, h.file_id);
        node["reason"] = h.reason;
        node["severity"] = h.severity;
        if (h.symbol_id != 0) {
            node["symbol_id"] = h.symbol_id;
            node["name"] = h.symbol_name;
            node["line"] = h.line;
            node["kind"] = std::string{symbol_kind_name(h.kind)};
        }
        // Emit only the trigger that fired; agents can branch on which
        // numeric field is present instead of parsing `reason`.
        if (h.complexity != 0) {
            node["complexity"] = h.complexity;
        }
        if (h.fan_in != 0) {
            node["fan_in"] = h.fan_in;
        }
        if (h.fan_out != 0) {
            node["fan_out"] = h.fan_out;
        }
        if (h.line_count != 0) {
            node["line_count"] = h.line_count;
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
[[nodiscard]] nlohmann::json build_central_files_json(std::span<const FileEntry> files,
                                                      std::span<const Dependency> deps,
                                                      const FileIdToPath& lookup,
                                                      std::size_t max_entries)
{
    auto out = nlohmann::json::array();
    const std::vector<PageRankResult> ranked = compute_pagerank(files, deps);
    const std::size_t cap = max_entries == 0 ? ranked.size() : std::min(max_entries, ranked.size());
    for (std::size_t i = 0; i < cap; ++i) {
        const PageRankResult& r = ranked[i];
        // Round to 6 dp via integer truncation to keep JSON stable.
        const double rounded = std::round(r.score * 1e6) / 1e6;
        out.push_back({
            {"file_id", r.file_id},
            {"file", path_for(lookup, r.file_id)},
            {"score", rounded},
        });
    }
    return out;
}

/// Emits the slim-only `_schema` block at the top of every slim digest.
/// `version` is a positional-contract pin: bump `k_slim_schema_version` when
/// the edge tuple's arity/order or any other positional element changes.
/// The `|null` suffix on edge_tuple entries is a prose hint for readers only —
/// it is not a type tag and must not be extracted or normalised.
[[nodiscard]] nlohmann::json build_slim_schema_header()
{
    nlohmann::json schema;
    schema["name"] = "vectis.slim";
    schema["version"] = k_slim_schema_version;
    schema["edge_tuple"] =
        nlohmann::json::array({"source_file_id", "target_file_id|null", "kind_id", "ref_id|null"});
    schema["edge_semantics"] = "target_file_id null => unresolved external (ref_id indexes into "
                               "refs[] which holds the raw import string); target_file_id non-null "
                               "=> internal edge (ref_id, when present, indexes a manifest "
                               "coordinate, FQCN, or relative-import artifact)";
    schema["cycle_semantics"] =
        "cycle file_ids include the first file_id repeated at the end to close "
        "the loop";
    schema["file_id_semantics"] =
        "file_id is a stable identifier, not an array offset into files[]";
    return schema;
}

[[nodiscard]] nlohmann::json build_architecture_json(const CodeIndex& index,
                                                     const ExportOptions& options)
{
    const ArchitectureDescription desc =
        detect_architecture(index, options.project_root, options.exclude_dir_names);
    nlohmann::json node;
    node["label"] = std::string{architecture_label_name(desc.label)};
    // `reasoning` is human prose for `vectis explain`; agents read the
    // structured `signals` array instead. Keep JSON machine-only.
    node["signals"] = desc.signals;
    node["confidence"] = desc.confidence;
    return node;
}

/// Build the shared top-level JSON object that full and slim formats
/// both start from.
[[nodiscard]] nlohmann::json build_json(const CodeIndex& index, const ExportOptions& options,
                                        bool include_file_details)
{
    std::vector<FileEntry> files = index.snapshot_files();
    std::ranges::sort(files, [](const FileEntry& a, const FileEntry& b) {
        return a.path_relative.generic_string() < b.path_relative.generic_string();
    });
    const std::vector<Dependency> deps_snapshot = index.all_dependencies();
    const FileIdToPath lookup = build_file_id_to_path(files);
    const std::span<const FileEntry> files_span{files};
    const std::span<const Dependency> deps_span{deps_snapshot};

    const std::vector<std::string> languages = distinct_language_names(files);
    const std::unordered_map<std::string, int> lang_lookup = build_id_lookup(languages);

    nlohmann::json root;
    root["vectis_version"] = k_vectis_version;
    if (!include_file_details) {
        root["_schema"] = build_slim_schema_header();
    }

    nlohmann::json project;
    project["name"] = effective_project_name(options);
    project["root"] = options.project_root.generic_string();
    project["file_count"] = files.size();
    project["symbol_count"] = index.symbol_count();
    project["dependency_count"] = index.dependency_count();
    root["project"] = std::move(project);

    if (include_file_details) {
        // Full format keeps `languages` inside `project` (legacy shape), but
        // count-sorted: a top-N read should lead with the dominant language,
        // not whatever sorts first alphabetically. Slim's top-level table stays
        // alphabetical below — it is an index target, so its order must be
        // stable across runs.
        root["project"]["languages"] = language_names_by_frequency(files);
    }
    else {
        // Slim v2 promotes the tables to top-level nodes.
        // kinds[] and refs[] are built here (slim-only) to skip the allocation
        // on the full path.
        const std::vector<std::string> kinds = build_dep_table(
            deps_span, [](const Dependency& d) -> const std::string& { return d.kind; });
        const std::vector<std::string> refs = build_dep_table(
            deps_span, [](const Dependency& d) -> const std::string& { return d.import_string; });
        root["languages"] = languages;
        root["kinds"] = kinds;
        root["refs"] = refs;
    }

    // Walk every file once and reuse the per-file `symbols` array to
    // build the flat top-level `symbols[]`. Re-querying the index per
    // file would double the shared-mutex acquisitions and the symbol
    // copies for no extra information.
    nlohmann::json files_array = nlohmann::json::array();
    nlohmann::json symbols_flat = nlohmann::json::array();
    for (const FileEntry& file : files) {
        nlohmann::json file_node = file_to_json(file, index, include_file_details, lang_lookup);
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

    // Dependency graph: full format uses object-shaped edges + cycles array;
    // slim uses positional tuples with kind_id and ref_id indices.
    root["dependency_graph"] = build_dependency_graph_json(deps_span, lookup, include_file_details);

    // Architecture is cheap (~150 bytes) and the single highest-value
    // orientation signal — worth emitting in both slim and full.
    root["architecture"] = build_architecture_json(index, options);

    // Per-strategy fidelity calibration for Python and Go import edges
    // (per-language `languages` map). The block is tiny, so both slim and
    // full carry it; the per-edge confidence/resolved_by fields, by
    // contrast, are full-only (slim edge tuples stay frozen at their
    // schema version).
    root["fidelity_metadata"] = build_fidelity_metadata_json();

    if (include_file_details) {
        root["hotspots"] = build_hotspots_json(index, files_span, lookup, options.project_root,
                                               /*include_excerpts=*/true,
                                               /*max_entries=*/0);
        root["central_files"] = build_central_files_json(files_span, deps_span, lookup,
                                                         /*max_entries=*/0);
        root["symbols"] = std::move(symbols_flat);
    }
    else {
        constexpr std::size_t k_slim_hotspot_cap = 10;
        constexpr std::size_t k_slim_central_files_cap = 10;
        root["hotspots"] = build_hotspots_json(index, files_span, lookup, options.project_root,
                                               /*include_excerpts=*/false,
                                               /*max_entries=*/k_slim_hotspot_cap);
        root["central_files"] =
            build_central_files_json(files_span, deps_span, lookup, k_slim_central_files_cap);

        nlohmann::json encoding;
        encoding["edge_format"] = k_slim_edge_format;
        encoding["files"] = files.size();
        encoding["languages"] = languages.size();
        encoding["kinds"] = root["kinds"].size();
        encoding["refs"] = root["refs"].size();
        root["encoding"] = std::move(encoding);
    }

    return root;
}

} // namespace

std::filesystem::path default_output_path(const std::filesystem::path& project_root,
                                          DigestFormat format)
{
    switch (format) {
    case DigestFormat::Json:
        return project_root / "vectis-digest.json";
    case DigestFormat::SlimJson:
        return project_root / "vectis-digest-slim.json";
    }
    return project_root / "vectis-digest.json";
}

std::string build_digest_string(const CodeIndex& index, const ExportOptions& options)
{
    // Body excerpts are sourced verbatim from disk and may contain bytes that
    // are not valid UTF-8 (legacy iso-8859-1 source files, non-UTF terminals,
    // binary-ish blobs that slipped past the size cap). nlohmann's strict
    // serialiser would throw type_error.316 and abort the whole digest;
    // `error_handler_t::replace` substitutes invalid sequences with U+FFFD
    // so a single bad byte can't kill a 100k-file scan.
    constexpr auto k_handler = nlohmann::json::error_handler_t::replace;
    switch (options.format) {
    case DigestFormat::Json: {
        const nlohmann::json root = build_json(index, options, /*include_file_details=*/true);
        return root.dump(2, ' ', /*ensure_ascii=*/false, k_handler);
    }
    case DigestFormat::SlimJson: {
        const nlohmann::json root = build_json(index, options, /*include_file_details=*/false);
        // Compact output: no indent, no per-token whitespace. Slim is
        // agent-only; pretty-printing wastes bytes proportional to
        // edge count (each tuple would otherwise occupy ~6 lines).
        return root.dump(-1, ' ', /*ensure_ascii=*/false, k_handler);
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
