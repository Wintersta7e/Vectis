#pragma once

#include <cstdint>
#include <string>

namespace vectis::code {

/// One directed edge in the project dependency graph.
///
/// `source_file_id` references the file containing the import /
/// include / use / require statement. `target_file_id` references
/// the file the import resolves to — or is 0 if the import could
/// not be resolved to a file inside the scanned project (i.e. it's
/// an external library, stdlib header, or similar).
///
/// The raw `import_string` is preserved either way so consumers can
/// display unresolved externals or debug resolution failures. When the
/// edge is external (`target_file_id == 0`), the JSON exporter renders
/// `import_string` as the `target_external` field. When the edge is
/// internal AND carries a non-empty `import_string`, the exporter
/// renders it as the optional `import_ref` field — useful for Maven /
/// Spring / NuGet edges where the resolved file path alone hides the
/// coordinate or FQCN the agent might want to reason about.
///
/// `kind` taxonomy — every value Vectis emits today, with the
/// introducing phase noted. Values are stable strings; cache load/save
/// round-trips them verbatim.
///
///   Source-language imports (always present):
///     `include`  — C/C++ `#include`
///     `import`   — JS/TS `import`, Java `import`, Python `import`/`from`
///     `use`      — Rust `use`, PHP `use`, C# `using`
///     `require`  — Ruby `require`, Go-style import
///     `mod`      — Rust `mod`
///
///   Maven POM edges (ISSUE-07 phase 1):
///     `maven-module`   — parent pom `<modules><module>` → child pom
///     `maven-parent`   — child pom `<parent>` → parent pom (default
///                        `<relativePath>` is `../pom.xml`)
///     `maven`          — `<dependency>` block, internal or external
///                        coordinate (`groupId:artifactId:version`)
///     `maven-managed`  — `<dependencyManagement><dependency>` — a
///                        catalogue entry, distinct from `maven` so the
///                        live graph isn't polluted by version metadata
///     `maven-bom`      — `<dependency><type>pom</type><scope>import</scope>`
///
///   MSBuild / .NET edges (ISSUE-07 phase 2):
///     `csproj-project` — `<ProjectReference Include="...">`
///     `csproj-package` — `<PackageReference Include="..." [Version="..."]>`
///                        (Version may come from Central Package Mgmt
///                        in `Directory.Packages.props`)
///     `csproj-import`  — `<Import Project="...">` to `.props`/`.targets`
///     `sln-project`    — `.sln` `Project(...) = "name", "X.csproj"` OR
///                        `.slnx` `<Project Path="X.csproj"/>` (filtered
///                        to C#/F#/VB by GUID or file extension)
///
///   Spring XML edges (ISSUE-07 phase 3):
///     `spring-bean`           — `<bean class="FQCN">` (resolved
///                                internally if FQCN matches an
///                                indexed Java class, else external)
///     `spring-import`         — `<import resource="path">`
///     `spring-component-scan` — `<context:component-scan
///                                base-package="X">` (one external
///                                edge per package)
///
///   Properties-file edges (ISSUE-07 phase 4):
///     `properties-include`    — `spring.config.import = X` or
///                                `include = X` (exact key match)
struct Dependency
{
    std::int64_t source_file_id = 0;
    std::int64_t target_file_id = 0; ///< 0 if external / unresolved
    std::string import_string;       ///< e.g. "./foo", "core/log.h"
    std::string kind;                ///< see taxonomy in struct doc above
};

/// True if this dependency resolved to a file inside the project.
[[nodiscard]] inline bool is_resolved(const Dependency& dep) noexcept
{
    return dep.target_file_id != 0;
}

} // namespace vectis::code
