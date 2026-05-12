#include "code/maven_pom_handler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "code/dependency.h"
#include "code/gitignore.h"
#include "code/language.h"
#include "code/symbol.h"
#include "code/xml_reader.h"
#include "core/hash.h"
#include "core/log.h"
#include "platform/file_io.h"

namespace vectis::code::maven {

namespace {

/// Skip files larger than the configured cap — same heuristic the
/// source scanner uses.
[[nodiscard]] int count_lines(std::string_view content) noexcept
{
    int count = content.empty() ? 0 : 1;
    for (const char ch : content) {
        if (ch == '\n') {
            ++count;
        }
    }
    return count;
}

/// Lexically normalise an absolute path relative to `root`. Returns
/// the relative path with forward-slash separators so it matches what
/// CodeIndex stores. Does NOT touch the filesystem — handles
/// non-existent paths gracefully (relevant for module / parent
/// resolution where the target may not exist on disk).
[[nodiscard]] std::string normalise_relative(const std::filesystem::path& absolute,
                                             const std::filesystem::path& root)
{
    const std::filesystem::path normalised = absolute.lexically_normal();
    return normalised.lexically_relative(root).generic_string();
}

/// Walk the tree under `config.root` collecting every `pom.xml` whose
/// directory chain is not excluded. Results are sorted by absolute
/// path so cold and warm runs produce bit-identical output.
void collect_pom_paths(const manifest_scanner::Config& config,
                       std::vector<std::filesystem::path>& out)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(config.root, ec) || ec) {
        return;
    }

    using Iter = std::filesystem::recursive_directory_iterator;
    const auto options = std::filesystem::directory_options::skip_permission_denied;

    Iter it{};
    try {
        it = Iter{config.root, options, ec};
    }
    catch (const std::exception& e) {
        VECTIS_LOG_WARN("PomHandler: failed to open recursive iterator: {}", e.what());
        return;
    }
    if (ec) {
        VECTIS_LOG_WARN("PomHandler: recursive iterator init failed: {}", ec.message());
        return;
    }

    const Iter end_it{};
    for (; it != end_it;) {
        const auto& entry = *it;
        if (entry.is_directory(ec)) {
            if (is_excluded_basename(entry.path(), config.exclude_dir_names,
                                     config.exclude_dir_globs)) {
                it.disable_recursion_pending();
            }
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }
        if (entry.is_regular_file(ec) && !ec && entry.path().filename() == "pom.xml") {
            const std::uint64_t size = entry.file_size(ec);
            if (!ec && size <= config.max_file_size_bytes) {
                out.push_back(entry.path());
            }
            else {
                ec.clear();
            }
        }
        try {
            it.increment(ec);
        }
        catch (...) {
            ec.clear();
            break;
        }
    }

    std::ranges::sort(out, [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.generic_string() < b.generic_string();
    });
}

/// Apply property substitution to a coordinate triple in place.
void substitute_coord(Coordinate& target, const Coordinate& own_coord,
                      const std::map<std::string, std::string>& own_props,
                      const std::map<std::string, std::string>& parent_props)
{
    target.group_id = substitute_properties(target.group_id, own_coord, own_props, parent_props);
    target.artifact_id =
        substitute_properties(target.artifact_id, own_coord, own_props, parent_props);
    target.version = substitute_properties(target.version, own_coord, own_props, parent_props);
}

/// Pending edge — built during Phase B emission, sorted, then handed
/// to `index.add_dependency`.
struct PendingEdge
{
    std::int64_t source_file_id = 0;
    std::int64_t target_file_id = 0;
    std::string kind;
    std::string import_string;
};

[[nodiscard]] bool less_pending(const PendingEdge& a, const PendingEdge& b) noexcept
{
    if (a.source_file_id != b.source_file_id) {
        return a.source_file_id < b.source_file_id;
    }
    if (a.kind != b.kind) {
        return a.kind < b.kind;
    }
    if (a.target_file_id != b.target_file_id) {
        return a.target_file_id < b.target_file_id;
    }
    return a.import_string < b.import_string;
}

} // namespace

