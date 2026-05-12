#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "code/xml_reader.h"

namespace vectis::code::maven {

/// Maven coordinate `<groupId>:<artifactId>:<version>`. Fields may be
/// empty at parse time — parent inheritance fills `group_id` and
/// `version` when the POM omits them; property substitution may still
/// leave `${X}` placeholders in any field.
struct Coordinate
{
    std::string group_id;
    std::string artifact_id;
    std::string version;

    [[nodiscard]] bool empty() const noexcept
    {
        return group_id.empty() && artifact_id.empty() && version.empty();
    }

    /// Concatenated `"g:a:v"` form — what JSON consumers see as
    /// `import_ref` / `target_external`.
    [[nodiscard]] std::string gav() const;

    bool operator==(const Coordinate&) const = default;
};

/// One `<dependency>` entry from a POM. Coordinate fields keep any
/// `${X}` placeholders; substitution happens later when the importer's
/// own + parent properties are known.
struct Dependency
{
    /// Where the entry was found. The handler dispatches the emitted
    /// edge kind by combining this with `is_bom`.
    enum class Location : std::uint8_t
    {
        TopLevel, ///< `<dependencies>` at the POM root
        Managed,  ///< `<dependencyManagement><dependencies>`
    };

    Coordinate coord;
    Location location = Location::TopLevel;
    /// `<type>pom</type><scope>import</scope>` — wins over `Location`
    /// when assigning the `maven-bom` kind, per the Phase 1 spec.
    bool is_bom = false;
};

/// Parsed contents of one `pom.xml`. `coord` reflects one hop of
/// parent inheritance for `group_id` and `version`. Strings retain
/// `${X}` placeholders; resolve them with `substitute_properties`.
struct ParsedPom
{
    Coordinate coord;
    /// `jar` (default when omitted), `war`, `pom`, etc.
    std::string packaging = "jar";
    /// `<parent>` block, if present.
    std::optional<Coordinate> parent;
    /// `<relativePath>` as written; defaults to `"../pom.xml"` when
    /// `<parent>` is present but `<relativePath>` is omitted (Maven's
    /// documented behaviour). Empty optional when there is no parent.
    std::optional<std::string> parent_relative_path;
    /// Own `<properties>` block.
    std::map<std::string, std::string> properties;
    /// `<modules>` child names (NOT resolved to paths).
    std::vector<std::string> modules;
    /// Every `<dependency>` entry found at top-level or under
    /// `<dependencyManagement>`. Entries nested inside `<plugins>` are
    /// filtered out at parse time.
    std::vector<Dependency> dependencies;
};

/// Parse a POM XML tree into `ParsedPom`. Applies one hop of parent
/// inheritance for `group_id` and `version` (Maven semantics) — does
/// NOT touch the filesystem, does NOT resolve `${X}` placeholders,
/// does NOT cross-reference other POMs.
[[nodiscard]] ParsedPom parse_pom(const xml::Element& root);

/// Resolve `${X}` placeholders in `input` using, in order:
///
///   1. `${project.version}` → `own_coord.version`
///   2. `${project.groupId}` → `own_coord.group_id`
///   3. `${X}` matching a key in `own_props` → that value
///   4. `${X}` matching a key in `parent_props` (one-hop) → that value
///   5. Anything else → leave the literal `${X}` in place.
///
/// Substitution is single-pass — values from `own_props` or
/// `parent_props` are NOT recursively substituted. Maven's full
/// recursive rules are out of scope for Phase 1.
[[nodiscard]] std::string
substitute_properties(std::string_view input, const Coordinate& own_coord,
                      const std::map<std::string, std::string>& own_props,
                      const std::map<std::string, std::string>& parent_props);

} // namespace vectis::code::maven
