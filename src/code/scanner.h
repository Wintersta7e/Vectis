#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/dependency_resolver.h"
#include "code/parser.h"
#include "core/result.h"
#include "core/task_queue.h"

namespace vectis::code {

/// Immutable snapshot of scanner progress, published on the
/// `codebase.scan.progress` ContextBus topic. Values reflect the
/// moment the publish was throttled out of the scanner loop, not the
/// current instant.
struct ScanProgress
{
    std::size_t files_scanned = 0;
    std::uint64_t files_skipped = 0;
    std::string current_path; // relative, may be empty
};

/// Final summary emitted on the `codebase.indexed` topic when a scan
/// completes successfully.
struct ScanSummary
{
    std::size_t file_count = 0;
    std::size_t symbol_count = 0;
    std::size_t language_count = 0;
    std::uint64_t files_skipped = 0;
};

/// Configuration for one scan run.
struct ScanConfig
{
    std::filesystem::path root;
    /// Exact directory basenames to skip (O(1) hash lookup). Built from
    /// the scanner's defaults plus any non-glob `.gitignore` lines.
    std::unordered_set<std::string> exclude_dir_names;
    /// Wildcard directory-name patterns (`build-*`, `*.egg-info`,
    /// `cmake-build-?`) populated from glob-bearing `.gitignore` lines.
    /// Tested in order against each directory basename via
    /// `wildcard_match`.
    std::vector<std::string> exclude_dir_globs;
    std::int64_t epoch = 0;
};

/// Summary of an incremental scan (how many files were changed).
struct IncrementalScanResult
{
    std::size_t files_added = 0;
    std::size_t files_updated = 0;
    std::size_t files_deleted = 0;
    std::size_t files_unchanged = 0;
    std::uint64_t files_skipped = 0;
};

/// Output of the source-scanner *collect* phase — the new lower-level
/// API that lets the CLI insert the manifest pass between source-walk
/// and edge resolution. Combine `run_collect` (cold) or
/// `run_incremental_collect` (warm) with `prune_missing` (warm only)
/// and `resolve` to produce the same end state the legacy `run` and
/// `run_incremental` wrappers do.
struct ScanResult
{
    /// File / symbol / language counters captured at the end of the
    /// walk. The dependency count is left zero because edges are
    /// emitted later by `Scanner::resolve`.
    ScanSummary summary;
    /// Raw imports collected per file; consumed by `Scanner::resolve`.
    std::vector<FileImports> per_file_imports;
    /// Relative paths the scanner saw on disk during this run. Populated
    /// by `run_incremental_collect`; empty on the cold full path. The
    /// manifest scanner appends to this set before `prune_missing` runs
    /// so registered manifest files survive the prune sweep.
    std::unordered_set<std::string> visited_paths;
    /// Incremental-only counters. Zero on the cold full path.
    std::size_t files_added = 0;
    std::size_t files_updated = 0;
    std::size_t files_unchanged = 0;
};

/// Runs a recursive directory walk on a background thread, detecting
/// languages, parsing source files with `TreeSitterParser`, and
/// populating a `CodeIndex`. Designed to be invoked as a single-shot
/// task via `TaskQueue::submit`.
///
/// All writes go to the caller-owned `CodeIndex`; the scanner does
/// not retain a pointer after the call returns. Progress is reported
/// through a `progress_callback` so callers can marshal to the UI
/// thread via a ContextBus publish (or any other transport).
class Scanner
{
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;
    using CompletionCallback = std::function<void(const ScanSummary&)>;

