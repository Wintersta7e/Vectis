#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "code/dependency.h"
#include "code/symbol.h"

namespace vectis::code {

/// Thread-safe in-memory index of a scanned codebase.
///
/// Writers (the scanner) call `add_file` / `add_symbols` / `clear`.
/// Readers (the UI) call the `snapshot_*` / `search_*` methods.
///
/// Mutation holds an exclusive lock on the internal `shared_mutex`;
/// queries take a shared lock so multiple UI readers never block each
/// other. Stat counters (`file_count`, `symbol_count`,
/// `language_count`) are backed by atomics so the status bar can poll
/// them every frame without taking any lock.
class CodeIndex
{
public:
    CodeIndex() = default;
    ~CodeIndex() = default;

    CodeIndex(const CodeIndex&) = delete;
    CodeIndex& operator=(const CodeIndex&) = delete;
    CodeIndex(CodeIndex&&) = delete;
    CodeIndex& operator=(CodeIndex&&) = delete;

    // ----- Mutation (writer thread) -----------------------------------

    /// Register a file. Returns the assigned `file_id`, which is a
    /// monotonic 1-based counter. The returned id must be written into
    /// every `Symbol` attached to this file before `add_symbols`.
    std::int64_t add_file(FileEntry file);

    /// Append a batch of symbols to the index. All symbols must share
    /// the same `file_id` (caller's responsibility) and that file must
    /// already exist in the index.
    void add_symbols(std::span<const Symbol> symbols);

    /// Register one dependency edge. Called by the dependency
    /// resolver after a scan completes. Updates both the outgoing and
    /// incoming indexes for O(1) later lookup.
    void add_dependency(Dependency dep);

    /// Remove a single file and all its symbols and dependencies.
    /// Used by incremental re-indexing when a file is deleted or updated.
    void remove_file(std::int64_t file_id);

    /// Drop every file, symbol, and dependency. Safe to call from any
    /// thread as long as no mutation is in flight.
    void clear();

    /// Drop soft-deleted entries (zero-id files/symbols/dependencies)
    /// and rebuild the lookup maps with fresh indices. Long-running
    /// incremental sessions accumulate tombstones via `remove_file`;
    /// `compact()` reclaims that space without invalidating live data.
    /// Caller must ensure no concurrent reader holds a snapshot from
    /// before the call returns.
    void compact();

    // ----- Queries (reader thread) ------------------------------------

    /// Snapshot of every registered file, sorted by relative path.
    /// Allocates and copies; cheap for the ~10k file workloads Step 2
    /// targets.
    [[nodiscard]] std::vector<FileEntry> snapshot_files() const;

    /// All symbols belonging to a single file.
    [[nodiscard]] std::vector<Symbol> symbols_in_file(std::int64_t file_id) const;

    /// Every live symbol in the index, in insertion order. One shared
    /// lock window; preferred over a per-file loop when the caller
    /// needs them all (digest export, cache save).
    [[nodiscard]] std::vector<Symbol> snapshot_all_symbols() const;

    /// Case-insensitive substring search over symbol names. Results
    /// are sorted by name. Capped at `limit` matches to keep the UI
    /// responsive on large indexes.
    [[nodiscard]] std::vector<Symbol> search_symbols(std::string_view query,
                                                     std::size_t limit = 500) const;

    /// Outgoing edges for the given file (what this file depends on).
    [[nodiscard]] std::vector<Dependency> dependencies_of(std::int64_t file_id) const;

    /// Incoming edges for the given file (what depends on this file).
    [[nodiscard]] std::vector<Dependency> dependents_of(std::int64_t file_id) const;

    /// Snapshot of every dependency edge in the project.
    [[nodiscard]] std::vector<Dependency> all_dependencies() const;

    // ----- Stats (lock-free atomic reads) -----------------------------

    [[nodiscard]] std::size_t file_count() const noexcept
    {
        return m_file_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t symbol_count() const noexcept
    {
        return m_symbol_count.load(std::memory_order_acquire);
    }

    /// Number of distinct languages represented in the index (excluding
    /// `Unknown`). Read from a bitmask so it's always O(1).
    [[nodiscard]] std::size_t language_count() const noexcept;

    /// Total number of dependency edges registered.
    [[nodiscard]] std::size_t dependency_count() const noexcept
    {
        return m_dependency_count.load(std::memory_order_acquire);
    }

    /// Monotonic generation counter — incremented on every write
    /// operation. UI code compares against a local snapshot to detect
    /// when a cache rebuild is needed.
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return m_generation.load(std::memory_order_acquire);
    }

private:
    mutable std::shared_mutex m_mutex;
    // Files are stored in insertion order. Right after `add_file` the
    // positional index equals `file_id - 1`, but `compact()` drops
    // tombstoned entries and leaves the surviving ids non-contiguous —
    // never index `m_files` by `id - 1`; iterate with a `id != 0` guard
    // or look up via `m_by_file`.
    std::vector<FileEntry> m_files;
    std::vector<Symbol> m_symbols;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> m_by_file;

    // Dependency graph — edges are stored once in `m_dependencies`
    // and indexed twice (by source and target) for O(1) lookup.
    std::vector<Dependency> m_dependencies;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> m_deps_outgoing;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> m_deps_incoming;

    std::atomic<std::size_t> m_file_count{0};
    std::atomic<std::size_t> m_symbol_count{0};
    std::atomic<std::size_t> m_dependency_count{0};
    std::atomic<std::uint32_t> m_language_bits{0};
    std::atomic<std::uint64_t> m_generation{0};
    std::int64_t m_next_file_id = 1;
    std::int64_t m_next_symbol_id = 1;
};

} // namespace vectis::code
