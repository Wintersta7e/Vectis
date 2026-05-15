#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    /// Upsert a file by its relative path. If a file with the same
    /// `path_relative` already exists in the index, the existing entry's
    /// content hash, size, line count, language, and last_modified are
    /// replaced; its symbols and outgoing dependencies (edges where it
    /// is `source`) are cleared. Returns the (possibly reused) file id.
    ///
    /// Incoming edges are preserved — only outgoing edges are dropped.
    /// Path-based dedup is what makes a warm-cache manifest pass safe:
    /// re-running the manifest scanner on an unchanged tree finds the
    /// same pom.xml at the same path and updates in place rather than
    /// inserting a duplicate.
    std::int64_t add_or_update_file_by_path(FileEntry file);

    /// Append a batch of symbols to the index. All symbols must share
    /// the same `file_id` (caller's responsibility) and that file must
    /// already exist in the index.
    void add_symbols(std::span<const Symbol> symbols);

    /// Register one dependency edge. Called by the dependency
    /// resolver after a scan completes. Updates both the outgoing and
    /// incoming indexes for O(1) later lookup.
    void add_dependency(Dependency dep);

    /// Batch variant of `add_dependency` — takes the write lock once
    /// for the whole span. Manifest handlers emit edges in sorted
    /// batches (per the determinism contract), so this collapses
    /// hundreds of lock-acquire/release pairs into one.
    void add_dependencies(std::span<const Dependency> deps);

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

    /// Stream every decorator string from every live symbol through
    /// `fn`. Holds the shared lock for the duration of the walk and
    /// never copies the underlying `Symbol` / decorator vector — use
    /// this when the caller only needs the decorator text (annotation
    /// tally, decorator histogram in the digest) and the full Symbol
    /// payload would be wasted memory. `fn` receives a `std::string_view`
    /// that is valid until the next mutation of the index.
    template <typename F> void for_each_symbol_decorator(F&& fn) const
    {
        std::shared_lock lock(m_mutex);
        for (const auto& sym : m_symbols) {
            for (const auto& dec : sym.decorators) {
                fn(std::string_view{dec});
            }
        }
    }

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

    /// Constant-time path → file-id lookup. Returns 0 if the path is
    /// not registered. Match keys are
    /// `path_relative.generic_string()`. Used by the manifest scanner
    /// to resolve cross-manifest references (e.g. a Maven `<parent>`
    /// pointing at the sibling pom.xml) and by anything else that
    /// needs to find a known file without scanning the snapshot.
    [[nodiscard]] std::int64_t file_id_for_path(std::string_view path) const noexcept;

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
    // `m_dep_keys` matches the cache table PK; see `make_dep_key`.
    std::vector<Dependency> m_dependencies;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> m_deps_outgoing;
    std::unordered_map<std::int64_t, std::vector<std::size_t>> m_deps_incoming;
    std::unordered_set<std::string> m_dep_keys;

    // Path → m_files index for O(1) lookup in
    // `add_or_update_file_by_path`. Keyed by `path_relative.generic_string()`
    // so callers don't have to care about OS path separators. Tombstoned
    // entries are removed by `remove_file`; `compact()` rebuilds the map
    // alongside the other structures.
    //
    // Transparent hash + equal so `find(string_view)` works without
    // constructing a `std::string` — meaningful because the manifest
    // scanner resolves cross-manifest references per edge.
    struct PathHash
    {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view>{}(sv);
        }
        [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, std::size_t, PathHash, std::equal_to<>> m_index_by_path;

    // Private helper — append a new file to `m_files` and update every
    // derived structure (index, count, language bits, generation).
    // Assumes the caller holds `m_mutex` for writing. Used by both
    // `add_file` and the miss branch of `add_or_update_file_by_path`.
    std::int64_t insert_file_locked(FileEntry file, std::string key);

    std::atomic<std::size_t> m_file_count{0};
    std::atomic<std::size_t> m_symbol_count{0};
    std::atomic<std::size_t> m_dependency_count{0};
    std::atomic<std::uint32_t> m_language_bits{0};
    std::atomic<std::uint64_t> m_generation{0};
    std::int64_t m_next_file_id = 1;
    std::int64_t m_next_symbol_id = 1;
};

} // namespace vectis::code