    /// Execute a full scan.
    ///
    /// Returns `Result<ScanSummary>`:
    /// - **Ok** — scan ran to completion; the payload is the final
    ///   summary (file/symbol/language counts).
    /// - **Err(Cancelled)** — scan was cancelled via the token or
    ///   pre-empted by an epoch bump. Not a hard failure; the caller
    ///   asked to stop. The partial state in `index` may be stale.
    /// - **Err(IoError / PlatformError)** — root is not a directory
    ///   or another unrecoverable filesystem problem occurred.
    ///
    /// `on_complete` is still invoked once at the end of a successful
    /// run (before the result is returned) so bus-based subscribers
    /// receive the summary even if the caller discards the return.
    ///
    /// @param config              Root, excludes, and epoch identifier.
    /// @param index               Caller-owned index to populate.
    /// @param parser              Caller-owned parser (one per worker thread).
    /// @param on_progress         Called at most every 50 files or 100 ms.
    /// @param on_complete         Called once with the final summary on success.
    /// @param cancel_token        Cooperative cancellation handle.
    /// @param current_epoch       Atomic compared with config.epoch at
    ///                            every batch boundary; mismatch ends
    ///                            the scan immediately.
    [[nodiscard]] static vectis::core::Result<ScanSummary>
    run(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
        const ProgressCallback& on_progress, const CompletionCallback& on_complete,
        const vectis::core::CancellationToken& cancel_token,
        const std::atomic<std::int64_t>& current_epoch);

    /// Incremental scan: walks the directory, compares content hashes
    /// against the existing index, and re-parses only changed/new files.
    /// Deleted files are removed from the index. Dependencies are
    /// re-resolved after all changes are applied.
    [[nodiscard]] static vectis::core::Result<IncrementalScanResult>
    run_incremental(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                    const ProgressCallback& on_progress,
                    const vectis::core::CancellationToken& cancel_token,
                    const std::atomic<std::int64_t>& current_epoch);

    // ----- Low-level collect / resolve / prune primitives ------------------
    //
    // Used by the CLI digest path to insert the manifest pass between
    // the source-scan walk and dependency resolution:
    //
    //   auto scan = Scanner::run_collect(...);                          // cold
    //   manifest_scanner::scan_manifests(..., scan->visited_paths);     // adds manifest paths
    //   Scanner::resolve(index, root, scan->per_file_imports);
    //
    //   auto scan = Scanner::run_incremental_collect(...);              // warm
    //   manifest_scanner::scan_manifests(..., scan->visited_paths);
    //   Scanner::prune_missing(index, scan->visited_paths);
    //   Scanner::resolve(index, root, scan->per_file_imports);
    //
    // The existing single-call `run` / `run_incremental` stay as thin
    // wrappers that compose these primitives, so existing callers don't
    // need to migrate.

    /// Cold full scan — walks the tree, parses, adds files & symbols.
    /// Does NOT resolve imports; the caller must follow up with
    /// `resolve(...)` (typically after a manifest pass).
    [[nodiscard]] static vectis::core::Result<ScanResult>
    run_collect(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                const ProgressCallback& on_progress,
                const vectis::core::CancellationToken& cancel_token,
                const std::atomic<std::int64_t>& current_epoch);

    /// Warm cached scan — same walk as `run_collect`, but compares
    /// content hashes against the existing index to re-parse only
    /// changed/new files. Modified files use `remove_file` + `add_file`
    /// to refresh in place. Does NOT prune missing files and does NOT
    /// resolve imports; the caller must invoke `prune_missing` (after
    /// the manifest pass appends to `visited_paths`) and `resolve`.
    [[nodiscard]] static vectis::core::Result<ScanResult>
    run_incremental_collect(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                            const ProgressCallback& on_progress,
                            const vectis::core::CancellationToken& cancel_token,
                            const std::atomic<std::int64_t>& current_epoch);

    /// Turn the per-file raw imports into `Dependency` edges on the
    /// index. Thin wrapper around `resolve_all`; exposed as a Scanner
    /// method so the digest path uses one symbol family for the whole
    /// pipeline.
    static void resolve(CodeIndex& index, const std::filesystem::path& project_root,
                        const std::vector<FileImports>& per_file);

    /// Remove every file in `index` whose `path_relative.generic_string()`
    /// is NOT present in `visited_paths`. Returns the count removed.
    /// Used by the warm-cache path after the source walk and manifest
    /// pass have both contributed to `visited_paths`.
    [[nodiscard]] static std::size_t
    prune_missing(CodeIndex& index, const std::unordered_set<std::string>& visited_paths);
};

} // namespace vectis::code
