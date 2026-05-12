#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "code/property_map.h"
#include "code/xml_reader.h"

namespace vectis::code::maven {

/// Backward-compat alias — `code::PropertyMap` is the shared form.
using PropertyMap = code::PropertyMap;

/// Maven coordinate `<groupId>:<artifactId>:<version>`. Fields may be
/// empty at parse time — parent inheritance fills `group_id` and
/// `version` when the POM omits them; property substitution may still
/// leave `${X}` placeholders in any field.
struct Coordinate
{
    std::string group_id;
    std::string artifact_id;
    std::string version;

    /// True iff both `group_id` and `artifact_id` are populated — the
    /// minimum needed to enter the in-repo coordinate registry.
    [[nodiscard]] bool has_artifact_id() const noexcept
    {
        return !group_id.empty() && !artifact_id.empty();
    }

    /// Concatenated `"g:a:v"` form — what JSON consumers see as
    /// `import_ref` / `target_external`.
    [[nodiscard]] std::string gav() const;

    bool operator==(const Coordinate&) const = default;
};

/// One `<dependency>` entry from a POM. Coordinate fields keep any
/// `${X}` placeholders; substitution happens later when the importer's
/// own + parent properties are known.
struct PomDependency
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
    /// when assigning the `maven-bom` kind.
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
    /// `<relativePath>` as written. When `<parent>` is present and
    /// `<relativePath>` is omitted, defaults to `"../pom.xml"` (Maven
    /// semantics). When `<parent>` is present but `<relativePath>` is
    /// *explicitly empty*, the value is an empty string — Maven treats
    /// that as "look up the parent in the repository, not on disk",
    /// and the handler emits the parent edge as external. `nullopt`
    /// when there is no `<parent>` at all.
    std::optional<std::string> parent_relative_path;
    /// Own `<properties>` block.
    code::PropertyMap properties;
    /// `<modules>` child names (NOT resolved to paths).
    std::vector<std::string> modules;
    /// Every `<dependency>` entry found at top-level or under
    /// `<dependencyManagement>`. Entries nested inside `<plugins>` are
    /// filtered out at parse time.
    std::vector<PomDependency> dependencies;
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
/// recursive rules are out of scope today.
[[nodiscard]] std::string substitute_properties(std::string_view input, const Coordinate& own_coord,
                                                const code::PropertyMap& own_props,
                                                const code::PropertyMap& parent_props);

} // namespace vectis::code::maven
