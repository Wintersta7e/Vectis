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

} // namespace vectis::core
