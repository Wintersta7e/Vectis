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
inline constexpr std::string_view k_python_fidelity_version = "py-import-2026-06-15";

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

/// Version pin for the Rust calibration table below.
inline constexpr std::string_view k_rust_fidelity_version = "rust-import-2026-06-15";

// --- Calibration table (Rust use/mod edges) ----------------------------------
//
// In-crate `use` (crate::/self::/super::) and `mod` re-measured 2026-06-03
// against 3 real-world Rust projects (449 .rs files, 1,164 use/mod edges) with
// an independent mod-graph oracle (a separate Python reimplementation, not
// Vectis) plus a hand-audited resolved-edge sample. A resolved in-crate `use`
// means a declared module file was found (trustworthy by construction); a
// non-match stays low (in-crate but unscanned: macros, cfg-gated mods, #[path],
// excluded dirs). Recalibrated 2026-06-13 over an 11-crate / ~7.3k-edge corpus
// (independent mod-graph oracle, numbers reproduced off the measuring box).
// Resolved workspace-sibling use paths are split out with package-name
// resolution.
inline constexpr double k_rust_mod_confidence =
    0.98; // 1074/1079 target-correct (Wilson LB 0.989, 11 crates)
inline constexpr double k_rust_mod_unresolved_confidence =
    0.90; // dir-module (`x/mod.rs`) miss now 0% on re-measure (n=15)
inline constexpr double k_rust_use_std_confidence =
    0.99; // 2246/2246 external, 43-repo corpus 2026-06-15
// `use crate::/self::/super::` resolves against the module graph: resolved =
// a declared module file was found; unresolved = in-crate but unscanned.
// 1174/1232 target-correct over 11 crates (Wilson LB 0.940) confirms 0.93;
// held below the LB for cfg/#[path]/macro blind spots + item-past-module.
inline constexpr double k_rust_use_internal_resolved_confidence = 0.93;
inline constexpr double k_rust_use_internal_unresolved_confidence =
    0.48; // 322/613 correctly-unresolved, 43-repo corpus 2026-06-15
inline constexpr double k_rust_use_sibling_resolved_confidence =
    0.91; // 176/184 target-correct (Wilson LB 0.917), 43-repo corpus 2026-06-15
inline constexpr double k_rust_use_extern_confidence =
    0.67; // 1335/1913 post-sibling-resolution (Wilson LB 0.677), 43-repo corpus 2026-06-15

/// Version pin for the C/C++ calibration table below.
inline constexpr std::string_view k_c_cpp_fidelity_version = "c-cpp-include-2026-06-15";

// --- Calibration table (C/C++ #include edges) --------------------------------
//
// Measured 2026-06-01 against 4 projects (2 C, 2 C++; 4,916 edges) with a
// mechanical filesystem oracle (full census). Only quoted `#include "..."`
// is captured (angle/system includes are invisible), so every edge is a
// quote-include. Strata split on whether the include string carries a
// directory part and whether it resolved. Resolved edges were 100%
// correct; external-bare is the weak case (50% are missed in-tree headers
// reached via a compiler -I path Vectis can't see), so it is published low.
inline constexpr double k_cinclude_resolved_path_confidence = 0.97; // 1186/1186 correct
inline constexpr double k_cinclude_resolved_bare_confidence =
    0.99; // 4390/4390, 43-repo corpus 2026-06-15
inline constexpr double k_cinclude_external_path_confidence =
    0.98; // 210/210, 43-repo corpus 2026-06-15
inline constexpr double k_cinclude_external_bare_confidence =
    0.94; // 177/181, 101-repo corpus 2026-06-15 (prior 0.68 was a noisy n=28 sample)

/// Version pin for the JavaScript/TypeScript calibration table below.
inline constexpr std::string_view k_jsts_fidelity_version = "jsts-import-2026-06-15";

// --- Calibration table (JavaScript/TypeScript import/require edges) -----------
//
// Measured 2026-06-01 against 8 projects (1 JS, 7 TS; ~12k edges) with a
// Node/TS-resolution + tsconfig-paths oracle. Only relative imports ever
// resolve; bare and path-alias specifiers are never resolved. The headline
// is `jsts-alias-unresolved`: `@/`, `~`, `#`-rooted specifiers are tsconfig
// path aliases that almost always DO resolve in-tree, so an "external"
// verdict there is near-certainly wrong — published at 0.05 as a
// false-external *detector*, not a trust signal.
inline constexpr double k_jsts_relative_resolved_confidence = 0.97; // 5128/5128 correct
inline constexpr double k_jsts_relative_unresolved_confidence =
    0.98; // 425/426, 43-repo corpus 2026-06-15
