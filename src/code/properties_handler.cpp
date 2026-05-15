#include "code/properties_handler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "code/dependency.h"
#include "code/gitignore.h"
#include "code/language.h"
#include "code/path_util.h"
#include "code/symbol.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::properties {

namespace {

/// Cap on the number of top-level key prefixes recorded in the
/// synthetic manifest symbol's `members[]`. Spec-mandated.
constexpr std::size_t k_max_prefix_members = 20;

/// Walk the tree under `config.root` collecting every `.properties`
/// file whose directory chain is not excluded and which is within the
/// size cap. Sorted by absolute path so cold and warm runs are
/// bit-identical. Mirrors `collect_xml_paths` in `spring_xml_handler.cpp`.
void collect_properties_paths(const manifest_scanner::Config& config,
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
        VECTIS_LOG_WARN("PropertiesHandler: failed to open recursive iterator: {}", e.what());
        return;
    }
    if (ec) {
        VECTIS_LOG_WARN("PropertiesHandler: recursive iterator init failed: {}", ec.message());
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
        if (entry.is_regular_file(ec) && !ec) {
            // Case-sensitive `.properties` per the spec (matches the
            // scanner's existing case-sensitive convention on Linux).
            if (entry.path().extension() == ".properties") {
                const std::uint64_t size = entry.file_size(ec);
                if (!ec && size <= config.max_file_size_bytes) {
                    out.push_back(entry.path());
                }
                else {
                    ec.clear();
                }
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

/// Build the sorted, unique, capped list of top-level key prefixes
/// (everything before the first `.`) for a parsed `.properties` file.
/// E.g. keys `spring.datasource.url`, `spring.datasource.password`,
/// `logging.level.root`, `app.timeout` -> `app`, `logging`, `spring`.
[[nodiscard]] std::vector<std::string>
top_level_prefixes(const std::vector<PropertiesEntry>& entries)
{
    std::set<std::string> unique;
    for (const auto& e : entries) {
        const std::size_t dot = e.key.find('.');
        std::string prefix = (dot == std::string::npos) ? e.key : e.key.substr(0, dot);
        if (prefix.empty()) {
            continue;
        }
        unique.insert(std::move(prefix));
    }
    std::vector<std::string> result(unique.begin(), unique.end());
    if (result.size() > k_max_prefix_members) {
        result.resize(k_max_prefix_members);
    }
    return result;
}

/// Resolve a `properties-include` value to a file id, or 0 (external)
/// when it cannot be resolved inside the project. `${...}` placeholders
/// always return 0 (the spec defers env-expansion). A leading `/`
/// means project-root-relative (mirrors Phase 3b's spring `<import>`
/// behavior); otherwise the value is taken as relative to the
/// importing file's directory. The raw value is preserved on the edge
/// regardless of resolution.
[[nodiscard]] std::int64_t resolve_include_value(std::string_view raw_value,
                                                 const std::filesystem::path& importer_abs,
                                                 const std::filesystem::path& root,
                                                 const CodeIndex& index)
{
    if (raw_value.empty()) {
        return 0;
    }
    if (raw_value.find("${") != std::string_view::npos) {
        return 0; // placeholder — never resolved
    }
    if (raw_value.starts_with('/')) {
        // Project-root-relative. Strip the leading `/` so the path
        // concatenation does NOT treat the value as filesystem-absolute
        // (`operator/` would replace `root` if we didn't strip).
        const std::string rel =
            normalise_relative(root / std::filesystem::path{raw_value.substr(1)}, root);
        return index.file_id_for_path(rel);
    }
    const std::string rel =
        normalise_relative(importer_abs.parent_path() / std::filesystem::path{raw_value}, root);
    return index.file_id_for_path(rel);
}

} // namespace

void PropertiesHandler::register_files(const manifest_scanner::Config& config, CodeIndex& index,
                                       std::unordered_set<std::string>& visited_paths)
{
    m_entries.clear();

    std::vector<std::filesystem::path> paths;
    collect_properties_paths(config, paths);

    for (const auto& path : paths) {
        const std::filesystem::path rel = std::filesystem::relative(path, config.root);
        const std::string rel_str = rel.generic_string();
        // Skip files already claimed by an earlier handler. No present
        // handler claims `.properties`, but honoring `visited_paths`
        // keeps the handler-contract symmetric and survives future
        // additions that might.
        if (visited_paths.contains(rel_str)) {
            continue;
        }

        auto content_result = vectis::platform::read_file(path);
        if (!content_result) {
            VECTIS_LOG_WARN("PropertiesHandler: cannot read '{}': {}", path.string(),
                            content_result.error().message);
            continue;
        }
        const std::string& content = *content_result;

        std::vector<PropertiesEntry> parsed = parse_properties(content);

        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(path, ec);

        FileEntry entry;
        entry.path_relative = rel;
        entry.language = Language::Properties;
        entry.size = content.size();
        entry.line_count = vectis::core::count_lines(content);
        entry.content_hash = vectis::core::fnv1a_hex(content);
        if (!ec) {
            entry.last_modified = last_write;
        }

        const std::int64_t file_id = index.add_or_update_file_by_path(std::move(entry));
        visited_paths.insert(rel_str);

        // Synthetic Symbol so agents browsing the index see the manifest
        // and a quick fingerprint of its top-level namespace.
        std::vector<std::string> prefixes = top_level_prefixes(parsed);

        Symbol manifest_symbol;
        manifest_symbol.file_id = file_id;
        manifest_symbol.name = path.stem().string();
        manifest_symbol.kind = SymbolKind::Manifest;
        manifest_symbol.line_start = 1;
        manifest_symbol.line_end = 1;
        manifest_symbol.visibility = Visibility::Public;
        manifest_symbol.members = std::move(prefixes);
        const std::array<Symbol, 1> batch{std::move(manifest_symbol)};
        index.add_symbols(std::span<const Symbol>(batch.data(), batch.size()));

        Entry stored;
        stored.file_id = file_id;
        stored.absolute_path = path;
        stored.parsed = std::move(parsed);
        m_entries.push_back(std::move(stored));
    }
}

void PropertiesHandler::emit_edges(const manifest_scanner::Config& config, CodeIndex& index)
{
    std::vector<vectis::code::Dependency> pending;

    for (const auto& entry : m_entries) {
        for (const auto& kv : entry.parsed) {
            // Exact-key match only — substring matches like
            // `filterParameters.include` are deliberately rejected.
            if (kv.key != "spring.config.import" && kv.key != "include") {
                continue;
            }
            // Empty value (e.g. `include=`) has no target to express;
            // emitting an external edge with a blank import_string
            // would just produce a blank `target_external` field.
            if (kv.value.empty()) {
                continue;
            }
            vectis::code::Dependency edge;
            edge.source_file_id = entry.file_id;
            edge.kind = "properties-include";
            edge.import_string = kv.value; // preserved verbatim
            edge.target_file_id =
                resolve_include_value(kv.value, entry.absolute_path, config.root, index);
            pending.push_back(std::move(edge));
        }
    }

    std::ranges::sort(pending, dependency_emission_less);
    index.add_dependencies(pending);
}

std::shared_ptr<manifest_scanner::Handler> make_properties_handler()
{
    return std::make_shared<PropertiesHandler>();
}

} // namespace vectis::code::properties
