#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/manifest_scanner.h"
#include "code/properties_reader.h"

namespace vectis::code::properties {

/// Two-phase handler for `.properties` files. Phase A (`register_files`)
/// walks the tree for `.properties`, parses each, and registers a
/// `FileEntry` with `Language::Properties` plus a `SymbolKind::Manifest`
/// symbol whose `members[]` is the sorted unique top-level key prefix
/// list (capped at 20 prefixes). Phase B (`emit_edges`) emits a
/// `properties-include` edge per parsed entry whose key is exactly
/// `spring.config.import` OR exactly `include` (substring matches like
/// `filterParameters.include` are NOT edges).
class PropertiesHandler final : public manifest_scanner::Handler
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
        std::vector<PropertiesEntry> parsed;
    };

    std::vector<Entry> m_entries;
};

/// Standalone factory so `manifest_scanner::default_handlers()` can
/// include this handler without dragging the full header into the
/// orchestrator translation unit.
[[nodiscard]] std::shared_ptr<manifest_scanner::Handler> make_properties_handler();

} // namespace vectis::code::properties
