#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace vectis::core {

/// FNV-1a 64-bit hash — fast, non-cryptographic, no external deps.
/// Returns a 16-character lowercase hex string suitable for content
/// change detection (not security).
[[nodiscard]] inline std::string fnv1a_hex(std::string_view data) noexcept
{
    constexpr std::uint64_t k_offset = 14695981039346656037ULL;
    constexpr std::uint64_t k_prime  = 1099511628211ULL;

    std::uint64_t hash = k_offset;
    for (const auto byte : data) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= k_prime;
    }

    // Format as 16-char lowercase hex.
    std::array<char, 17> buf{};
    std::snprintf(buf.data(), buf.size(), "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(buf.data(), 16);
}

} // namespace vectis::core
