#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace vectis::code {

/// Per-edge resolution-fidelity model for Python `import` edges.
///
/// Vectis's resolver records *whether* a Python import resolved to a
/// file inside the project, but not *how confident* that resolution is.
/// This module reconstructs the resolution strategy from the edge's
/// existing data (import string + resolved target path) — mirroring the
/// offline Stage-1 ground-truth harness — and attaches a calibrated
/// per-strategy precision so a consuming agent can weight edges instead
/// of treating every resolved/unresolved edge as equally trustworthy.
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

/// Build the top-level `fidelity_metadata` block. Distribution-level
/// expected reliability for repos resembling the calibration corpus —
/// explicitly NOT a per-repo guarantee (see the `caveat` field).
[[nodiscard]] nlohmann::json build_fidelity_metadata_json();

} // namespace vectis::code
