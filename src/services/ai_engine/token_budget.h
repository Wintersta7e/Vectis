#pragma once

#include <string_view>

namespace vectis::services {

/// Approximate token count for a UTF-8 string using a flat
/// 3.5-chars-per-token heuristic. Good enough for prompt budgeting
/// — not a substitute for the real BPE tokenizer. English prose
/// is close to 4 chars/token; code is closer to 3. We split the
/// difference.
///
/// Returns `text.size() * 2 / 7`, rounded down. Never returns a
/// negative value; empty input returns 0.
[[nodiscard]] constexpr int estimate_tokens(std::string_view text) noexcept
{
    return static_cast<int>((text.size() * 2U) / 7U);
}

} // namespace vectis::services
