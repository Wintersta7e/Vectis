#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/parser.h"

namespace vectis::modes::code {

/// Immutable snapshot of scanner progress, published on the
/// `codebase.scan.progress` ContextBus topic. Values reflect the
/// moment the publish was throttled out of the scanner loop, not the
/// current instant.
struct ScanProgress {
    std::size_t       files_scanned = 0;
    std::uint64_t     files_skipped = 0;
    std::string       current_path;   // relative, may be empty
};

/// Final summary emitted on the `codebase.indexed` topic when a scan
/// completes successfully.
struct ScanSummary {
    std::size_t file_count     = 0;
    std::size_t symbol_count   = 0;
    std::size_t language_count = 0;
};

/// Configuration for one scan run.
struct ScanConfig {
    std::filesystem::path    root;
    std::unordered_set<std::string> exclude_dir_names;
    std::int64_t             epoch = 0;
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
class Scanner {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;
    using CompletionCallback = std::function<void(const ScanSummary&)>;

    /// Execute a full scan. Returns true on normal completion, false
    /// if the scan was cancelled or pre-empted by an epoch bump.
    ///
    /// @param config              Root, excludes, and epoch identifier.
    /// @param index               Caller-owned index to populate.
    /// @param parser              Caller-owned parser (one per worker thread).
    /// @param on_progress         Called at most every 50 files or 100 ms.
    /// @param on_complete         Called once with the final summary.
    /// @param cancel_token        Cooperative cancellation handle.
    /// @param current_epoch       Atomic compared with config.epoch at
    ///                            every batch boundary; mismatch ends
    ///                            the scan immediately.
    static bool run(const ScanConfig&                           config,
                    CodeIndex&                                  index,
                    TreeSitterParser&                           parser,
                    const ProgressCallback&                     on_progress,
                    const CompletionCallback&                   on_complete,
                    const vectis::core::CancellationToken&      cancel_token,
                    const std::atomic<std::int64_t>&            current_epoch);
};

} // namespace vectis::modes::code
