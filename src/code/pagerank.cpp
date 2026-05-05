#include "code/pagerank.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/symbol.h"

namespace vectis::code {

std::vector<PageRankResult> compute_pagerank(std::span<const FileEntry> files,
                                             std::span<const Dependency> deps,
                                             const PageRankOptions& options)
{
    if (files.empty()) {
        return {};
    }

    const std::size_t n = files.size();

    std::unordered_map<std::int64_t, std::size_t> id_to_idx;
    id_to_idx.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        id_to_idx.emplace(files[i].id, i);
    }

    // A `Dependency(src, tgt)` imports tgt from src, which is a vote
    // for tgt, so the edge points src → tgt and rank flows the same
    // direction.
    std::vector<std::vector<std::size_t>> out_edges(n);
    for (const Dependency& dep : deps) {
        const auto src_it = id_to_idx.find(dep.source_file_id);
        const auto tgt_it = id_to_idx.find(dep.target_file_id);
        if (src_it == id_to_idx.end() || tgt_it == id_to_idx.end()) {
            continue;
        }
        if (src_it->second == tgt_it->second) {
            continue;
        }
        out_edges[src_it->second].push_back(tgt_it->second);
    }
    for (std::vector<std::size_t>& adj : out_edges) {
        std::sort(adj.begin(), adj.end());
        adj.erase(std::unique(adj.begin(), adj.end()), adj.end());
    }

    const double n_d = static_cast<double>(n);
    const double base = (1.0 - options.damping) / n_d;

    std::vector<double> rank(n, 1.0 / n_d);
    std::vector<double> next(n);

    for (int iter = 0; iter < options.max_iterations; ++iter) {
        // Dangling-rank pool: rank held by nodes with no out-edges
        // gets redistributed uniformly so total rank stays at 1.
        double dangling = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (out_edges[i].empty()) {
                dangling += rank[i];
            }
        }
        const double dangling_share = options.damping * dangling / n_d;

        for (std::size_t i = 0; i < n; ++i) {
            next[i] = base + dangling_share;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (out_edges[i].empty()) {
                continue;
            }
            const double share =
                options.damping * rank[i] / static_cast<double>(out_edges[i].size());
            for (const std::size_t j : out_edges[i]) {
                next[j] += share;
            }
        }

        double max_delta = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            max_delta = std::max(max_delta, std::abs(next[i] - rank[i]));
        }
        rank.swap(next);
        if (max_delta < options.convergence_eps) {
            break;
        }
    }

    std::vector<PageRankResult> result;
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        result.push_back(PageRankResult{.file_id = files[i].id, .score = rank[i]});
    }
    std::sort(result.begin(), result.end(), [](const PageRankResult& a, const PageRankResult& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.file_id < b.file_id;
    });
    return result;
}

std::vector<PageRankResult> compute_pagerank(const CodeIndex& index, const PageRankOptions& options)
{
    const std::vector<FileEntry> files = index.snapshot_files();
    const std::vector<Dependency> deps = index.all_dependencies();
    return compute_pagerank(std::span{files}, std::span{deps}, options);
}

} // namespace vectis::code
