#include "code/dotnet_project.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/string_util.h"

namespace vectis::code::dotnet {

namespace {

/// Generic `$<open>NAME<close>` placeholder substitution. Used for
/// MSBuild `$(X)` and the same-file `<PropertyGroup>` resolution
/// rules. Unresolved placeholders pass through verbatim so agents see
/// what couldn't be resolved.
template <typename Lookup>
std::string substitute_placeholders(std::string_view input, char open, char close,
                                    const Lookup& lookup_fn)
{
    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        if (i + 1 < input.size() && input[i] == '$' && input[i + 1] == open) {
            const std::size_t close_pos = input.find(close, i + 2);
            if (close_pos == std::string_view::npos) {
                out.append(input.substr(i));
                break;
            }
            const std::string_view name = input.substr(i + 2, close_pos - i - 2);
            if (auto resolved = lookup_fn(name)) {
                out.append(*resolved);
            }
            else {
                out.append(input.substr(i, close_pos - i + 1));
            }
            i = close_pos + 1;
            continue;
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

/// Collect every `<PropertyGroup>`'s direct children into the map.
/// MSBuild allows multiple PropertyGroup blocks at root level.
void collect_property_groups(const xml::Element& root, PropertyMap& out)
{
    for (const auto& pg : root.children("PropertyGroup")) {
        for (const auto& prop : pg.children()) {
            out.emplace(std::string{prop.local_name()}, prop.text());
        }
    }
}

[[nodiscard]] std::string substitute_against_props(std::string_view input, const PropertyMap& props)
{
    return substitute_placeholders(input, '(', ')',
                                   [&](std::string_view name) -> std::optional<std::string_view> {
                                       if (const auto it = props.find(name); it != props.end()) {
                                           return std::string_view{it->second};
                                       }
                                       return std::nullopt;
                                   });
}

/// Walk `<Folder>` nesting collecting `<Project Path=...>` elements.
void collect_slnx_projects(const xml::Element& parent, std::vector<SolutionProjectEntry>& out)
{
    for (const auto& child : parent.children()) {
        const auto local = child.local_name();
        if (local == "Project") {
            SolutionProjectEntry entry;
            entry.path = std::string{child.attribute("Path")};
            entry.name = std::string{child.attribute("Name")};
            out.push_back(std::move(entry));
        }
        else if (local == "Folder") {
            collect_slnx_projects(child, out);
        }
    }
}

} // namespace

CsprojData parse_csproj(const xml::Element& root)
{
    CsprojData data;
    collect_property_groups(root, data.properties);

    for (const auto& ig : root.children("ItemGroup")) {
        for (const auto& pref : ig.children("ProjectReference")) {
            const auto include = pref.attribute("Include");
            if (include.empty()) {
                continue; // Remove= / Update= — filtered out
            }
            ProjectReference ref;
            ref.include_path = std::string{include};
            data.project_references.push_back(std::move(ref));
        }
        for (const auto& pkg : ig.children("PackageReference")) {
            const auto include = pkg.attribute("Include");
            if (include.empty()) {
                continue;
            }
            PackageReference ref;
            ref.name = std::string{include};
            const auto version_attr = pkg.attribute("Version");
            if (!version_attr.empty()) {
                ref.version = substitute_against_props(version_attr, data.properties);
            }
            data.package_references.push_back(std::move(ref));
        }
    }

    for (const auto& imp : root.children("Import")) {
        const auto project_attr = imp.attribute("Project");
        if (project_attr.empty()) {
            continue;
        }
        ProjectImport import;
        import.project_path = std::string{project_attr};
        data.imports.push_back(std::move(import));
    }

    return data;
}

std::vector<SolutionProjectEntry> parse_sln_text(std::string_view content)
{
    std::vector<SolutionProjectEntry> out;
    constexpr std::string_view k_prefix = "Project(\"{";

    std::size_t line_start = 0;
    while (line_start <= content.size()) {
        std::size_t line_end = content.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = content.size();
        }
        std::string_view line = content.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.starts_with(k_prefix)) {
            const std::size_t guid_end = line.find("}\")", k_prefix.size());
            if (guid_end != std::string_view::npos) {
                const std::string_view guid =
                    line.substr(k_prefix.size(), guid_end - k_prefix.size());
                std::size_t cur = guid_end + 3; // past "})"
                while (cur < line.size() && (line[cur] == ' ' || line[cur] == '=')) {
                    ++cur;
                }
                std::array<std::string, 3> parts;
                std::size_t got = 0;
                while (cur < line.size() && got < parts.size()) {
                    if (line[cur] != '"') {
                        break;
                    }
                    ++cur;
                    const std::size_t close = line.find('"', cur);
                    if (close == std::string_view::npos) {
                        break;
                    }
                    parts[got++] = std::string{line.substr(cur, close - cur)};
                    cur = close + 1;
                    while (cur < line.size() && (line[cur] == ',' || line[cur] == ' ')) {
                        ++cur;
                    }
                }
                if (got == parts.size()) {
                    SolutionProjectEntry entry;
                    entry.project_type_guid = std::string{guid};
                    entry.name = std::move(parts[0]);
                    entry.path = std::move(parts[1]);
                    out.push_back(std::move(entry));
                }
            }
        }
        if (line_end == content.size()) {
            break;
        }
        line_start = line_end + 1;
    }
    return out;
}

std::vector<SolutionProjectEntry> parse_slnx(const xml::Element& root)
{
    std::vector<SolutionProjectEntry> out;
    collect_slnx_projects(root, out);
    return out;
}

bool is_csharp_family_guid(std::string_view guid)
{
    static constexpr std::array<std::string_view, 4> k_csharp_guids = {
        "FAE04EC0-301F-11D3-BF4B-00C04F79EFBC", // C#
        "F2A71F9B-5D33-465A-A702-920D77279786", // F#
        "F184B08F-C81C-45F6-A57F-5ABD9991F28F", // VB
        "9A19103F-16F7-4668-BE54-9A1E7A4F7556", // C# .NET SDK-style
    };
    const std::string upper = [&] {
        std::string s{guid};
        for (char& c : s) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    }();
    return std::ranges::find(k_csharp_guids, upper) != k_csharp_guids.end();
}

bool is_csharp_family_extension(std::string_view name)
{
    const std::string lower = vectis::core::to_lower_ascii(name);
    return lower.ends_with(".csproj") || lower.ends_with(".fsproj") || lower.ends_with(".vbproj");
}

PropertyMap parse_packages_props(const xml::Element& root)
{
    PropertyMap own_props;
    collect_property_groups(root, own_props);

    PropertyMap versions;
    for (const auto& ig : root.children("ItemGroup")) {
        for (const auto& entry : ig.children("PackageVersion")) {
            const auto name = entry.attribute("Include");
            if (name.empty()) {
                continue;
            }
            const auto version_attr = entry.attribute("Version");
            std::string version = version_attr.empty()
                                      ? std::string{}
                                      : substitute_against_props(version_attr, own_props);
            versions.emplace(std::string{name}, std::move(version));
        }
    }
    return versions;
}

std::string substitute_msbuild_builtins(std::string_view input, const MsbuildContext& ctx)
{
    return substitute_placeholders(
        input, '(', ')', [&](std::string_view name) -> std::optional<std::string> {
            if (name == "RepoRoot") {
                return ctx.repo_root.generic_string();
            }
            if (name == "ProjectDir" || name == "MSBuildThisFileDirectory") {
                return ctx.this_file_dir.generic_string();
            }
            if (name == "MSBuildProjectName") {
                return ctx.project_name;
            }
            return std::nullopt;
        });
}

} // namespace vectis::code::dotnet
