#include "code/explain.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "code/architecture_detector.h"
#include "code/code_index.h"
#include "code/dependency_graph.h"
#include "code/hotspot_detector.h"
#include "code/language.h"
#include "code/pagerank.h"
#include "code/symbol.h"

namespace vectis::code {

namespace {

[[nodiscard]] std::string project_display_name(const ExplainOptions& options)
{
    if (!options.project_name.empty()) {
        return options.project_name;
    }
    auto derived = options.project_root.filename().string();
    if (!derived.empty()) {
        return derived;
    }
    return "project";
}

/// Collapse runs of whitespace inside a decorator/annotation string to
/// a single space, strip leading/trailing whitespace, and truncate to
/// `max_len` chars (with a trailing `…`) so a multi-line `@WebService(
/// serviceName = "x", portName = "y", …)` does not eat the explain
/// output's line budget.
[[nodiscard]] std::string normalise_decorator(std::string_view in)
{
    constexpr std::size_t k_max_len = 80;
    std::string out;
    out.reserve(in.size());
    bool prev_ws = false;
    for (const char c : in) {
        const bool is_ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (is_ws) {
            if (!prev_ws && !out.empty()) {
                out.push_back(' ');
            }
            prev_ws = true;
        }
        else {
            out.push_back(c);
            prev_ws = false;
        }
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (out.size() > k_max_len) {
        out.resize(k_max_len - 1);
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out += "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS (3 bytes UTF-8)
    }
    return out;
}

/// Top N (name, count) pairs from a map, sorted by count descending.
template <typename K, typename V>
[[nodiscard]] std::vector<std::pair<K, V>> top_n(const std::unordered_map<K, V>& m, std::size_t n)
{
    std::vector<std::pair<K, V>> out;
    out.reserve(m.size());
    for (const auto& kv : m) {
        out.emplace_back(kv.first, kv.second);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (out.size() > n) {
        out.resize(n);
    }
    return out;
}

/// Build a short "languages: X (a%), Y (b%)" line from the file list.
[[nodiscard]] std::string render_languages(const std::vector<FileEntry>& files)
{
    std::unordered_map<std::string, std::size_t> by_lang;
    std::size_t classified = 0;
    for (const auto& f : files) {
        if (f.language == Language::Unknown) {
            continue;
        }
        ++by_lang[std::string{language_name(f.language)}];
        ++classified;
    }
    if (classified == 0) {
        return "no recognised languages";
    }
    const auto top = top_n(by_lang, 3);
    std::ostringstream oss;
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        const int pct = static_cast<int>((top[i].second * 100 + classified / 2) / classified);
        oss << top[i].first << " (" << pct << "%, " << top[i].second << " files)";
    }
    return oss.str();
}

/// `47 public / 318 private` — collapse the visibility distribution
/// into a one-line summary. Languages without a real visibility
/// concept (JavaScript, plain C, SQL) report `Visibility::Unknown`
/// for every symbol; those are excluded so a vendored jQuery bundle
/// doesn't claim 6000+ "public" symbols when it has no API surface
/// at all in the Java sense.
[[nodiscard]] std::string render_visibility(const std::vector<Symbol>& symbols)
{
    std::size_t pub = 0;
    std::size_t priv = 0;
    std::size_t prot = 0;
    std::size_t internal = 0;
    for (const auto& s : symbols) {
        switch (s.visibility) {
        case Visibility::Public:
            ++pub;
            break;
        case Visibility::Private:
            ++priv;
            break;
        case Visibility::Protected:
            ++prot;
            break;
        case Visibility::Internal:
            ++internal;
            break;
        case Visibility::Unknown:
            // Languages without a visibility concept skip the count.
            break;
        }
    }
    std::ostringstream oss;
    oss << pub << " public";
    if (priv > 0) {
        oss << " / " << priv << " private";
    }
    if (prot > 0) {
        oss << " / " << prot << " protected";
    }
    if (internal > 0) {
        oss << " / " << internal << " internal";
    }
    return oss.str();
}

/// Render a pre-selected hotspot list. Function-level entries get a
/// `path:line  name  [kind, reason]` line; file-level entries
/// (`symbol_id == 0`) get `path  [reason]`.
void render_hotspots(std::ostream& out, const std::vector<Hotspot>& hotspots,
                     const std::vector<FileEntry>& files)
{
    if (hotspots.empty()) {
        return;
    }
    std::unordered_map<std::int64_t, std::string> path_by_id;
    path_by_id.reserve(files.size());
    for (const auto& f : files) {
        path_by_id.emplace(f.id, f.path_relative.generic_string());
    }
    out << "\nTop hotspots:\n";
    for (const Hotspot& h : hotspots) {
        const auto path_it = path_by_id.find(h.file_id);
        const std::string path = path_it == path_by_id.end() ? std::string{"?"} : path_it->second;
        out << "  " << path;
        if (h.symbol_id != 0) {
            out << ":" << h.line << "  " << h.symbol_name << "  [" << symbol_kind_name(h.kind)
                << ", " << h.reason << "]\n";
        }
        else {
            out << "  [" << h.reason << "]\n";
        }
    }
}

/// Top external dependencies by import-count. Internal edges and
/// cycles get a one-line summary.
void render_dependencies(std::ostream& out, const CodeIndex& index)
{
    const auto deps = index.all_dependencies();
    if (deps.empty()) {
        return;
    }
    std::unordered_map<std::string, std::size_t> external_count;
    std::size_t internal_edges = 0;
    for (const auto& d : deps) {
        if (d.target_file_id == 0) {
            ++external_count[d.import_string];
        }
        else {
            ++internal_edges;
        }
    }
    out << "\nDependency graph: " << internal_edges << " internal edge"
        << (internal_edges == 1 ? "" : "s");
    const auto cycles = detect_cycles(deps);
    if (cycles.empty()) {
        out << ", no cycles";
    }
    else {
        out << ", " << cycles.size() << " cycle" << (cycles.size() == 1 ? "" : "s");
    }
    out << ".\n";

    if (!external_count.empty()) {
        const auto top = top_n(external_count, 5);
        out << "External imports (top 5): ";
        for (std::size_t i = 0; i < top.size(); ++i) {
            if (i > 0) {
                out << ", ";
            }
            out << top[i].first << " (" << top[i].second << ")";
        }
        out << ".\n";
    }
}

} // namespace

std::string build_explanation(const CodeIndex& index, const ExplainOptions& options)
{
    std::ostringstream out;

    const auto& excludes = options.exclude_dir_names;
    const ArchitectureDescription arch = detect_architecture(index, options.project_root, excludes);
    const std::string name = project_display_name(options);

    // Header — one line you can paste into a Slack message.
    out << name << " — " << architecture_label_name(arch.label) << " (" << +arch.confidence
        << "% confidence)\n";

    // Architecture reasoning
    if (!arch.reasoning.empty()) {
        out << "Architecture: " << arch.reasoning << ".\n";
    }

    // Scale + language mix
    const auto files = index.snapshot_files();
    const auto symbols = index.snapshot_all_symbols();
    out << "Scale: " << files.size() << " files, " << symbols.size() << " symbols, "
        << index.dependency_count() << " dependency edges.\n";
    out << "Languages: " << render_languages(files) << ".\n";

    // Public-API surface
    const std::string vis = render_visibility(symbols);
    if (!vis.empty()) {
        out << "API surface: " << vis << ".\n";
    }

    // Use the detector's bucketed top-N so a dominant trigger (e.g.
    // high fan-in in big monorepos) can't crowd out complexity / size
    // / fan-out hotspots in the explain output. Reuse `files`; the
    // detector would otherwise re-snapshot the index — wasteful on
    // huge projects.
    constexpr std::size_t k_hotspot_cap = 5;
    const std::vector<Hotspot> hotspots =
        diversify_top_n(detect_hotspots(index, std::span<const FileEntry>{files}), k_hotspot_cap);
    render_hotspots(out, hotspots, files);

    // Most central files by PageRank — shows the structural backbone.
    // Distinct from hotspots (which are "complex / risky"); this is
    // "imported by everything", so an agent picks reading order from it.
    {
        const std::vector<Dependency> deps = index.all_dependencies();
        const std::vector<PageRankResult> ranked =
            compute_pagerank(std::span{files}, std::span{deps});
        if (!ranked.empty()) {
            // Cap at 5 — the explain output is meant to fit on a screen.
            constexpr std::size_t k_cap = 5;
            const std::size_t cap = std::min(k_cap, ranked.size());
            std::unordered_map<std::int64_t, std::filesystem::path> id_to_path;
            id_to_path.reserve(files.size());
            for (const auto& f : files) {
                id_to_path.emplace(f.id, f.path_relative);
            }
            out << "\nMost central files (by PageRank):\n";
            for (std::size_t i = 0; i < cap; ++i) {
                const auto it = id_to_path.find(ranked[i].file_id);
                if (it == id_to_path.end()) {
                    continue;
                }
                out << "  " << it->second.generic_string() << "\n";
            }
        }
    }

    // Decorators / annotations — surface the top-N so an agent can see
    // "this project has 99 @app.route handlers, 17 @pytest.fixture
    // markers, …" without re-reading the source.
    {
        std::unordered_map<std::string, std::size_t> dec_count;
        std::size_t decorated_symbols = 0;
        for (const auto& s : symbols) {
            if (s.decorators.empty()) {
                continue;
            }
            ++decorated_symbols;
            for (const auto& d : s.decorators) {
                ++dec_count[normalise_decorator(d)];
            }
        }
        if (!dec_count.empty()) {
            const auto top = top_n(dec_count, 5);
            out << "\nDecorators (top 5 over " << decorated_symbols << " decorated symbols): ";
            for (std::size_t i = 0; i < top.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << "@" << top[i].first << " (" << top[i].second << ")";
            }
            out << ".\n";
        }
    }

    // Dependencies
    render_dependencies(out, index);

    return std::move(out).str();
}

} // namespace vectis::code