void PomHandler::register_files(const manifest_scanner::Config& config, CodeIndex& index,
                                std::unordered_set<std::string>& visited_paths)
{
    m_entries.clear();
    m_registry.clear();
    m_entry_by_id.clear();

    std::vector<std::filesystem::path> pom_paths;
    collect_pom_paths(config, pom_paths);

    for (const auto& path : pom_paths) {
        auto content_result = vectis::platform::read_file(path);
        if (!content_result) {
            VECTIS_LOG_WARN("PomHandler: cannot read '{}': {}", path.string(),
                            content_result.error().message);
            continue;
        }
        const std::string& content = *content_result;

        auto doc = xml::parse(content);
        if (!doc) {
            VECTIS_LOG_WARN("PomHandler: malformed XML in '{}': {}", path.string(),
                            doc.error().message);
            continue;
        }
        ParsedPom parsed = parse_pom(doc->root());

        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(path, ec);

        FileEntry entry;
        entry.path_relative = std::filesystem::relative(path, config.root);
        entry.language = Language::MavenPom;
        entry.size = content.size();
        entry.line_count = count_lines(content);
        entry.content_hash = vectis::core::fnv1a_hex(content);
        if (!ec) {
            entry.last_modified = last_write;
        }

        const std::string rel_str = entry.path_relative.generic_string();
        const std::int64_t file_id = index.add_or_update_file_by_path(std::move(entry));
        visited_paths.insert(rel_str);

        // Synthetic Symbol so agents browsing the index see the POM's
        // coordinate (and parent reference, when present).
        Symbol pom_symbol;
        pom_symbol.file_id = file_id;
        pom_symbol.name = parsed.coord.gav();
        pom_symbol.kind = SymbolKind::Manifest;
        pom_symbol.line_start = 1;
        pom_symbol.line_end = 1;
        pom_symbol.visibility = Visibility::Public;
        pom_symbol.members.push_back("packaging:" + parsed.packaging);
        if (parsed.parent.has_value()) {
            pom_symbol.members.push_back("parent:" + parsed.parent->gav());
        }
        const std::array<Symbol, 1> symbol_batch{std::move(pom_symbol)};
        index.add_symbols(std::span<const Symbol>(symbol_batch.data(), symbol_batch.size()));

        const Coordinate own_coord = parsed.coord;
        Entry stored;
        stored.file_id = file_id;
        stored.absolute_path = path;
        stored.relative_path = std::filesystem::path{rel_str};
        stored.parsed = std::move(parsed);
        const std::size_t stored_index = m_entries.size();
        m_entries.push_back(std::move(stored));
        m_entry_by_id.emplace(file_id, stored_index);
        if (!own_coord.group_id.empty() && !own_coord.artifact_id.empty()) {
            m_registry.emplace(std::make_pair(own_coord.group_id, own_coord.artifact_id), file_id);
        }
    }
}

void PomHandler::emit_edges(const manifest_scanner::Config& config, CodeIndex& index)
{
    std::vector<PendingEdge> pending;

    static const std::map<std::string, std::string> k_empty_props;

    for (const auto& entry : m_entries) {
        const ParsedPom& pom = entry.parsed;

        // Resolve the parent file id + parent properties (one hop).
        std::int64_t parent_file_id = 0;
        const std::map<std::string, std::string>* parent_props = &k_empty_props;
        if (pom.parent_relative_path.has_value()) {
            const std::filesystem::path candidate =
                entry.absolute_path.parent_path() / *pom.parent_relative_path;
            const std::string parent_rel = normalise_relative(candidate, config.root);
            parent_file_id = index.file_id_for_path(parent_rel);
            if (parent_file_id != 0) {
                if (const auto it = m_entry_by_id.find(parent_file_id); it != m_entry_by_id.end()) {
                    parent_props = &m_entries[it->second].parsed.properties;
                }
            }
        }

        // maven-parent edge.
        if (pom.parent.has_value()) {
            Coordinate parent_coord = *pom.parent;
            substitute_coord(parent_coord, pom.coord, pom.properties, *parent_props);
            pending.push_back({entry.file_id, parent_file_id, "maven-parent", parent_coord.gav()});
        }

        // maven-module edges.
        for (const auto& module_name : pom.modules) {
            const std::filesystem::path child_path =
                entry.absolute_path.parent_path() / module_name / "pom.xml";
            const std::string child_rel = normalise_relative(child_path, config.root);
            const std::int64_t child_id = index.file_id_for_path(child_rel);
            if (child_id == 0) {
                VECTIS_LOG_WARN("PomHandler: <module>{}</module> in '{}' did not resolve",
                                module_name, entry.relative_path.string());
                continue;
            }
            pending.push_back({entry.file_id, child_id, "maven-module", std::string{}});
        }

        // <dependency> edges — kind chosen by (is_bom, location).
        for (const auto& dep : pom.dependencies) {
            Coordinate resolved = dep.coord;
            substitute_coord(resolved, pom.coord, pom.properties, *parent_props);
            const std::string gav = resolved.gav();

            std::string kind;
            std::int64_t target_id = 0;
            if (dep.is_bom) {
                kind = "maven-bom";
                // BOMs are catalogue references — always external,
                // even when the coordinate matches an in-repo POM.
            }
            else {
                kind = dep.location == Dependency::Location::Managed ? "maven-managed" : "maven";
                if (const auto it =
                        m_registry.find(std::make_pair(resolved.group_id, resolved.artifact_id));
                    it != m_registry.end()) {
                    target_id = it->second;
                }
            }
            pending.push_back({entry.file_id, target_id, std::move(kind), gav});
        }
    }

    std::ranges::sort(pending, less_pending);

    for (const auto& p : pending) {
        vectis::code::Dependency edge;
        edge.source_file_id = p.source_file_id;
        edge.target_file_id = p.target_file_id;
        edge.kind = p.kind;
        edge.import_string = p.import_string;
        index.add_dependency(std::move(edge));
    }
}

std::shared_ptr<manifest_scanner::Handler> make_pom_handler()
{
    return std::make_shared<PomHandler>();
}

} // namespace vectis::code::maven
