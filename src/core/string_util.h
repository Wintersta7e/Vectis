#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace vectis::core {

/// Locale-independent lowercase copy. Only the 26 ASCII upper letters
/// are folded; bytes outside `A`–`Z` pass through unchanged. Suitable
/// for file extensions, identifier comparisons, and other places
/// where Unicode-aware case folding would be wrong (file paths can
/// be arbitrary bytes on POSIX).
[[nodiscard]] inline std::string to_lower_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// Counterpart of `to_lower_ascii`. ASCII upper-case fold, locale-free.
[[nodiscard]] inline std::string to_upper_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// Count source lines in `content`. Empty content has 0 lines; any
/// non-empty content has at least 1, plus one per embedded `\n`. The
/// last line need not end with a newline.
[[nodiscard]] inline int count_lines(std::string_view content) noexcept
{
    int count = content.empty() ? 0 : 1;
    for (const char ch : content) {
        if (ch == '\n') {
            ++count;
        }
    }
    return count;
}

} // namespace vectis::core