inline constexpr double k_jsts_alias_unresolved_confidence = 0.05; // 40/40 actually resolve in-tree
inline constexpr double k_jsts_bare_external_confidence =
    0.99; // 4021/4021, 43-repo corpus 2026-06-15

/// Version pin for the Java calibration table below.
inline constexpr std::string_view k_java_fidelity_version = "java-import-2026-06-15";

// --- Calibration table (Java import edges) -----------------------------------
//
// Measured 2026-06-01 against 3 projects (~3.9k edges) with a source-parsed
// FQCN/package oracle + Maven/Gradle dep check. Resolved imports are a
// specific class (last dotted segment is Uppercase) or a bare package
// resolved via the namespace index (last segment lowercase — the wildcard
// case, since the trailing `.*` is dropped at parse). Externals split into
// JDK (`java.`/`javax.`), plain third-party, and inner-type/static imports
// (`Outer.Inner`, second-to-last segment Uppercase) — the last is the one
// genuinely low-precision case (25% are in-project types the FQCN→file
// lookup misses).
inline constexpr double k_java_dotted_resolved_confidence = 0.95;     // 1176/1176 correct
inline constexpr double k_java_wildcard_resolved_confidence = 0.90;   // 33/33; thin, 1 project
inline constexpr double k_java_external_jdk_confidence = 0.97;        // 0/1215 false-external
inline constexpr double k_java_external_thirdparty_confidence = 0.96; // 0/1129 false-external
inline constexpr double k_java_external_innertype_confidence =
    0.40; // 8/40 hand-labeled: in-tree static-import classes left external; weak + corpus-dependent

/// Version pin for the C# calibration table below.
inline constexpr std::string_view k_csharp_fidelity_version = "csharp-using-2026-06-15";

// --- Calibration table (C# using edges) --------------------------------------
//
// Measured 2026-06-01 against 6 projects (~191k edges) with a source-parsed
// namespace oracle. A `using` resolves by exact-namespace match against the
// in-project namespace index (internal) or is external; externals split on
// whether the first dotted segment is a framework root (System/Microsoft/…).
// Internal was 100% precise; the third-party stratum carries a 13.7%
// false-external rate (type-level usings — `using Some.Ns.TypeName;` — that
// the namespace-keyed index can't resolve), so it is published lowest. Only
// plain usings are captured (`using static` / aliased `using X =` are not).
inline constexpr double k_cs_internal_confidence = 0.97;        // 173868/173868 correct
inline constexpr double k_cs_external_system_confidence = 0.95; // 2.1% false-external
inline constexpr double k_cs_external_thirdparty_confidence =
    0.90; // 40/40 hand-labeled sample 2026-06-15

/// Version pin for the PHP calibration table below.
inline constexpr std::string_view k_php_fidelity_version = "php-import-2026-06-15";

// --- Calibration table (PHP require/include/use edges) -----------------------
//
// Measured 2026-06-01 against 3 projects (~48k edges) with an independent
// FQCN/path oracle. require/include are path-based; `use` is namespace-based
// and resolves either by an exact PSR-4 path match (psr4-exact) or via a
// namespace-index fallback that over-approximates (fanout). The fanout
// stratum has a per-edge precision of only ~0.03–0.09 — published at 0.30 as
// a strong de-weight signal (the right file is usually in the fanned-out set,
// but most siblings are wrong). External `use`s split on whether the symbol
// is namespaced (`\`) or a root/global name.
inline constexpr double k_php_require_resolved_confidence = 0.95;   // 33/33 correct
inline constexpr double k_php_require_external_confidence = 0.90;   // recall-side
inline constexpr double k_php_use_psr4_confidence = 0.97;           // 8051/8051 correct
inline constexpr double k_php_use_nsindex_fanout_confidence = 0.30; // over-approximation
inline constexpr double k_php_use_external_global_confidence =
    0.99; // 2276/2276, 43-repo corpus 2026-06-15
inline constexpr double k_php_use_external_namespaced_confidence =
    0.99; // 2944/2944, 43-repo corpus 2026-06-15

/// Version pin for the Ruby calibration table below.
inline constexpr std::string_view k_ruby_fidelity_version = "ruby-import-2026-06-15";

// --- Calibration table (Ruby require edges) ----------------------------------
//
// Measured 2026-06-01 against 3 projects (467 edges) with a filesystem
// oracle + full-population recheck. `require` and `require_relative` both
// emit kind `require`, and idiomatic args are bare (no `./`), so strata key
// on resolution + path shape. The one systematic defect is stdlib-shadow:
// a single-segment `require 'json'` can resolve to a coincidental in-tree
// `json.rb` via the suffix-first fallback, so resolved-single is published
// below resolved-multi.
inline constexpr double k_ruby_relative_explicit_confidence = 0.90; // n=1; structural prior
inline constexpr double k_ruby_resolved_multi_confidence =
    0.99; // 863/863, 43-repo corpus 2026-06-15
