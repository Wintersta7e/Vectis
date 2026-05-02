#include "code/dependency_graph.h"

#include <algorithm>
#include <cstdint>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/symbol.h"

namespace vectis::code {

namespace {

/// Tarjan's SCC runner. Holds the state across the recursive DFS so
/// each node is visited exactly once. Uses explicit recursion
/// because tree depths on large codebases can overrun the default
/// thread stack; see `strongconnect_iterative` below.
struct TarjanState
{
    /// Adjacency list: source_file_id -> list of internal target_file_ids.
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> adj;

    /// Tarjan scratch state, keyed by file_id.
    std::unordered_map<std::int64_t, int> index_of;
    std::unordered_map<std::int64_t, int> lowlink_of;
    std::unordered_map<std::int64_t, bool> on_stack;
    std::vector<std::int64_t> stack_vec;
    int next_index = 0;

    /// Output — all SCCs of size > 1 (or size 1 with self-loop).
    std::vector<DependencyCycle> cycles;

    /// Iterative form of Tarjan's strongconnect, so deep graphs
    /// don't blow the C stack. Each "frame" on m_frames tracks one
    /// in-progress vertex and the iterator through its neighbors.
    void strongconnect_iterative(std::int64_t root)
    {
        struct Frame
        {
            std::int64_t v;
            std::size_t neighbor_idx;
        };
        std::vector<Frame> frames;

        // --- Enter root -----
        index_of[root] = next_index;
        lowlink_of[root] = next_index;
        ++next_index;
        stack_vec.push_back(root);
        on_stack[root] = true;
        frames.push_back(Frame{root, 0});

        while (!frames.empty()) {
            Frame& frame = frames.back();
            const auto adj_it = adj.find(frame.v);

            bool descended = false;
            if (adj_it != adj.end()) {
                const std::vector<std::int64_t>& neighbors = adj_it->second;
                while (frame.neighbor_idx < neighbors.size()) {
                    const std::int64_t w = neighbors[frame.neighbor_idx++];
                    if (index_of.find(w) == index_of.end()) {
                        // Descend into w.
                        index_of[w] = next_index;
                        lowlink_of[w] = next_index;
                        ++next_index;
                        stack_vec.push_back(w);
                        on_stack[w] = true;
                        frames.push_back(Frame{w, 0});
                        descended = true;
                        break;
                    }
                    if (on_stack[w]) {
                        lowlink_of[frame.v] = std::min(lowlink_of[frame.v], index_of[w]);
                    }
                }
            }

            if (descended) {
                continue;
            }

            // All neighbors of frame.v have been processed. Emit an
            // SCC root if applicable, then pop.
            if (lowlink_of[frame.v] == index_of[frame.v]) {
                DependencyCycle scc;
                while (true) {
                    const std::int64_t w = stack_vec.back();
                    stack_vec.pop_back();
                    on_stack[w] = false;
                    scc.file_ids.push_back(w);
                    if (w == frame.v) {
                        break;
                    }
                }
                // Only keep cycles: size > 1, OR size 1 with a self-
                // loop (i.e. the node has an edge to itself).
                const bool has_self_loop = (scc.file_ids.size() == 1) &&
                                           (std::find(adj[frame.v].begin(), adj[frame.v].end(),
                                                      frame.v) != adj[frame.v].end());
                if (scc.file_ids.size() > 1 || has_self_loop) {
                    cycles.push_back(std::move(scc));
                }
            }

            const std::int64_t child_v = frame.v;
            frames.pop_back();
            if (!frames.empty()) {
                const std::int64_t parent = frames.back().v;
                lowlink_of[parent] = std::min(lowlink_of[parent], lowlink_of[child_v]);
            }
        }
    }
};

} // namespace

std::vector<DependencyCycle> detect_cycles(const CodeIndex& index)
{
    TarjanState state;

    // Build the adjacency list from internal (resolved) edges only.
    for (const Dependency& dep : index.all_dependencies()) {
        if (dep.target_file_id == 0) {
            continue; // external, doesn't participate in cycles
        }
        state.adj[dep.source_file_id].push_back(dep.target_file_id);
    }

    // Run strongconnect starting from every vertex that hasn't been
    // visited yet. We need to seed from both adjacency-list keys and
    // from all file_ids in the index so that isolated files don't
    // trip up the traversal. For cycle detection it's enough to seed
    // from every node that appears as the source of at least one
    // internal edge.
    std::unordered_set<std::int64_t> seeds;
    for (const auto& [src, _dsts] : state.adj) {
        seeds.insert(src);
    }
    for (const auto& [_src, dsts] : state.adj) {
        for (const std::int64_t d : dsts) {
            seeds.insert(d);
        }
    }

    for (const std::int64_t v : seeds) {
        if (state.index_of.find(v) == state.index_of.end()) {
            state.strongconnect_iterative(v);
        }
    }

    return state.cycles;
}

} // namespace vectis::code
