#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "code/code_index.h"
#include "code/manifest_scanner.h"
#include "code/spring_xml.h"

namespace vectis::code::spring {

/// Two-phase handler for Spring `<beans>` XML wiring. Phase A walks the
/// tree for `.xml` files, content-sniffs each through the
/// `maybe_spring_beans` -> `is_spring_beans_xml` gate, and registers
/// survivors with `Language::SpringXml`. Phase B emits `spring-bean`,
/// `spring-import`, and `spring-component-scan` edges.
///
/// `<bean class="FQCN">` resolves internally only when the FQCN maps to
/// exactly one indexed `.java` file (uniqueness rule, shared with the
/// Java import resolver via `match_java_dotted_candidates`). Ambiguous
/// or unresolved FQCNs emit external edges carrying the literal FQCN.
class SpringXmlHandler final : public manifest_scanner::Handler
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
        ParsedSpringXml parsed;
    };

    std::vector<Entry> m_entries;
};

/// Standalone factory so `manifest_scanner::default_handlers()` can
/// include this handler without dragging the full header into the
/// orchestrator translation unit.
[[nodiscard]] std::shared_ptr<manifest_scanner::Handler> make_spring_xml_handler();

} // namespace vectis::code::spring
