#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vectis::code::properties {

/// One key/value entry extracted from a `.properties` file. Keys are
/// unescaped (`\=` / `\:` / `\ ` collapsed to the literal character);
/// values are taken literally (Phase 4 scope decision — value-side
/// escapes are not interpreted).
struct PropertiesEntry
{
    std::string key;
    std::string value;
    int line_start = 0; // 1-based line number of the logical line's first physical line
};

/// Parse the contents of a Java `.properties` file into a flat list of
/// entries in source order. Blank lines and `#`/`!` comment lines are
/// dropped. Physical lines ending in an odd number of trailing `\`
/// characters are concatenated into the following physical line (Java
/// line-continuation semantics). The first unescaped `=`, `:`, or run
/// of whitespace terminates the key. Malformed lines are skipped
/// silently (Phase 4 spec: per-line tolerance — one bad line does not
/// abort file parsing). Returns entries in declaration order; callers
/// that need de-duplication or `last-write-wins` semantics handle that
/// themselves.
[[nodiscard]] std::vector<PropertiesEntry> parse_properties(std::string_view content);

} // namespace vectis::code::properties
