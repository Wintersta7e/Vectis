#include "code/manifest_scanner.h"

namespace vectis::code::manifest_scanner {

std::vector<std::shared_ptr<Handler>> default_handlers()
{
    // Phase 0 ships with no built-in handlers. Phase 1-4 of ISSUE-07
    // will populate this list as each manifest format lands.
    return {};
}

void scan_manifests(const Config& config, CodeIndex& index,
                    std::unordered_set<std::string>& visited_paths,
                    const std::vector<std::shared_ptr<Handler>>& handlers)
{
    // Phase A — every handler registers its files. Done across all
    // handlers before any edges are emitted so cross-manifest
    // references resolve regardless of dispatch order.
    for (const auto& handler : handlers) {
        if (handler) {
            handler->register_files(config, index, visited_paths);
        }
    }

    // Phase B — emit edges with the file table now stable.
    for (const auto& handler : handlers) {
        if (handler) {
            handler->emit_edges(config, index);
        }
    }
}

} // namespace vectis::code::manifest_scanner
