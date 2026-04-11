#include "modes/code/code_index.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_set>
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
        case Language::CSharp:     return 1U << 7U;
        case Language::Go:         return 1U << 8U;
        case Language::Ruby:       return 1U << 9U;
        case Language::Php:        return 1U << 10U;
        case Language::Sql:        return 1U << 11U;
        case Language::Unknown:    return 0U;
    }
    return 0U;
}

} // namespace

std::int64_t CodeIndex::add_file(FileEntry file)
{
    const std::unique_lock lock(m_mutex);

    const std::int64_t assigned_id = m_next_file_id++;
    file.id = assigned_id;

    const std::uint32_t bit = language_bit(file.language);

    m_files.push_back(std::move(file));
    m_file_count.store(m_files.size(), std::memory_order_release);

    if (bit != 0U) {
        m_language_bits.fetch_or(bit, std::memory_order_acq_rel);
    }

    m_generation.fetch_add(1, std::memory_order_acq_rel);
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
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

void CodeIndex::add_dependency(Dependency dep)
{
    const std::unique_lock lock(m_mutex);
    const std::size_t index = m_dependencies.size();
    const std::int64_t source = dep.source_file_id;
    const std::int64_t target = dep.target_file_id;
    m_dependencies.push_back(std::move(dep));
    m_deps_outgoing[source].push_back(index);
    if (target != 0) {
        m_deps_incoming[target].push_back(index);
    }
    m_dependency_count.store(m_dependencies.size(), std::memory_order_release);
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

void CodeIndex::remove_file(std::int64_t file_id)
{
    const std::unique_lock lock(m_mutex);

    // Find and null out the file entry by scanning (IDs may not be positional).
    bool found_file = false;
    for (auto& f : m_files) {
        if (f.id == file_id) {
            f.id = 0;
            f.path_relative.clear();
            found_file = true;
            break;
        }
    }

    // Remove symbols belonging to this file.
    std::size_t symbols_removed = 0;
    const auto by_file_it = m_by_file.find(file_id);
    if (by_file_it != m_by_file.end()) {
        for (const std::size_t sym_idx : by_file_it->second) {
            m_symbols[sym_idx].file_id = 0;
            ++symbols_removed;
        }
        m_by_file.erase(by_file_it);
    }

    // Remove dependencies — track unique dep indices to avoid double-counting.
    std::unordered_set<std::size_t> removed_dep_indices;
    auto collect_deps = [&](std::unordered_map<std::int64_t, std::vector<std::size_t>>& index,
                            std::int64_t key) {
        const auto it = index.find(key);
        if (it != index.end()) {
            for (const std::size_t dep_idx : it->second) {
                m_dependencies[dep_idx].source_file_id = 0;
                m_dependencies[dep_idx].target_file_id = 0;
                removed_dep_indices.insert(dep_idx);
            }
            index.erase(it);
        }
    };
    collect_deps(m_deps_outgoing, file_id);
    collect_deps(m_deps_incoming, file_id);

    // Update counters.
    if (found_file) {
        const auto fc = m_file_count.load(std::memory_order_relaxed);
        m_file_count.store(fc > 0 ? fc - 1 : 0, std::memory_order_release);
    }
    const auto sc = m_symbol_count.load(std::memory_order_relaxed);
    m_symbol_count.store(sc >= symbols_removed ? sc - symbols_removed : 0,
                         std::memory_order_release);
    const auto dc = m_dependency_count.load(std::memory_order_relaxed);
    m_dependency_count.store(
        dc >= removed_dep_indices.size() ? dc - removed_dep_indices.size() : 0,
        std::memory_order_release);

    // Recompute language bitmask from live files.
    std::uint32_t new_bits = 0;
    for (const auto& f : m_files) {
        if (f.id != 0) {
            new_bits |= language_bit(f.language);
        }
    }
    m_language_bits.store(new_bits, std::memory_order_release);

    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

void CodeIndex::clear()
{
    const std::unique_lock lock(m_mutex);
    m_files.clear();
    m_symbols.clear();
    m_by_file.clear();
    m_dependencies.clear();
    m_deps_outgoing.clear();
    m_deps_incoming.clear();
    m_file_count.store(0, std::memory_order_release);
    m_symbol_count.store(0, std::memory_order_release);
    m_dependency_count.store(0, std::memory_order_release);
    m_language_bits.store(0, std::memory_order_release);
    m_next_file_id   = 1;
    m_next_symbol_id = 1;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

std::vector<FileEntry> CodeIndex::snapshot_files() const
{
    std::vector<FileEntry> copy;
    {
        const std::shared_lock lock(m_mutex);
        copy.reserve(m_files.size());
        for (const auto& f : m_files) {
            if (f.id != 0) { // skip removed entries
                copy.push_back(f);
            }
        }
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
                if (m_symbols[i].file_id == 0) continue; // skip removed
                matches.push_back(m_symbols[i]);
            }
        } else {
            for (const Symbol& sym : m_symbols) {
                if (matches.size() >= limit) {
                    break;
                }
                if (sym.file_id == 0) continue; // skip removed
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

std::vector<Dependency> CodeIndex::dependencies_of(std::int64_t file_id) const
{
    const std::shared_lock lock(m_mutex);
    const auto it = m_deps_outgoing.find(file_id);
    if (it == m_deps_outgoing.end()) {
        return {};
    }
    std::vector<Dependency> out;
    out.reserve(it->second.size());
    for (const std::size_t idx : it->second) {
        out.push_back(m_dependencies[idx]);
    }
    return out;
}

std::vector<Dependency> CodeIndex::dependents_of(std::int64_t file_id) const
{
    const std::shared_lock lock(m_mutex);
    const auto it = m_deps_incoming.find(file_id);
    if (it == m_deps_incoming.end()) {
        return {};
    }
    std::vector<Dependency> out;
    out.reserve(it->second.size());
    for (const std::size_t idx : it->second) {
        out.push_back(m_dependencies[idx]);
    }
    return out;
}

std::vector<Dependency> CodeIndex::all_dependencies() const
{
    const std::shared_lock lock(m_mutex);
    std::vector<Dependency> out;
    out.reserve(m_dependencies.size());
    for (const auto& d : m_dependencies) {
        if (d.source_file_id != 0) { // skip removed
            out.push_back(d);
        }
    }
    return out;
}

} // namespace vectis::modes::code
