#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vectis::modes::code {

class CodeIndex;

/// One "hotspot" — a file or symbol that exceeds one of the
/// configured quality thresholds (high complexity, large file, or
/// high fan-in / fan-out).
struct Hotspot {
    std::int64_t file_id   = 0;
    std::int64_t symbol_id = 0;    ///< 0 when the hotspot is file-level
    std::string  reason;           ///< "high complexity (24)", "large file (823 lines)", ...
    int          severity  = 1;    ///< 1 = minor, 2 = moderate, 3 = major
};

/// Default thresholds used by `detect_hotspots`. Conservative and
/// industry-standard. Tunable via config in a future step.
struct HotspotThresholds {
    int function_complexity = 15;  ///< per-function cyclomatic complexity
    int file_line_count     = 500; ///< lines in a single file
    int file_fan_out        = 15;  ///< outgoing dependency count
    int file_fan_in         = 30;  ///< incoming dependency count
};

/// Scan the populated `CodeIndex` and return every hotspot it
/// contains, sorted by severity (highest first).
///
/// File-level hotspots are emitted once per file regardless of how
/// many triggers fire — the reason string lists them jointly
/// ("large file and high fan-out"). Function-level hotspots are
/// emitted once per symbol.
[[nodiscard]] std::vector<Hotspot>
detect_hotspots(const CodeIndex&        index,
                HotspotThresholds       thresholds = {});

} // namespace vectis::modes::code
