#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "code/code_index.h"
#include "code/manifest_scanner.h"
#include "code/maven_pom.h"

namespace vectis::code::maven {

/// Two-phase handler for Maven POM files. Registers every `pom.xml`
/// found under the scan root in Phase A, then resolves and emits
/// `maven-module` / `maven-parent` / `maven` / `maven-managed` /
/// `maven-bom` edges in Phase B once the in-repo coordinate registry
/// is complete.
///
/// Property substitution uses the importing POM's own `<properties>`
/// plus one hop of parent properties (the parent located via
/// `<relativePath>` defaulting to `../pom.xml`). Grandparent
/// properties are NOT recursed — agents see the literal `${X}` for
/// values that need multi-hop resolution.
///
/// BOM imports (`<type>pom</type><scope>import</scope>`) are always
/// external regardless of whether the coordinate matches an in-repo
/// POM, per the spec — BOMs are catalogue references, not build
/// dependencies.
class PomHandler final : public manifest_scanner::Handler
{
public:
    void register_files(const manifest_scanner::Config& config, CodeIndex& index,
                        std::unordered_set<std::string>& visited_paths) override;

    void emit_edges(const manifest_scanner::Config& config, CodeIndex& index) override;

private:
    struct Entry
    {
        std::int64_t file_id = 0;
        std::filesystem::path absolute_path;
        std::filesystem::path relative_path; // relative to scan root
        ParsedPom parsed;
    };

    std::vector<Entry> m_entries;
    /// `(group_id, artifact_id) → file_id` for in-repo POMs. Phase B's
    /// `<dependency>` resolution consults this — a hit means internal
    /// edge, a miss means external.
    std::map<std::pair<std::string, std::string>, std::int64_t> m_registry;
    /// `file_id → m_entries` index for O(log n) parent-properties
    /// lookup during Phase B. Built alongside `m_registry`.
    std::map<std::int64_t, std::size_t> m_entry_by_id;
};

/// Standalone factory so `manifest_scanner::default_handlers()` can
/// include this handler without dragging the full header into the
/// orchestrator translation unit.
[[nodiscard]] std::shared_ptr<manifest_scanner::Handler> make_pom_handler();

} // namespace vectis::code::maven
