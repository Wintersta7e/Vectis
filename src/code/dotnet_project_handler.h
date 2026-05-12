#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/dotnet_project.h"
#include "code/manifest_scanner.h"

namespace vectis::code::dotnet {

/// Two-phase handler for the .NET project graph. Walks the tree for
/// `*.csproj` / `*.fsproj` / `*.vbproj`, `*.sln` / `*.slnx`,
/// `*.props` / `*.targets`. Phase A registers every file and builds a
/// directory-keyed map of Central Package Management version maps.
/// Phase B emits `csproj-project`, `csproj-package`, `csproj-import`,
/// and `sln-project` edges.
///
/// Path resolution treats `\` as `/` (MSBuild-on-Windows paths are
/// common in cross-platform repos). MSBuild built-in `$(X)` is
/// resolved; user-defined `<PropertyGroup>` values are not.
class DotNetHandler final : public manifest_scanner::Handler
{
public:
    void register_files(const manifest_scanner::Config& config, CodeIndex& index,
                        std::unordered_set<std::string>& visited_paths) override;

    void emit_edges(const manifest_scanner::Config& config, CodeIndex& index) override;

private:
    struct CsprojEntry
    {
        std::int64_t file_id = 0;
        std::filesystem::path absolute_path;
        std::filesystem::path relative_path;
        CsprojData parsed;
    };

    struct SolutionEntry
    {
        std::int64_t file_id = 0;
        std::filesystem::path absolute_path;
        std::filesystem::path relative_path;
        std::vector<SolutionProjectEntry> projects;
        bool is_xml_format = false;
    };

    std::vector<CsprojEntry> m_csprojs;
    std::vector<SolutionEntry> m_solutions;
    /// Nearest-ancestor lookup index: `<dir generic_string()> →
    /// CPM version map`. The directory is the one *containing* a
    /// `Directory.Packages.props` file. Transparent comparator so the
    /// per-package probe loop doesn't allocate a temporary string per
    /// ancestor step.
    std::map<std::string, PropertyMap, std::less<>> m_cpm_by_dir;

    static void emit_solution_edges(const SolutionEntry& sln, CodeIndex& index,
                                    const manifest_scanner::Config& config,
                                    std::vector<vectis::code::Dependency>& pending);
    void emit_csproj_edges(const CsprojEntry& cs, CodeIndex& index,
                           const manifest_scanner::Config& config,
                           std::vector<vectis::code::Dependency>& pending) const;

    /// Walk ancestor directories from `start_dir` toward `root` and
    /// return the nearest CPM map, or nullptr.
    [[nodiscard]] const PropertyMap* find_nearest_cpm(std::filesystem::path start_dir,
                                                      const std::filesystem::path& root) const;
};

[[nodiscard]] std::shared_ptr<manifest_scanner::Handler> make_dotnet_handler();

} // namespace vectis::code::dotnet
