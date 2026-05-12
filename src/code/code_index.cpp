#include "code/code_index.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "core/string_util.h"

namespace vectis::code {

namespace {

/// Cache table PK as a single string: (source_file_id, target_file_id,
/// kind, import_string) joined with U+001F (US), the only ASCII byte
/// that cannot legitimately appear in any of the four fields.
[[nodiscard]] std::string make_dep_key(const Dependency& dep)
{
    std::string key;
    key.reserve(dep.kind.size() + dep.import_string.size() + 24);
    key.append(std::to_string(dep.source_file_id));
    key.push_back('\x1f');
    key.append(std::to_string(dep.target_file_id));
    key.push_back('\x1f');
    key.append(dep.kind);
    key.push_back('\x1f');
    key.append(dep.import_string);
    return key;
}

/// Bit position in `m_language_bits` for a given language. Kept
/// outside the enum so we can increment without renumbering.
[[nodiscard]] constexpr std::uint32_t language_bit(Language language) noexcept
{
    switch (language) {
    case Language::Python:
        return 1U << 0U;
    case Language::JavaScript:
        return 1U << 1U;
    case Language::TypeScript:
        return 1U << 2U;
    case Language::C:
        return 1U << 3U;
    case Language::Cpp:
        return 1U << 4U;
    case Language::Rust:
        return 1U << 5U;
    case Language::Java:
        return 1U << 6U;
    case Language::CSharp:
        return 1U << 7U;
    case Language::Go:
        return 1U << 8U;
    case Language::Ruby:
        return 1U << 9U;
    case Language::Php:
        return 1U << 10U;
    case Language::Sql:
        return 1U << 11U;
    case Language::MavenPom:
        return 1U << 12U;
    case Language::Csproj:
        return 1U << 13U;
    case Language::DotNetSolution:
        return 1U << 14U;
    case Language::SpringXml:
        return 1U << 15U;
    case Language::Properties:
        return 1U << 16U;
    case Language::MsbuildProps:
        return 1U << 17U;
    case Language::Unknown:
        return 0U;
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
    std::string key = file.path_relative.generic_string();

    const std::size_t new_index = m_files.size();
    m_files.push_back(std::move(file));
    m_index_by_path.emplace(std::move(key), new_index);
    m_file_count.store(m_files.size(), std::memory_order_release);

    if (bit != 0U) {
        m_language_bits.fetch_or(bit, std::memory_order_acq_rel);
    }

    m_generation.fetch_add(1, std::memory_order_acq_rel);
    return assigned_id;
}

std::int64_t CodeIndex::add_or_update_file_by_path(FileEntry file)
{
    const std::unique_lock lock(m_mutex);

    const std::string key = file.path_relative.generic_string();
    const auto it = m_index_by_path.find(key);
    if (it == m_index_by_path.end()) {
        // Miss — same path-tracking insert as `add_file`, just inline so
        // we avoid the recursive lock on `m_mutex`.
        const std::int64_t assigned_id = m_next_file_id++;
        file.id = assigned_id;
        const std::uint32_t bit = language_bit(file.language);
        const std::size_t new_index = m_files.size();
        m_files.push_back(std::move(file));
        m_index_by_path.emplace(key, new_index);
        m_file_count.store(m_files.size(), std::memory_order_release);
        if (bit != 0U) {
            m_language_bits.fetch_or(bit, std::memory_order_acq_rel);
        }
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        return assigned_id;
    }

    // Hit — keep the existing id and overwrite the mutable fields.
    const std::size_t idx = it->second;
    FileEntry& existing = m_files[idx];
    const std::int64_t reused_id = existing.id;
    existing.content_hash = std::move(file.content_hash);
    existing.size = file.size;
    existing.line_count = file.line_count;
    existing.language = file.language;
    existing.last_modified = file.last_modified;

    // Recompute the language bitmask from live files since the upserted
    // entry may have switched languages and dropped the only bit of its
    // prior language.
    std::uint32_t new_bits = 0;
    for (const auto& f : m_files) {
        if (f.id != 0) {
            new_bits |= language_bit(f.language);
        }
    }
    m_language_bits.store(new_bits, std::memory_order_release);

    // Clear this file's symbols. Tombstone via file_id=0 (matches
    // remove_file's contract) and erase the lookup so future
    // `symbols_in_file(reused_id)` returns empty.
    const auto by_file_it = m_by_file.find(reused_id);
    std::size_t symbols_cleared = 0;
    if (by_file_it != m_by_file.end()) {
        for (const std::size_t sym_idx : by_file_it->second) {
            m_symbols[sym_idx].file_id = 0;
            ++symbols_cleared;
        }
        m_by_file.erase(by_file_it);
    }
    if (symbols_cleared > 0) {
        const auto sc = m_symbol_count.load(std::memory_order_relaxed);
        m_symbol_count.store(sc >= symbols_cleared ? sc - symbols_cleared : 0,
                             std::memory_order_release);
    }

    // Clear outgoing dependencies (edges where this file is `source`).
    // Incoming edges (where it is `target`) are deliberately preserved
    // per the upsert contract — they were registered by *other* files
    // and aren't affected by re-processing this manifest.
    const auto out_it = m_deps_outgoing.find(reused_id);
    if (out_it != m_deps_outgoing.end()) {
        std::size_t cleared_edges = 0;
        for (const std::size_t dep_idx : out_it->second) {
            Dependency& d = m_dependencies[dep_idx];
            const std::int64_t target_id = d.target_file_id;
            m_dep_keys.erase(make_dep_key(d));
            // Drop the corresponding entry from m_deps_incoming so the
            // target's incoming list doesn't keep stale pointers.
            if (target_id != 0) {
                const auto in_it = m_deps_incoming.find(target_id);
                if (in_it != m_deps_incoming.end()) {
                    auto& list = in_it->second;
                    list.erase(std::remove(list.begin(), list.end(), dep_idx), list.end());
                    if (list.empty()) {
                        m_deps_incoming.erase(in_it);
                    }
                }
            }
            d.source_file_id = 0;
            d.target_file_id = 0;
            ++cleared_edges;
        }
        m_deps_outgoing.erase(out_it);
        if (cleared_edges > 0) {
            const auto dc = m_dependency_count.load(std::memory_order_relaxed);
            m_dependency_count.store(dc >= cleared_edges ? dc - cleared_edges : 0,
                                     std::memory_order_release);
        }
    }

    m_generation.fetch_add(1, std::memory_order_acq_rel);
    return reused_id;
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
        copy.id = m_next_symbol_id++;

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

    if (!m_dep_keys.insert(make_dep_key(dep)).second) {
        return;
    }

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
            m_index_by_path.erase(f.path_relative.generic_string());
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
                Dependency& d = m_dependencies[dep_idx];
                // Erase before tombstoning so a re-resolve of the
                // same edge after this remove is not swallowed.
                m_dep_keys.erase(make_dep_key(d));
                d.source_file_id = 0;
                d.target_file_id = 0;
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
    m_dependency_count.store(dc >= removed_dep_indices.size() ? dc - removed_dep_indices.size() : 0,
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
    m_dep_keys.clear();
    m_index_by_path.clear();
    m_file_count.store(0, std::memory_order_release);
    m_symbol_count.store(0, std::memory_order_release);
    m_dependency_count.store(0, std::memory_order_release);
    m_language_bits.store(0, std::memory_order_release);
    m_next_file_id = 1;
    m_next_symbol_id = 1;
    m_generation.fetch_add(1, std::memory_order_acq_rel);
}

void CodeIndex::compact()
{
    const std::unique_lock lock(m_mutex);

    // Files — keep entries with non-zero id; preserve order so file
    // ids and the lookup maps below stay consistent. Rebuild
    // `m_index_by_path` against the new compacted indices.
    std::vector<FileEntry> live_files;
    live_files.reserve(m_files.size());
    std::unordered_map<std::string, std::size_t> new_index_by_path;
    for (auto& f : m_files) {
        if (f.id != 0) {
            new_index_by_path.emplace(f.path_relative.generic_string(), live_files.size());
            live_files.push_back(std::move(f));
        }
    }
    m_files = std::move(live_files);
    m_index_by_path = std::move(new_index_by_path);

    // Symbols — drop tombstones and rebuild m_by_file with fresh
    // positional indices into the compacted vector.
    std::vector<Symbol> live_symbols;
    live_symbols.reserve(m_symbols.size());
    std::unordered_map<std::int64_t, std::vector<std::size_t>> new_by_file;
    for (auto& s : m_symbols) {
        if (s.file_id == 0) {
            continue;
        }
        new_by_file[s.file_id].push_back(live_symbols.size());
        live_symbols.push_back(std::move(s));
    }
    m_symbols = std::move(live_symbols);
    m_by_file = std::move(new_by_file);

    // Dependencies — same pattern, plus the two outgoing/incoming
    // adjacency maps need fresh positional indices into the
    // compacted edge vector.
    std::vector<Dependency> live_deps;
    live_deps.reserve(m_dependencies.size());
    std::unordered_map<std::int64_t, std::vector<std::size_t>> new_outgoing;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> new_incoming;
    for (auto& d : m_dependencies) {
        if (d.source_file_id == 0 && d.target_file_id == 0) {
            continue;
        }
        const std::size_t new_idx = live_deps.size();
        if (d.source_file_id != 0) {
            new_outgoing[d.source_file_id].push_back(new_idx);
        }
        if (d.target_file_id != 0) {
            new_incoming[d.target_file_id].push_back(new_idx);
        }
        live_deps.push_back(std::move(d));
    }
    m_dependencies = std::move(live_deps);
    m_deps_outgoing = std::move(new_outgoing);
    m_deps_incoming = std::move(new_incoming);

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

std::vector<Symbol> CodeIndex::snapshot_all_symbols() const
{
    std::vector<Symbol> out;
    const std::shared_lock lock(m_mutex);
    out.reserve(m_symbols.size());
    for (const auto& s : m_symbols) {
        if (s.file_id != 0) {
            out.push_back(s);
        }
    }
    return out;
}

std::vector<Symbol> CodeIndex::search_symbols(std::string_view query, std::size_t limit) const
{
    std::vector<Symbol> matches;
    const std::string needle = vectis::core::to_lower_ascii(query);

    {
        const std::shared_lock lock(m_mutex);
        if (needle.empty()) {
            // Empty query → return a cheap prefix of all symbols, so
            // the UI shows something immediately instead of going
            // blank while the user clears the filter.
            matches.reserve(std::min(limit, m_symbols.size()));
            for (std::size_t i = 0; i < m_symbols.size() && matches.size() < limit; ++i) {
                if (m_symbols[i].file_id == 0) {
                    continue; // skip removed
                }
                matches.push_back(m_symbols[i]);
            }
        }
        else {
            for (const Symbol& sym : m_symbols) {
                if (matches.size() >= limit) {
                    break;
                }
                if (sym.file_id == 0) {
                    continue; // skip removed
                }
                const std::string lower_name = vectis::core::to_lower_ascii(sym.name);
                if (lower_name.find(needle) != std::string::npos) {
                    matches.push_back(sym);
                }
            }
        }
    }

    std::sort(matches.begin(), matches.end(),
              [](const Symbol& a, const Symbol& b) { return a.name < b.name; });
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

std::int64_t CodeIndex::file_id_for_path(std::string_view path) const noexcept
{
    const std::shared_lock lock(m_mutex);
    // unordered_map::find requires a key with std::hash<T>; constructing
    // a temporary std::string is cheap relative to the path-walk
    // alternative, and we don't expect this to be a hot path.
    const std::string key{path};
    const auto it = m_index_by_path.find(key);
    if (it == m_index_by_path.end()) {
        return 0;
    }
    return m_files[it->second].id;
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

} // namespace vectis::code
