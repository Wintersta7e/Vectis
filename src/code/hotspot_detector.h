#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "code/symbol.h"

namespace vectis::code {

class CodeIndex;

/// One "hotspot" — a file or symbol that exceeds one of the
/// configured quality thresholds (high complexity, large file, or
/// high fan-in / fan-out).
///
/// The structured fields (`complexity`, `fan_in`, `fan_out`,
/// `line_count`, plus the symbol locator triplet) duplicate the
/// numbers embedded in `reason` so an agent consumer can read them
/// directly without string-parsing — `reason` stays for human-
/// readable output. Each numeric stays 0 when its trigger didn't
/// fire; the symbol locator stays empty/zero for file-level
/// hotspots.
struct Hotspot
{
    std::int64_t file_id = 0;
    std::int64_t symbol_id = 0;
    std::string symbol_name;
    int line = 0; ///< 1-based start line
    SymbolKind kind = SymbolKind::Unknown;
    std::string reason; ///< "high complexity (24)", "large file (823 lines)", ...
    int severity = 1;   ///< 1 = minor, 2 = moderate, 3 = major

    int complexity = 0;
    int fan_in = 0;
    int fan_out = 0;
    int line_count = 0;
};

/// Default thresholds used by `detect_hotspots`. Conservative and
/// industry-standard. Tunable via config in a future step.
struct HotspotThresholds
{
    int function_complexity = 15; ///< per-function cyclomatic complexity
    int file_line_count = 500;    ///< lines in a single file
    int file_fan_out = 15;        ///< outgoing dependency count
    int file_fan_in = 30;         ///< incoming dependency count
};

/// Scan the populated `CodeIndex` and return every hotspot it
/// contains, sorted by severity (highest first).
///
/// File-level hotspots are emitted once per file regardless of how
/// many triggers fire — the reason string lists them jointly
/// ("large file and high fan-out"). Function-level hotspots are
/// emitted once per symbol.
[[nodiscard]] std::vector<Hotspot> detect_hotspots(const CodeIndex& index,
                                                   HotspotThresholds thresholds = {});

} // namespace vectis::code
