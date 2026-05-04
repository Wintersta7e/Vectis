#include "code/explain.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "code/architecture_detector.h"
#include "code/code_index.h"
#include "code/dependency_graph.h"
#include "code/language.h"
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
/// into a one-line summary. Empty/unknown visibility is treated as
/// `public` for the API-surface count.
[[nodiscard]] std::string render_visibility(const std::vector<Symbol>& symbols)
{
    std::size_t pub = 0;
    std::size_t priv = 0;
    std::size_t prot = 0;
    std::size_t internal = 0;
    for (const auto& s : symbols) {
        if (s.visibility.empty() || s.visibility == "public") {
            ++pub;
        }
        else if (s.visibility == "private") {
            ++priv;
        }
        else if (s.visibility == "protected") {
            ++prot;
        }
        else if (s.visibility == "internal") {
            ++internal;
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

/// Top-5 symbols by cyclomatic complexity. We do this directly off the
/// symbol list rather than `detect_hotspots()` because the explain
/// summary wants the highest-complexity symbols regardless of whether
/// they cleared the file-level / fan-in / fan-out thresholds.
void render_hotspots(std::ostream& out, const std::vector<Symbol>& symbols,
                     const std::vector<FileEntry>& files)
{
    std::vector<const Symbol*> by_complexity;
    by_complexity.reserve(symbols.size());
    for (const auto& s : symbols) {
        if (s.complexity > 1) { // 1 = straight-line; not interesting
            by_complexity.push_back(&s);
        }
    }
    if (by_complexity.empty()) {
        return;
    }
    std::sort(by_complexity.begin(), by_complexity.end(),
              [](const Symbol* a, const Symbol* b) { return a->complexity > b->complexity; });

    std::unordered_map<std::int64_t, std::string> path_by_id;
    path_by_id.reserve(files.size());
    for (const auto& f : files) {
        path_by_id.emplace(f.id, f.path_relative.generic_string());
    }
    out << "\nTop hotspots (by cyclomatic complexity):\n";
    constexpr std::size_t k_max = 5;
    const std::size_t cap = std::min(k_max, by_complexity.size());
    for (std::size_t i = 0; i < cap; ++i) {
        const Symbol* s = by_complexity[i];
        const auto path_it = path_by_id.find(s->file_id);
        const std::string path = path_it == path_by_id.end() ? std::string{"?"} : path_it->second;
        out << "  " << path << ":" << s->line_start << "  " << s->name << "  ["
            << symbol_kind_name(s->kind) << ", complexity " << s->complexity << "]\n";
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
    const auto cycles = detect_cycles(index);
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

    render_hotspots(out, symbols, files);

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
                ++dec_count[d];
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
