#include "code/dotnet_project_handler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "code/dependency.h"
#include "code/gitignore.h"
#include "code/language.h"
#include "code/path_util.h"
#include "code/symbol.h"
#include "code/xml_reader.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code::dotnet {

namespace {

enum class FileKind : std::uint8_t
{
    Csproj,   // .csproj / .fsproj / .vbproj
    Sln,      // .sln (text)
    Slnx,     // .slnx (XML)
    Props,    // .props / .targets (incl. Directory.Build.* and Directory.Packages.props)
    CpmProps, // Directory.Packages.props specifically
};

[[nodiscard]] std::optional<FileKind> classify_filename(std::string_view filename)
{
    const std::string lower = vectis::core::to_lower_ascii(filename);
    if (lower == "directory.packages.props") {
        return FileKind::CpmProps;
    }
    if (lower.ends_with(".csproj") || lower.ends_with(".fsproj") || lower.ends_with(".vbproj")) {
        return FileKind::Csproj;
    }
    if (lower.ends_with(".slnx")) {
        return FileKind::Slnx;
    }
    if (lower.ends_with(".sln")) {
        return FileKind::Sln;
    }
    if (lower.ends_with(".props") || lower.ends_with(".targets")) {
        return FileKind::Props;
    }
    return std::nullopt;
}

struct FileKindMeta
{
    FileKind kind;
    Language language;
    std::string_view symbol_member;
};

inline constexpr std::array<FileKindMeta, 5> k_file_kind_meta = {{
    {FileKind::Csproj, Language::Csproj, "kind:csproj"},
    {FileKind::Sln, Language::DotNetSolution, "kind:sln"},
    {FileKind::Slnx, Language::DotNetSolution, "kind:slnx"},
    {FileKind::Props, Language::MsbuildProps, "kind:msbuild-props"},
    {FileKind::CpmProps, Language::MsbuildProps, "kind:cpm-props"},
}};

[[nodiscard]] const FileKindMeta& meta_for(FileKind kind)
{
    return k_file_kind_meta[static_cast<std::size_t>(kind)];
}

struct DiscoveredFile
{
    std::filesystem::path absolute_path;
    FileKind kind;
};

void collect_dotnet_files(const manifest_scanner::Config& config, std::vector<DiscoveredFile>& out)
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
        VECTIS_LOG_WARN("DotNetHandler: iterator open failed: {}", e.what());
        return;
    }
    if (ec) {
        VECTIS_LOG_WARN("DotNetHandler: iterator init error: {}", ec.message());
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
            const std::string filename = entry.path().filename().string();
            if (const auto kind = classify_filename(filename)) {
                const std::uint64_t size = entry.file_size(ec);
                if (!ec && size <= config.max_file_size_bytes) {
                    out.push_back({entry.path(), *kind});
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

    std::ranges::sort(out, [](const DiscoveredFile& a, const DiscoveredFile& b) {
        return a.absolute_path.generic_string() < b.absolute_path.generic_string();
    });
}

/// Convert any `\` to `/`. MSBuild paths in cross-platform repos often
/// use Windows separators even when committed from Linux toolchains.
[[nodiscard]] std::string normalise_msbuild_path(std::string_view raw)
{
    std::string out{raw};
    std::ranges::replace(out, '\\', '/');
    return out;
}

[[nodiscard]] std::string with_trailing_slash(const std::filesystem::path& dir)
{
    auto s = dir.generic_string();
    if (!s.empty() && s.back() != '/') {
        s.push_back('/');
    }
    return s;
}

} // namespace

void DotNetHandler::emit_solution_edges(const SolutionEntry& sln, CodeIndex& index,
                                        const manifest_scanner::Config& config,
                                        std::vector<vectis::code::Dependency>& pending)
{
    const auto sln_dir = sln.absolute_path.parent_path();
    for (const auto& proj : sln.projects) {
        const bool extension_ok = is_csharp_family_extension(proj.path);
        const bool guid_ok =
            sln.is_xml_format ? true : is_csharp_family_guid(proj.project_type_guid);
        if (!extension_ok || !guid_ok) {
            continue;
        }
        const std::filesystem::path joined = sln_dir / normalise_msbuild_path(proj.path);
        const std::string rel = normalise_relative(joined, config.root);
        const std::int64_t target = index.file_id_for_path(rel);

        vectis::code::Dependency edge;
        edge.source_file_id = sln.file_id;
        edge.target_file_id = target;
        edge.kind = "sln-project";
        if (target == 0) {
            edge.import_string = proj.path;
        }
        pending.push_back(std::move(edge));
    }
}

void DotNetHandler::emit_csproj_edges(const CsprojEntry& cs, CodeIndex& index,
                                      const manifest_scanner::Config& config,
                                      std::vector<vectis::code::Dependency>& pending) const
{
    const auto cs_dir = cs.absolute_path.parent_path();
    MsbuildContext ctx;
    ctx.repo_root = with_trailing_slash(config.root);
    ctx.this_file_dir = with_trailing_slash(cs_dir);
    ctx.project_name = cs.absolute_path.stem().string();

    const auto emit_path_edge = [&](std::string_view raw_path, std::string_view kind) {
        const std::string substituted =
            substitute_msbuild_builtins(normalise_msbuild_path(raw_path), ctx);

        vectis::code::Dependency edge;
        edge.source_file_id = cs.file_id;
        edge.kind = std::string{kind};
        if (substituted.find("$(") != std::string::npos) {
            edge.import_string = std::string{raw_path};
        }
        else {
            const std::filesystem::path candidate = cs_dir / substituted;
            edge.target_file_id =
                index.file_id_for_path(normalise_relative(candidate, config.root));
            if (edge.target_file_id == 0) {
                edge.import_string = std::string{raw_path};
            }
        }
        pending.push_back(std::move(edge));
    };

    for (const auto& pref : cs.parsed.project_references) {
        emit_path_edge(pref.include_path, "csproj-project");
    }
    for (const auto& imp : cs.parsed.imports) {
        emit_path_edge(imp.project_path, "csproj-import");
    }

    // PackageReference: hoist the nearest-CPM lookup out of the loop so
    // a csproj with hundreds of references doesn't walk the ancestor
    // chain afresh per package.
    const PropertyMap* nearest_cpm = find_nearest_cpm(cs_dir, config.root);
    for (const auto& pkg : cs.parsed.package_references) {
        std::string version = pkg.version;
        if (version.empty() && nearest_cpm != nullptr) {
            if (const auto v = nearest_cpm->find(pkg.name); v != nearest_cpm->end()) {
                version = v->second;
            }
        }

        vectis::code::Dependency edge;
        edge.source_file_id = cs.file_id;
        edge.kind = "csproj-package";
        edge.import_string.reserve(pkg.name.size() + version.size() + 1);
        edge.import_string.append(pkg.name);
        edge.import_string.push_back(':');
        edge.import_string.append(version);
        pending.push_back(std::move(edge));
    }
}

const PropertyMap* DotNetHandler::find_nearest_cpm(const std::filesystem::path& start_dir,
                                                   const std::filesystem::path& root) const
{
    // Build the start path once, then slice off trailing path
    // components as we walk toward the root. Combined with the
    // transparent comparator on `m_cpm_by_dir` this keeps the probe
    // allocation-free per step.
    std::string key = start_dir.generic_string();
    const std::string root_key = root.generic_string();
    while (true) {
        if (const auto it = m_cpm_by_dir.find(std::string_view{key}); it != m_cpm_by_dir.end()) {
            return &it->second;
        }
        if (key.empty() || key == root_key) {
            return nullptr;
        }
        const auto slash = key.find_last_of('/');
        if (slash == std::string::npos) {
            return nullptr;
        }
        key.erase(slash);
    }
}

[[nodiscard]] static std::optional<std::vector<SolutionProjectEntry>>
parse_solution_for(const DiscoveredFile& file, const std::string& content)
{
    if (file.kind == FileKind::Sln) {
        return parse_sln_text(content);
    }
    auto doc = xml::parse(content);
    if (!doc) {
        VECTIS_LOG_WARN("DotNetHandler: malformed .slnx '{}': {}", file.absolute_path.string(),
                        doc.error().message);
        return std::nullopt;
    }
    return parse_slnx(doc->root());
}

void DotNetHandler::register_files(const manifest_scanner::Config& config, CodeIndex& index,
                                   std::unordered_set<std::string>& visited_paths)
{
    m_csprojs.clear();
    m_solutions.clear();
    m_cpm_by_dir.clear();

    std::vector<DiscoveredFile> files;
    collect_dotnet_files(config, files);

    for (const auto& file : files) {
        auto content_result = vectis::platform::read_file(file.absolute_path);
        if (!content_result) {
            VECTIS_LOG_WARN("DotNetHandler: cannot read '{}': {}", file.absolute_path.string(),
                            content_result.error().message);
            continue;
        }
        const std::string& content = *content_result;

        std::error_code ec;
        const auto last_write = std::filesystem::last_write_time(file.absolute_path, ec);

        FileEntry entry;
        entry.path_relative = std::filesystem::relative(file.absolute_path, config.root);
        entry.language = meta_for(file.kind).language;
        entry.size = content.size();
        entry.line_count = vectis::core::count_lines(content);
        entry.content_hash = vectis::core::fnv1a_hex(content);
        if (!ec) {
            entry.last_modified = last_write;
        }

        const std::string rel_str = entry.path_relative.generic_string();
        const std::int64_t file_id = index.add_or_update_file_by_path(std::move(entry));
        visited_paths.insert(rel_str);

        Symbol manifest_symbol;
        manifest_symbol.file_id = file_id;
        manifest_symbol.name = file.absolute_path.stem().string();
        manifest_symbol.kind = SymbolKind::Manifest;
        manifest_symbol.line_start = 1;
        manifest_symbol.line_end = 1;
        manifest_symbol.visibility = Visibility::Public;
        manifest_symbol.members.emplace_back(meta_for(file.kind).symbol_member);
        const std::array<Symbol, 1> batch{std::move(manifest_symbol)};
        index.add_symbols(std::span<const Symbol>(batch.data(), batch.size()));

        switch (file.kind) {
        case FileKind::Csproj: {
            auto doc = xml::parse(content);
            if (!doc) {
                VECTIS_LOG_WARN("DotNetHandler: malformed XML in '{}': {}",
                                file.absolute_path.string(), doc.error().message);
                break;
            }
            CsprojEntry stored;
            stored.file_id = file_id;
            stored.absolute_path = file.absolute_path;
            stored.relative_path = std::filesystem::path{rel_str};
            stored.parsed = parse_csproj(doc->root());
            m_csprojs.push_back(std::move(stored));
            break;
        }
        case FileKind::Sln:
        case FileKind::Slnx: {
            auto projects = parse_solution_for(file, content);
            if (!projects) {
                break;
            }
            SolutionEntry stored;
            stored.file_id = file_id;
            stored.absolute_path = file.absolute_path;
            stored.relative_path = std::filesystem::path{rel_str};
            stored.projects = std::move(*projects);
            stored.is_xml_format = (file.kind == FileKind::Slnx);
            m_solutions.push_back(std::move(stored));
            break;
        }
        case FileKind::CpmProps: {
            auto doc = xml::parse(content);
            if (!doc) {
                VECTIS_LOG_WARN("DotNetHandler: malformed XML in CPM '{}': {}",
                                file.absolute_path.string(), doc.error().message);
                break;
            }
            PropertyMap versions = parse_packages_props(doc->root());
            const std::string dir_key = file.absolute_path.parent_path().generic_string();
            m_cpm_by_dir.emplace(dir_key, std::move(versions));
            break;
        }
        case FileKind::Props:
            // Plain .props/.targets — just registered; no edges
            // sourced from them (their imports flow through csprojs).
            break;
        }
    }
}

void DotNetHandler::emit_edges(const manifest_scanner::Config& config, CodeIndex& index)
{
    std::size_t projected = 0;
    for (const auto& cs : m_csprojs) {
        projected += cs.parsed.project_references.size() + cs.parsed.package_references.size() +
                     cs.parsed.imports.size();
    }
    for (const auto& sln : m_solutions) {
        projected += sln.projects.size();
    }
    std::vector<vectis::code::Dependency> pending;
    pending.reserve(projected);

    for (const auto& sln : m_solutions) {
        emit_solution_edges(sln, index, config, pending);
    }
    for (const auto& cs : m_csprojs) {
        emit_csproj_edges(cs, index, config, pending);
    }

    std::ranges::sort(pending, dependency_emission_less);
    index.add_dependencies(pending);
}

std::shared_ptr<manifest_scanner::Handler> make_dotnet_handler()
{
    return std::make_shared<DotNetHandler>();
}

} // namespace vectis::code::dotnet
