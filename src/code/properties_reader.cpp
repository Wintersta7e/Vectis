#include "code/properties_reader.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vectis::code::properties {

namespace {

/// True if `ch` is a Java-properties separator-or-whitespace character
/// (the character class that terminates an unescaped key).
[[nodiscard]] constexpr bool is_key_terminator(char ch) noexcept
{
    return ch == '=' || ch == ':' || ch == ' ' || ch == '\t' || ch == '\f';
}

/// True if `ch` is leading-whitespace per Java spec (excludes newline).
[[nodiscard]] constexpr bool is_horizontal_ws(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\f';
}

/// Count the number of trailing backslash characters at the end of `s`.
/// A line with an odd count of trailing `\` is a continuation line per
/// Java's properties spec.
[[nodiscard]] std::size_t trailing_backslash_count(std::string_view s) noexcept
{
    std::size_t count = 0;
    for (auto it = s.rbegin(); it != s.rend() && *it == '\\'; ++it) {
        ++count;
    }
    return count;
}

/// Split `content` into physical lines, preserving line numbers but
/// dropping the terminating `\n` from each line. A final unterminated
/// line is included.
void split_physical_lines(std::string_view content, std::vector<std::string_view>& out)
{
    out.clear();
    std::size_t start = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            std::string_view line = content.substr(start, i - start);
            // Drop a trailing '\r' for CRLF tolerance.
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            out.push_back(line);
            start = i + 1;
        }
    }
    if (start < content.size()) {
        std::string_view line = content.substr(start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.push_back(line);
    }
}

/// Apply the four key-side escape sequences Phase 4 supports:
/// `\=` -> `=`, `\:` -> `:`, `\ ` -> ` `, `\\` -> `\`. Any other escape is
/// left as-is (the leading `\` is preserved). Per Java spec, key-side
/// escapes only cover separator characters and the backslash itself.
[[nodiscard]] std::string unescape_key(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            const char next = raw[i + 1];
            if (next == '=' || next == ':' || next == ' ' || next == '\\') {
                out.push_back(next);
                ++i;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

/// Find the position of the first **unescaped** key terminator in
/// `line`. Returns `std::string_view::npos` if none. An unescaped
/// terminator is one not immediately preceded by an odd number of `\`
/// characters.
[[nodiscard]] std::size_t find_unescaped_terminator(std::string_view line) noexcept
{
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (!is_key_terminator(line[i])) {
            continue;
        }
        // Count backslashes immediately before position i.
        std::size_t bs = 0;
        std::size_t j = i;
        while (j > 0 && line[j - 1] == '\\') {
            ++bs;
            --j;
        }
        if (bs % 2 == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

std::vector<PropertiesEntry> parse_properties(std::string_view content)
{
    std::vector<PropertiesEntry> result;

    // Strip an optional UTF-8 BOM so it does not become part of the
    // first key. Java's `Properties.load(Reader)` consumers typically
    // wrap a UTF-8 decoder that handles this; we do it explicitly.
    constexpr std::string_view k_utf8_bom = "\xEF\xBB\xBF";
    if (content.starts_with(k_utf8_bom)) {
        content.remove_prefix(k_utf8_bom.size());
    }

    std::vector<std::string_view> physical_lines;
    split_physical_lines(content, physical_lines);

    std::string logical;
    std::size_t i = 0;
    while (i < physical_lines.size()) {
        const int line_start = static_cast<int>(i + 1);

        logical.clear();
        bool continues = true;
        bool is_first_phys = true;
        while (continues && i < physical_lines.size()) {
            std::string_view phys = physical_lines[i];
            ++i;
            // Per Java spec: leading whitespace on continuation lines
            // is dropped — `key=line1\\\n  line2` parses as `line1line2`,
            // not `line1  line2`. The first physical line's leading WS
            // is handled later by the logical-line trim.
            if (!is_first_phys) {
                while (!phys.empty() && is_horizontal_ws(phys.front())) {
                    phys.remove_prefix(1);
                }
            }
            is_first_phys = false;
            const std::size_t bs = trailing_backslash_count(phys);
            if (bs % 2 == 1) {
                phys.remove_suffix(1);
                logical.append(phys);
            }
            else {
                logical.append(phys);
                continues = false;
            }
        }

        std::string_view view{logical};
        while (!view.empty() && is_horizontal_ws(view.front())) {
            view.remove_prefix(1);
        }

        if (view.empty()) {
            continue; // blank
        }
        if (view.front() == '#' || view.front() == '!') {
            continue; // comment
        }

        const std::size_t sep_pos = find_unescaped_terminator(view);
        std::string raw_key;
        std::string value;
        if (sep_pos == std::string_view::npos) {
            raw_key = std::string{view};
        }
        else {
            raw_key = std::string{view.substr(0, sep_pos)};
            std::string_view rest = view.substr(sep_pos);
            bool consumed_explicit = false;
            while (!rest.empty()) {
                const char ch = rest.front();
                if (is_horizontal_ws(ch)) {
                    rest.remove_prefix(1);
                }
                else if (!consumed_explicit && (ch == '=' || ch == ':')) {
                    rest.remove_prefix(1);
                    consumed_explicit = true;
                }
                else {
                    break;
                }
            }
            value = std::string{rest};
        }

        PropertiesEntry entry;
        entry.key = unescape_key(raw_key);
        entry.value = std::move(value);
        entry.line_start = line_start;
        result.push_back(std::move(entry));
    }

    return result;
}

} // namespace vectis::code::properties
