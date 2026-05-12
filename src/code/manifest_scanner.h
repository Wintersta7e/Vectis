#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"

namespace vectis::code::manifest_scanner {

/// Configuration for one manifest pass. Mirrors `ScanConfig` from the
/// source scanner — the two run over the same tree with the same
/// exclude rules and per-file size cap.
struct Config
{
    std::filesystem::path root;
    /// Exact directory basenames to skip (`node_modules`, `.git`, …).
    std::unordered_set<std::string> exclude_dir_names;
    /// Wildcard directory patterns (`build-*`, `*.egg-info`, …).
    std::vector<std::string> exclude_dir_globs;
    /// Cancellation hook used by the scanner; the manifest pass reuses
    /// the same epoch + token so a cancelled scan stops both phases.
    std::int64_t epoch = 0;
    /// Per-file size cap. Files larger than this are skipped without
    /// being parsed. Defaults to 2 MB to match the source scanner.
    std::uint64_t max_file_size_bytes = 2ULL * 1024 * 1024;
};

/// One handler for a single manifest format (POM, csproj, Spring XML,
/// .properties, etc.). Handlers MUST follow the documented two-phase
/// lifecycle so cross-manifest references resolve regardless of
/// dispatch order:
///
///   Phase A — register_files(): walk for this handler's filename
///     pattern (e.g. `pom.xml`), sort by `path_relative.generic_string()`,
///     register each file via `index.add_or_update_file_by_path(...)`,
///     and append its relative path to `visited_paths` so the
///     incremental-scan prune sweep doesn't delete it.
///
///   Phase B — emit_edges(): once every handler has finished Phase A
///     across the whole tree, emit pending `Dependency` edges via
///     `index.add_dependency(...)`. Edges MUST be sorted by
///     `(source_file_id, kind, target_or_import_string)` before
///     insertion so cold and warm runs produce bit-identical output.
class Handler
{
public:
    virtual ~Handler() = default;

    Handler() = default;
    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

    /// Walk the tree and register files this handler owns. Append every
    /// registered path to `visited_paths`.
    virtual void register_files(const Config& config, CodeIndex& index,
                                std::unordered_set<std::string>& visited_paths) = 0;

    /// Emit dependency edges sourced from this handler's manifest files.
    /// All Phase A registration is complete by the time this is called.
    virtual void emit_edges(const Config& config, CodeIndex& index) = 0;
};

/// Built-in handler set for the CLI digest path. Phase 0 ships with an
/// empty list; Phase 1-4 of ISSUE-07 will add Maven POM, csproj/sln,
/// Spring XML, and .properties handlers in turn.
[[nodiscard]] std::vector<std::shared_ptr<Handler>> default_handlers();

/// Run the manifest pass. Two strictly-ordered loops:
///   1. For every handler: `register_files(...)`.
///   2. For every handler: `emit_edges(...)`.
///
/// This is what guarantees that a Phase B edge from `a.csproj` to a
/// sibling `b.csproj` resolves internally regardless of which handler
/// was dispatched first or the order of the filesystem walk.
///
/// Every element of `handlers` must be non-null; the orchestrator
/// dereferences without checking.
void scan_manifests(const Config& config, CodeIndex& index,
                    std::unordered_set<std::string>& visited_paths,
                    const std::vector<std::shared_ptr<Handler>>& handlers);

} // namespace vectis::code::manifest_scanner
