#include "code/spring_xml_handler.h"

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
#include "code/dependency_resolver.h"
#include "code/gitignore.h"
#include "code/language.h"
#include "code/symbol.h"
#include "code/xml_reader.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::spring {

namespace {

/// Bytes of the file head fed to the cheap `maybe_spring_beans`
/// pre-filter before the full XML parse is attempted.
constexpr std::size_t k_peek_bytes = 4096;

/// Walk the tree under `config.root` collecting every `.xml` file whose
/// directory chain is not excluded and which is within the size cap.
/// Sorted by absolute path so cold and warm runs are bit-identical.
void collect_xml_paths(const manifest_scanner::Config& config,
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
        VECTIS_LOG_WARN("SpringXmlHandler: failed to open recursive iterator: {}", e.what());
        return;
    }
    if (ec) {
        VECTIS_LOG_WARN("SpringXmlHandler: recursive iterator init failed: {}", ec.message());
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
            const std::string ext = vectis::core::to_lower_ascii(entry.path().extension().string());
            if (ext == ".xml") {
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

} // namespace

void SpringXmlHandler::register_files(const manifest_scanner::Config& config, CodeIndex& index,
                                      std::unordered_set<std::string>& visited_paths)
{
    m_entries.clear();

    std::vector<std::filesystem::path> xml_paths;
    collect_xml_paths(config, xml_paths);

    for (const auto& path : xml_paths) {
        const std::filesystem::path rel = std::filesystem::relative(path, config.root);
        const std::string rel_str = rel.generic_string();
        // Skip files already claimed by an earlier handler (e.g. pom.xml
        // registered by the Maven handler -- Spring runs last).
        if (visited_paths.contains(rel_str)) {
            continue;
        }

        auto content_result = vectis::platform::read_file(path);
        if (!content_result) {
            VECTIS_LOG_WARN("SpringXmlHandler: cannot read '{}': {}", path.string(),
                            content_result.error().message);
            continue;
        }
        const std::string& content = *content_result;

        // Cheap 4 KB pre-filter -- rules out non-Spring XML without paying
        // the full parse cost.
        const std::string_view peek = std::string_view{content}.substr(0, k_peek_bytes);
        if (!maybe_spring_beans(peek)) {
            continue;
        }

        auto doc = xml::parse(content);
        if (!doc) {
            VECTIS_LOG_WARN("SpringXmlHandler: malformed XML in '{}': {}", path.string(),
                            doc.error().message);
            continue;
        }
        if (!is_spring_beans_xml(*doc)) {
            continue; // has "<beans" somewhere but is not a Spring beans doc
        }
        ParsedSpringXml parsed = parse_spring_xml(*doc);

        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(path, ec);

        FileEntry entry;
        entry.path_relative = rel;
        entry.language = Language::SpringXml;
        entry.size = content.size();
        entry.line_count = vectis::core::count_lines(content);
        entry.content_hash = vectis::core::fnv1a_hex(content);
        if (!ec) {
            entry.last_modified = last_write;
        }

        const std::int64_t file_id = index.add_or_update_file_by_path(std::move(entry));
        visited_paths.insert(rel_str);

        // Synthetic Symbol so agents browsing the index see the manifest.
        Symbol manifest_symbol;
        manifest_symbol.file_id = file_id;
        manifest_symbol.name = path.stem().string();
        manifest_symbol.kind = SymbolKind::Manifest;
        manifest_symbol.line_start = 1;
        manifest_symbol.line_end = 1;
        manifest_symbol.visibility = Visibility::Public;
        manifest_symbol.members.emplace_back("kind:spring-xml");
        const std::array<Symbol, 1> batch{std::move(manifest_symbol)};
        index.add_symbols(std::span<const Symbol>(batch.data(), batch.size()));

        Entry stored;
        stored.file_id = file_id;
        stored.absolute_path = path;
        stored.relative_path = rel;
        stored.parsed = std::move(parsed);
        m_entries.push_back(std::move(stored));
    }
}

void SpringXmlHandler::emit_edges(const manifest_scanner::Config& /*config*/, CodeIndex& index)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    std::vector<vectis::code::Dependency> pending;

    for (const auto& entry : m_entries) {
        for (const auto& bean : entry.parsed.beans) {
            vectis::code::Dependency edge;
            edge.source_file_id = entry.file_id;
            edge.kind = "spring-bean";
            edge.import_string = bean.fqcn; // full FQCN, incl. any $Inner suffix
            // Strip a nested-class suffix before path-shape resolution,
            // but keep the original FQCN in import_string above.
            const std::string_view stripped = fqcn_without_nested(bean.fqcn);
            const std::vector<std::int64_t> candidates =
                match_java_dotted_candidates(files, stripped);
            if (candidates.size() == 1) {
                edge.target_file_id = candidates.front();
            }
            pending.push_back(std::move(edge));
        }

        for (const auto& scan : entry.parsed.scans) {
            for (const auto& package : scan.packages) {
                vectis::code::Dependency edge;
                edge.source_file_id = entry.file_id;
                edge.kind = "spring-component-scan";
                edge.import_string = package;
                // target_file_id stays 0 — component-scan is always external.
                pending.push_back(std::move(edge));
            }
        }
    }

    std::ranges::sort(pending, dependency_emission_less);
    index.add_dependencies(pending);
}

std::shared_ptr<manifest_scanner::Handler> make_spring_xml_handler()
{
    return std::make_shared<SpringXmlHandler>();
}

} // namespace vectis::code::spring
