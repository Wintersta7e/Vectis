#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace vectis::code {

/// Transparent-comparator string map. Used for MSBuild
/// `<PropertyGroup>` values, Maven `<properties>`, and CPM
/// `Directory.Packages.props` version maps — `find(string_view)`
/// lands without constructing a `std::string`.
using PropertyMap = std::map<std::string, std::string, std::less<>>;

/// Generic `$<open>NAME<close>` placeholder substitution. `lookup_fn`
/// receives the name between the delimiters and returns an optional
/// resolved value (any string-like type that `std::string::append`
/// accepts). Unresolved placeholders pass through verbatim so agents
/// see what couldn't be resolved.
template <typename Lookup>
[[nodiscard]] std::string substitute_placeholders(std::string_view input, char open, char close,
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

} // namespace vectis::code