inline constexpr double k_ruby_resolved_single_confidence =
    0.93; // 103/105, 43-repo corpus 2026-06-15
inline constexpr double k_ruby_external_stdlib_confidence =
    0.96;                                                      // 101/101, 43-repo corpus 2026-06-15
inline constexpr double k_ruby_external_gem_confidence = 0.96; // 97/97, 43-repo corpus 2026-06-15

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

/// Reconstruct the resolution strategy for one Rust `use`/`mod` edge.
/// `kind` selects the mechanism: `mod` → `rust-mod` (resolved) /
/// `rust-mod-unresolved`; `use` → `rust-use-std` (first `::` segment is
/// std/core/alloc), `rust-use-internal-resolved` or
/// `rust-use-internal-unresolved` (crate/self/super/Self — split on
/// `is_external`: resolved when the module graph matched a file, unresolved
/// for residual gaps from macros / cfg-gated mods / #[path]), else resolved
/// sibling-crate paths use `rust-use-sibling-resolved` and unresolved bare paths
/// use `rust-use-extern`.
[[nodiscard]] std::string reconstruct_rust_resolved_by(std::string_view kind,
                                                       std::string_view import_string,
                                                       bool is_external);

/// Calibrated precision for a `reconstruct_rust_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double rust_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one C/C++ `#include` edge:
/// `cinclude-{resolved|external}-{path|bare}`, where `path` means the
/// include string carries a directory part (`a/b.h`) and `bare` a plain
/// basename (`b.h`). Only quoted includes are captured.
[[nodiscard]] std::string reconstruct_c_cpp_resolved_by(std::string_view import_string,
                                                        bool is_external);

/// Calibrated precision for a `reconstruct_c_cpp_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double c_cpp_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one JS/TS `import`/`require`
/// edge: `jsts-relative-resolved` (resolved); else `jsts-relative-unresolved`
/// (specifier starts `./`/`../`), `jsts-alias-unresolved` (tsconfig path
/// alias root `@/`, `~`, `#`), else `jsts-bare-external` (an npm specifier).
[[nodiscard]] std::string reconstruct_jsts_resolved_by(std::string_view import_string,
                                                       bool is_external);

/// Calibrated precision for a `reconstruct_jsts_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double jsts_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one Java `import` edge. Resolved:
/// `java-wildcard-resolved` if the last dotted segment is lowercase (a bare
/// package), else `java-dotted-resolved` (a specific class). External:
/// `java-external-jdk` (`java.`/`javax.`); else `java-external-innertype`
/// (an `Outer.Inner`/static import — second-to-last segment Uppercase),
/// else `java-external-thirdparty`.
[[nodiscard]] std::string reconstruct_java_resolved_by(std::string_view import_string,
                                                       bool is_external);

/// Calibrated precision for a `reconstruct_java_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double java_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one C# `using` edge:
/// `csharp-internal` (resolved); else `csharp-external-system` if the first
/// dotted segment is a framework root (System/Microsoft/Windows/Mono/Internal),
/// else `csharp-external-thirdparty`.
[[nodiscard]] std::string reconstruct_csharp_resolved_by(std::string_view import_string,
                                                         bool is_external);

/// Calibrated precision for a `reconstruct_csharp_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double csharp_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one PHP edge. `kind` is
/// `require`/`include` (path-based) or `use` (namespace). require/include:
/// `php-require-resolved` / `php-require-external`. use, resolved:
/// `php-use-psr4-exact` if `target_relpath` ends with the PSR-4 path of the
/// namespace, else `php-use-nsindex-fanout`. use, external:
/// `php-use-external-namespaced` if the symbol contains `\`, else
/// `php-use-external-global`.
[[nodiscard]] std::string reconstruct_php_resolved_by(std::string_view import_string,
                                                      std::string_view target_relpath,
                                                      std::string_view kind, bool is_external);

/// Calibrated precision for a `reconstruct_php_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double php_edge_confidence(std::string_view strategy);

/// Reconstruct the resolution strategy for one Ruby `require` edge.
/// Resolved: `ruby-relative-explicit` (starts `./`/`../`), else
/// `ruby-resolved-multi` (the require has a `/`), else `ruby-resolved-single`.
/// External: `ruby-external-stdlib` if the first path segment is a known
/// stdlib/default-gem name, else `ruby-external-gem`.
[[nodiscard]] std::string reconstruct_ruby_resolved_by(std::string_view import_string,
                                                       bool is_external);

/// Calibrated precision for a `reconstruct_ruby_resolved_by` strategy.
/// Unknown strategies return 0.0 (fail closed).
[[nodiscard]] double ruby_edge_confidence(std::string_view strategy);

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
