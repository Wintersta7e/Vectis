#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "code/xml_reader.h"

namespace vectis::code::dotnet {

/// Transparent-comparator string map. Used for `<PropertyGroup>`
/// values and `Directory.Packages.props` version maps; the
/// `find(string_view)` calls during substitution stay allocation-free.
using PropertyMap = std::map<std::string, std::string, std::less<>>;

/// `<ProjectReference Include="path" />` — single relative path. The
/// path is left as-written (with `\` separators on Windows); the
/// handler normalises before lookup.
struct ProjectReference
{
    std::string include_path;
};

/// `<PackageReference Include="name" [Version="ver"]>`. `version` may
/// contain `$(X)` placeholders that the parser already substituted
/// against the csproj's own `<PropertyGroup>`; remaining unresolved
/// placeholders are passed through to `import_string` so agents see
/// them. Empty `version` means the csproj relied on CPM — the handler
/// fills it in at edge-emission time.
struct PackageReference
{
    std::string name;
    std::string version;
};

/// `<Import Project="path" />` — a MSBuild `.props`/`.targets`
/// inclusion. `project_path` may contain `$()` built-ins which the
/// handler resolves at edge-emission time.
struct ProjectImport
{
    std::string project_path;
};

/// Parsed contents of one `.csproj`/`.fsproj`/`.vbproj`. Property
/// substitution against the csproj's own `<PropertyGroup>` is already
/// applied to `<PackageReference Version="...">` values.
struct CsprojData
{
    /// Concatenation of every `<PropertyGroup>` block — used by the
    /// handler when resolving Version attributes that weren't already
    /// substituted at parse time (the parser does its own pass; this
    /// is the snapshot for any later cross-cutting lookups).
    PropertyMap properties;
    std::vector<ProjectReference> project_references;
    std::vector<PackageReference> package_references;
    std::vector<ProjectImport> imports;
};

/// Parse a `.csproj` / `.fsproj` / `.vbproj` XML tree.
/// Filters out `Remove=` and `Update=` entries — only `Include=` rows
/// produce a Reference. Same-file `<PropertyGroup>` values are
/// substituted into `<PackageReference Version="...">` at parse time.
[[nodiscard]] CsprojData parse_csproj(const xml::Element& root);

/// One project entry from a `.sln` text file or `.slnx` XML file.
/// `project_type_guid` is populated for `.sln` (from the leading
/// `Project("{...}")` GUID), empty for `.slnx` (which has no type
/// GUIDs — filter by extension instead).
struct SolutionProjectEntry
{
    std::string project_type_guid;
    std::string name;
    std::string path;
};

/// Parse a `.sln` text file. Skips folder entries and non-C#/F#/VB
/// projects via the GUID filter built into the implementation; the
/// caller still gets every parsed entry to inspect, with its guid.
[[nodiscard]] std::vector<SolutionProjectEntry> parse_sln_text(std::string_view content);

/// Parse a `.slnx` XML document — newer SDK-style solution format.
/// Walks every `<Project>` element regardless of `<Folder>` nesting.
[[nodiscard]] std::vector<SolutionProjectEntry> parse_slnx(const xml::Element& root);

/// True iff `guid` (without surrounding braces or quotes) is one of
/// the C# / F# / VB / C#-SDK project-type GUIDs Phase 2 emits edges
/// for. Lowercase-compared — `.sln` files always uppercase the GUIDs
/// but tooling can be inconsistent.
[[nodiscard]] bool is_csharp_family_guid(std::string_view guid);

/// True iff `name` (a project path's filename) ends with one of the
/// SDK-style extensions we emit `sln-project` edges for: `.csproj`,
/// `.fsproj`, `.vbproj`. `.vcxproj` is out of scope and rejected.
[[nodiscard]] bool is_csharp_family_extension(std::string_view name);

/// Parse a `Directory.Packages.props` (Central Package Management)
/// file. Builds a `name → version` map from `<PackageVersion Include="..."
/// Version="..." />` entries. The CPM file's own `<PropertyGroup>`
/// values are substituted into each Version at parse time so callers
/// get a flat map. Unresolved `$(X)` references are preserved verbatim.
[[nodiscard]] PropertyMap parse_packages_props(const xml::Element& root);

/// Context for MSBuild built-in `$(X)` substitution. Properties that
/// don't match a known built-in are left verbatim — Phase 2 doesn't
/// resolve user-defined `<PropertyGroup>` values for paths.
struct MsbuildContext
{
    /// `$(RepoRoot)` — wherever `vectis digest` was rooted. Trailing
    /// slash MUST be present so naive `"$(RepoRoot)src/..."` produces a
    /// well-formed path.
    std::filesystem::path repo_root;
    /// `$(ProjectDir)` / `$(MSBuildThisFileDirectory)` — the
    /// directory of the file doing the `<Import>` (or
    /// `<ProjectReference>`). Trailing slash again.
    std::filesystem::path this_file_dir;
    /// `$(MSBuildProjectName)` — csproj filename minus extension.
    std::string project_name;
};

/// Resolve MSBuild built-in `$(X)` placeholders in `input`. Anything
/// else is left as literal `$(X)`. The handler treats remaining
/// placeholders as "external path, can't resolve" and emits the edge
/// externally with the unresolved string preserved.
[[nodiscard]] std::string substitute_msbuild_builtins(std::string_view input,
                                                      const MsbuildContext& ctx);

} // namespace vectis::code::dotnet
