#include "modes/code/code_index.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace vectis::modes::code {

namespace {

/// ASCII lowercase in place. Good enough for symbol-name matching —
/// identifiers in supported languages are overwhelmingly ASCII, and
/// false misses on non-ASCII identifiers are acceptable for Step 2.
[[nodiscard]] std::string to_lower_ascii(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    for (const char ch : input) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// Bit position in `m_language_bits` for a given language. Kept
/// outside the enum so we can increment without renumbering.
[[nodiscard]] constexpr std::uint32_t language_bit(Language language) noexcept
{
    switch (language) {
        case Language::Python:     return 1U << 0U;
        case Language::JavaScript: return 1U << 1U;
        case Language::TypeScript: return 1U << 2U;
        case Language::C:          return 1U << 3U;
        case Language::Cpp:        return 1U << 4U;
        case Language::Rust:       return 1U << 5U;
        case Language::Java:       return 1U << 6U;
        case Language::Unknown:    return 0U;
    }
    return 0U;
}

} // namespace

std::int64_t CodeIndex::add_file(FileEntry file)
{
    const std::unique_lock lock(m_mutex);

    const std::int64_t assigned_id = static_cast<std::int64_t>(m_files.size()) + 1;
    file.id = assigned_id;

    const std::uint32_t bit = language_bit(file.language);

    m_files.push_back(std::move(file));
    m_file_count.store(m_files.size(), std::memory_order_release);

    if (bit != 0U) {
        // Fetch-or so we never lose a bit if another mutator somehow
        // updates concurrently (we hold the unique lock, but belt +
        // braces keeps the field self-consistent w.r.t. reads).
        m_language_bits.fetch_or(bit, std::memory_order_acq_rel);
    }

    return assigned_id;
}

void CodeIndex::add_symbols(std::span<const Symbol> symbols)
{
    if (symbols.empty()) {
        return;
    }

    const std::unique_lock lock(m_mutex);

    m_symbols.reserve(m_symbols.size() + symbols.size());
    for (const Symbol& incoming : symbols) {
        Symbol copy = incoming;
        copy.id     = m_next_symbol_id++;

        const std::size_t symbol_index = m_symbols.size();
        m_by_file[copy.file_id].push_back(symbol_index);
        m_symbols.push_back(std::move(copy));
    }
    m_symbol_count.store(m_symbols.size(), std::memory_order_release);
}

void CodeIndex::clear()
{
    const std::unique_lock lock(m_mutex);
    m_files.clear();
    m_symbols.clear();
    m_by_file.clear();
    m_file_count.store(0, std::memory_order_release);
    m_symbol_count.store(0, std::memory_order_release);
    m_language_bits.store(0, std::memory_order_release);
    m_next_symbol_id = 1;
}

std::vector<FileEntry> CodeIndex::snapshot_files() const
{
    std::vector<FileEntry> copy;
    {
        const std::shared_lock lock(m_mutex);
        copy = m_files;
    }
    std::sort(copy.begin(), copy.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.path_relative < b.path_relative;
    });
    return copy;
}

std::vector<Symbol> CodeIndex::symbols_in_file(std::int64_t file_id) const
{
    const std::shared_lock lock(m_mutex);
    const auto it = m_by_file.find(file_id);
    if (it == m_by_file.end()) {
        return {};
    }
    std::vector<Symbol> out;
    out.reserve(it->second.size());
    for (const std::size_t idx : it->second) {
        out.push_back(m_symbols[idx]);
    }
    return out;
}

std::vector<Symbol> CodeIndex::search_symbols(std::string_view query, std::size_t limit) const
{
    std::vector<Symbol> matches;
    const std::string   needle = to_lower_ascii(query);

    {
        const std::shared_lock lock(m_mutex);
        if (needle.empty()) {
            // Empty query → return a cheap prefix of all symbols, so
            // the UI shows something immediately instead of going
            // blank while the user clears the filter.
            matches.reserve(std::min(limit, m_symbols.size()));
            for (std::size_t i = 0; i < m_symbols.size() && matches.size() < limit; ++i) {
                matches.push_back(m_symbols[i]);
            }
        } else {
            for (const Symbol& sym : m_symbols) {
                if (matches.size() >= limit) {
                    break;
                }
                const std::string lower_name = to_lower_ascii(sym.name);
                if (lower_name.find(needle) != std::string::npos) {
                    matches.push_back(sym);
                }
            }
        }
    }

    std::sort(matches.begin(), matches.end(), [](const Symbol& a, const Symbol& b) {
        return a.name < b.name;
    });
    return matches;
}

std::size_t CodeIndex::language_count() const noexcept
{
    const std::uint32_t bits = m_language_bits.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::popcount(bits));
}

} // namespace vectis::modes::code
