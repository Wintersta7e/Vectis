#include "code/maven_pom.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vectis::code::maven {

namespace {

/// Direct-child text helper. Returns the normalised (entity-decoded,
/// whitespace-collapsed) text content of the first child element with
/// the given local name, or empty string when absent.
[[nodiscard]] std::string child_text(const xml::Element& elem, std::string_view local_name)
{
    const auto child = elem.first_child(local_name);
    if (!child) {
        return {};
    }
    return child->text();
}

/// Extract `<groupId>` / `<artifactId>` / `<version>` from an element
/// that uses them as direct children. Works for `<project>`,
/// `<parent>`, and each `<dependency>` block.
[[nodiscard]] Coordinate read_coordinate(const xml::Element& elem)
{
    Coordinate c;
    c.group_id = child_text(elem, "groupId");
    c.artifact_id = child_text(elem, "artifactId");
    c.version = child_text(elem, "version");
    return c;
}

/// Read one `<dependency>` block. `type` and `scope` decide BOM
/// classification — Maven recognises an entry as a BOM import when
/// `<type>pom</type>` AND `<scope>import</scope>` are both present.
[[nodiscard]] Dependency read_dependency(const xml::Element& elem, Dependency::Location location)
{
    Dependency dep;
    dep.coord = read_coordinate(elem);
    dep.location = location;
    dep.is_bom = (child_text(elem, "type") == "pom") && (child_text(elem, "scope") == "import");
    return dep;
}

/// Append every `<dependency>` direct child of `container` to `out`,
/// tagging each with the given location.
void collect_dependencies(const xml::Element& container, Dependency::Location location,
                          std::vector<Dependency>& out)
{
    for (const auto& dep_elem : container.children("dependency")) {
        out.push_back(read_dependency(dep_elem, location));
    }
}

} // namespace

std::string Coordinate::gav() const
{
    std::string out;
    out.reserve(group_id.size() + artifact_id.size() + version.size() + 2);
    out.append(group_id);
    out.push_back(':');
    out.append(artifact_id);
    out.push_back(':');
    out.append(version);
    return out;
}

ParsedPom parse_pom(const xml::Element& root)
{
    ParsedPom pom;

    pom.coord = read_coordinate(root);

    const std::string packaging = child_text(root, "packaging");
    if (!packaging.empty()) {
        pom.packaging = packaging;
    }

    // `<parent>` — record raw coordinate and apply one-hop inheritance
    // for group_id and version. `<relativePath>` defaults to
    // `"../pom.xml"` when omitted (Maven semantics).
    if (const auto parent_elem = root.first_child("parent")) {
        const Coordinate parent_coord = read_coordinate(*parent_elem);
        pom.parent = parent_coord;

        const auto rel_text = child_text(*parent_elem, "relativePath");
        pom.parent_relative_path = rel_text.empty() ? std::string{"../pom.xml"} : rel_text;

        if (pom.coord.group_id.empty()) {
            pom.coord.group_id = parent_coord.group_id;
        }
        if (pom.coord.version.empty()) {
            pom.coord.version = parent_coord.version;
        }
    }

    // `<properties>` — names are arbitrary, so iterate every direct
    // child and use the tag name as the key.
    if (const auto props_elem = root.first_child("properties")) {
        for (const auto& prop : props_elem->children()) {
            pom.properties.emplace(std::string{prop.local_name()}, prop.text());
        }
    }

    // `<modules><module>X</module></modules>`.
    if (const auto modules_elem = root.first_child("modules")) {
        for (const auto& mod : modules_elem->children("module")) {
            pom.modules.push_back(mod.text());
        }
    }

    // Top-level `<dependencies>`. Plugin / build / profile dependency
    // blocks live deeper and are never direct children of `<project>`,
    // so this lookup naturally excludes them.
    if (const auto deps_elem = root.first_child("dependencies")) {
        collect_dependencies(*deps_elem, Dependency::Location::TopLevel, pom.dependencies);
    }

    // `<dependencyManagement><dependencies><dependency>` — same shape
    // but tagged Managed for downstream kind dispatch.
    if (const auto dm_elem = root.first_child("dependencyManagement")) {
        if (const auto dm_deps = dm_elem->first_child("dependencies")) {
            collect_dependencies(*dm_deps, Dependency::Location::Managed, pom.dependencies);
        }
    }

    return pom;
}

namespace {

/// Look up a placeholder name in own → parent → built-ins, in that
/// order. Returns `std::nullopt` when the name resolves nowhere; the
/// caller leaves the literal `${name}` in place.
[[nodiscard]] std::optional<std::string_view>
lookup_property(std::string_view name, const Coordinate& own_coord,
                const std::map<std::string, std::string>& own_props,
                const std::map<std::string, std::string>& parent_props)
{
    if (name == "project.version") {
        return std::string_view{own_coord.version};
    }
    if (name == "project.groupId") {
        return std::string_view{own_coord.group_id};
    }
    if (const auto it = own_props.find(std::string{name}); it != own_props.end()) {
        return std::string_view{it->second};
    }
    if (const auto it = parent_props.find(std::string{name}); it != parent_props.end()) {
        return std::string_view{it->second};
    }
    return std::nullopt;
}

} // namespace

std::string substitute_properties(std::string_view input, const Coordinate& own_coord,
                                  const std::map<std::string, std::string>& own_props,
                                  const std::map<std::string, std::string>& parent_props)
{
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        // Look for the next `${`.
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == '{') {
            const std::size_t close = input.find('}', i + 2);
            if (close == std::string_view::npos) {
                // Unterminated placeholder — copy the rest verbatim.
                out.append(input.substr(i));
                break;
            }
            const std::string_view name = input.substr(i + 2, close - i - 2);
            if (const auto resolved = lookup_property(name, own_coord, own_props, parent_props)) {
                out.append(*resolved);
            }
            else {
                out.append(input.substr(i, close - i + 1));
            }
            i = close + 1;
            continue;
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

} // namespace vectis::code::maven
