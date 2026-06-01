#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace vectis::code {

/// Per-edge resolution-fidelity model for Python and Go `import` edges.
///
/// Vectis's resolver records *whether* an import resolved to a file
/// inside the project, but not *how confident* that resolution is.
/// This module reconstructs the resolution strategy from the edge's
/// existing data (import string + resolved target path) — mirroring the
/// offline Stage-1 ground-truth harness — and attaches a calibrated
/// per-strategy precision so a consuming agent can weight edges instead
/// of treating every resolved/unresolved edge as equally trustworthy.
/// Each language has its own strata, calibration table, and version pin.
///
/// All inputs come from the exporter's existing edge view, so nothing
/// here touches the resolver and the digest stays byte-deterministic.

/// Version pin for the calibration table below. Bump whenever any
/// `k_py_*` confidence value or the strategy taxonomy changes so a
/// consumer can detect a recalibration across digest runs.
inline constexpr std::string_view k_python_fidelity_version = "py-import-2026-06-01";

// --- Calibration table (Python import edges) ---------------------------------
//
// Manual ground-truth study, measured 2026-06-01 against the
// src-layout-aware resolver. Corpus = 2 Python projects, 112-edge
// labelled sample. Per-strategy precision was observed near ~1.0 for
// every resolved strategy; published at a deliberately conservative 0.95
// so the figure doesn't over-promise on a small sample. Distribution-level,
// NOT a per-repo guarantee (see build_fidelity_metadata_json's caveat).
//
// Resolved strategies (relative/dotted × package/module) observed 72/72
// correct (n in [12, 20] per stratum); published conservatively.
inline constexpr double k_py_resolved_confidence = 0.95;
// Unresolved, leading-dot relative import. Higher trust than dotted:
// a stray relative import is almost always a real (if unscanned) module.
inline constexpr double k_py_external_relative_confidence = 0.90;
// Unresolved, no leading dot. 39/40 truly-external (n=40) once src-layout
// imports resolve; the lone residual was an example-tree package root (a
// known follow-up). Published at 0.95.
inline constexpr double k_py_external_dotted_confidence = 0.95;

/// Version pin for the Go calibration table below; bump on any change to
/// the `k_go_*` values or the Go strategy taxonomy.
inline constexpr std::string_view k_go_fidelity_version = "go-import-2026-06-01";

// --- Calibration table (Go import edges) -------------------------------------
//
// Spec-oracle ground-truth study, measured 2026-06-01 against the go.mod
// module-prefix resolver. Corpus = 3 Go projects, 90-edge stratified
// sample (40 internal / 50 external). Go's resolution is mechanical, so
// the oracle is an automated go.mod-prefix check + anomaly review rather
// than manual labelling. Every stratum was observed perfect (go-internal
// 40/40; 0/50 false-externals); published conservatively below.
// Distribution-level, NOT a per-repo guarantee.
//
// Resolved (internal) import: the unambiguous module-prefix model gives
// the highest figure of the three.
inline constexpr double k_go_internal_confidence = 0.98;
// Unresolved standard-library import (first path segment has no dot, e.g.
// `fmt`, `database/sql`): structurally certain to be external.
inline constexpr double k_go_external_stdlib_confidence = 0.95;
// Unresolved third-party import (first path segment is a domain, e.g.
// `github.com/...`): open-ended namespace, same observed precision.
inline constexpr double k_go_external_thirdparty_confidence = 0.95;

/// Reconstruct the resolution strategy string for one Python import
/// edge, mirroring the Stage-1 harness:
///   - relative iff `import_string` begins with '.', else dotted;
///   - external (target null / unresolved): `external-<relative|dotted>`;
///   - resolved: `<relative|dotted>-<package|module>`, where package
///     iff the resolved target path ends in `__init__.py`.
/// `target_relpath` is ignored when `is_external` is true.
[[nodiscard]] std::string reconstruct_python_resolved_by(std::string_view import_string,
                                                         std::string_view target_relpath,
                                                         bool is_external);

/// Calibrated precision for a strategy string produced by
/// `reconstruct_python_resolved_by`. Unknown strategies return 0.0 so a
/// future taxonomy addition fails closed (no false high confidence)
/// rather than silently inheriting a neighbour's number.
[[nodiscard]] double python_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy string for one Go import edge:
///   - resolved (`is_external` false): `go-internal` — the import matched
///     the go.mod module prefix and points at a package inside the tree;
///   - external: `go-external-stdlib` if the first `/`-separated segment
///     has no `.` (`fmt`, `database/sql`), else `go-external-thirdparty`
///     (`github.com/...`). Mirrors the Stage-1 harness.
[[nodiscard]] std::string reconstruct_go_resolved_by(std::string_view import_string,
                                                     bool is_external);

/// Calibrated precision for a strategy string produced by
/// `reconstruct_go_resolved_by`. Unknown strategies return 0.0 (fail
/// closed), as with `python_edge_confidence`.
[[nodiscard]] double go_edge_confidence(std::string_view strategy);

/// One edge's reconstructed resolution strategy plus its calibrated
/// precision. Returned by `reconstruct_edge_fidelity`.
struct EdgeFidelity
{
    std::string resolved_by;
    double confidence;
};

/// Reconstruct fidelity for one dependency edge, dispatching on the source
/// file's extension and the edge `kind` to the matching per-language model.
/// Returns `std::nullopt` for any (language, kind) pair that isn't
/// calibrated, so the exporter leaves such edges untouched. Pure function
/// of the edge's existing data — never re-runs resolution. `target_relpath`
/// is the resolved target's path (empty for external edges); only some
/// languages consult it.
[[nodiscard]] std::optional<EdgeFidelity> reconstruct_edge_fidelity(std::string_view source_path,
                                                                    std::string_view kind,
                                                                    std::string_view import_string,
                                                                    std::string_view target_relpath,
                                                                    bool is_external);

/// Build the top-level `fidelity_metadata` block: a shared `caveat`, then a
/// per-language `languages` map. Each language carries its own version /
/// scope / method / corpus / expected_precision and its own `provisional`
/// flag (calibration rigor differs per language). Distribution-level
/// expected reliability for repos resembling each calibration corpus —
/// explicitly NOT a per-repo guarantee (see the `caveat` field).
[[nodiscard]] nlohmann::json build_fidelity_metadata_json();

} // namespace vectis::code
