#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vectis::code {

class CodeIndex;
struct Dependency;
struct FileEntry;

struct PageRankResult
{
    std::int64_t file_id = 0;
    double score = 0.0;
};

/// Tuning knobs for `compute_pagerank`. Defaults follow the standard
/// Brin/Page recipe — d=0.85 damping, 100-iteration cap, 1e-6 L∞
/// convergence — which works well on file-level graphs of any size we
/// realistically see (tens of thousands of files at most).
struct PageRankOptions
{
    double damping = 0.85;
    int max_iterations = 100;
    double convergence_eps = 1e-6;
};

/// Compute file-level PageRank over the dependency graph of `index`.
///
/// Edge convention: a `Dependency(source → target)` (i.e. "source
/// imports target") is treated as a vote *for* `target`, mirroring the
/// classic web-page PageRank metaphor. So a file imported by many
/// importers — a hub header, a core data type — accumulates rank, and
/// the importers themselves don't unless they're also imported.
///
/// Self-loops and parallel edges are de-duped before iteration so a
/// file can't inflate its own (or a peer's) score by importing
/// something twice.
///
/// Dangling files (no outgoing imports) have their rank redistributed
/// uniformly across all nodes per iteration — the standard treatment
/// that keeps total rank conserved.
///
/// Returned vector is sorted by `score` descending; ties resolved by
/// `file_id` ascending so output is reproducible across runs. Returns
/// empty if `index` has no files.
[[nodiscard]] std::vector<PageRankResult>
compute_pagerank(const CodeIndex& index, const PageRankOptions& options = {});

/// Lower-level overload for callers that already hold `files` and the
/// dep list — saves the two `CodeIndex` snapshots that the
/// index-taking overload would otherwise re-do under shared lock.
[[nodiscard]] std::vector<PageRankResult>
compute_pagerank(std::span<const FileEntry> files, std::span<const Dependency> deps,
                 const PageRankOptions& options = {});

} // namespace vectis::code
