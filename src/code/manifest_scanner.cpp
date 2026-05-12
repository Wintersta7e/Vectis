#include "code/manifest_scanner.h"

#include "code/dotnet_project_handler.h"
#include "code/maven_pom_handler.h"

namespace vectis::code::manifest_scanner {

std::vector<std::shared_ptr<Handler>> default_handlers()
{
    return {
        vectis::code::maven::make_pom_handler(),
        vectis::code::dotnet::make_dotnet_handler(),
    };
}

void scan_manifests(const Config& config, CodeIndex& index,
                    std::unordered_set<std::string>& visited_paths,
                    const std::vector<std::shared_ptr<Handler>>& handlers)
{
    // Phase A — every handler registers its files. Done across all
    // handlers before any edges are emitted so cross-manifest
    // references resolve regardless of dispatch order.
    for (const auto& handler : handlers) {
        handler->register_files(config, index, visited_paths);
    }

    // Phase B — emit edges with the file table now stable.
    for (const auto& handler : handlers) {
        handler->emit_edges(config, index);
    }
}

} // namespace vectis::code::manifest_scanner
