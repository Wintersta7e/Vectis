#include "code/fidelity.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace vectis::code {

namespace {

// Strategy taxonomy. Shared between reconstruction and the confidence
// lookup so the two can never drift apart on a literal typo.
constexpr std::string_view k_strategy_relative_module = "relative-module";
constexpr std::string_view k_strategy_relative_package = "relative-package";
constexpr std::string_view k_strategy_dotted_module = "dotted-module";
constexpr std::string_view k_strategy_dotted_package = "dotted-package";
constexpr std::string_view k_strategy_external_relative = "external-relative";
constexpr std::string_view k_strategy_external_dotted = "external-dotted";

// Go strata: internal (resolved via go.mod prefix) vs external split into
// standard-library and third-party by the first import path segment.
constexpr std::string_view k_strategy_go_internal = "go-internal";
constexpr std::string_view k_strategy_go_external_stdlib = "go-external-stdlib";
constexpr std::string_view k_strategy_go_external_thirdparty = "go-external-thirdparty";

// Rust strata: `mod` (path-resolved) vs `use` (never resolved today; split
// by the first `::` segment into std, internal crate paths, and externs).
constexpr std::string_view k_strategy_rust_mod = "rust-mod";
constexpr std::string_view k_strategy_rust_mod_unresolved = "rust-mod-unresolved";
constexpr std::string_view k_strategy_rust_use_std = "rust-use-std";
constexpr std::string_view k_strategy_rust_use_internal = "rust-use-internal";
constexpr std::string_view k_strategy_rust_use_extern = "rust-use-extern";

// C/C++ #include strata: resolved/external × path/bare (directory part in the
// include string or not).
constexpr std::string_view k_strategy_cinclude_resolved_path = "cinclude-resolved-path";
constexpr std::string_view k_strategy_cinclude_resolved_bare = "cinclude-resolved-bare";
constexpr std::string_view k_strategy_cinclude_external_path = "cinclude-external-path";
constexpr std::string_view k_strategy_cinclude_external_bare = "cinclude-external-bare";

// Source-file extension sets for languages whose edge `kind` is shared with
// other languages (so dispatch must gate on extension).
constexpr std::array<std::string_view, 10> k_c_cpp_exts = {".c",   ".h",  ".cc",  ".cpp", ".cxx",
                                                           ".hpp", ".hh", ".hxx", ".inl", ".ipp"};

/// True if `path` names a Python package init file (`__init__.py`),
/// which distinguishes a package-resolved import from a module one.
[[nodiscard]] bool ends_with_init_py(std::string_view path)
{
    constexpr std::string_view k_init = "__init__.py";
    return path.size() >= k_init.size() && path.substr(path.size() - k_init.size()) == k_init;
}

/// True if `path` ends with the source-file extension `ext`. Used by
/// `reconstruct_edge_fidelity` to dispatch by language.
[[nodiscard]] bool ends_with(std::string_view path, std::string_view ext)
{
    return path.size() >= ext.size() && path.substr(path.size() - ext.size()) == ext;
}

/// True if `path` ends with any extension in `exts` (for languages spanning
/// several source extensions, e.g. C/C++).
[[nodiscard]] bool ends_with_any(std::string_view path, std::span<const std::string_view> exts)
{
    return std::ranges::any_of(exts, [path](std::string_view e) { return ends_with(path, e); });
}

} // namespace

std::string reconstruct_python_resolved_by(std::string_view import_string,
                                           std::string_view target_relpath, bool is_external)
{
    const bool relative = !import_string.empty() && import_string.front() == '.';

    if (is_external) {
        return std::string{relative ? k_strategy_external_relative : k_strategy_external_dotted};
    }

    const bool package = ends_with_init_py(target_relpath);
    if (relative) {
        return std::string{package ? k_strategy_relative_package : k_strategy_relative_module};
    }
    return std::string{package ? k_strategy_dotted_package : k_strategy_dotted_module};
}

double python_edge_confidence(std::string_view strategy)
{
    // Resolved strategies share one conservative figure: all four were
    // observed at ~1.0 precision (72/72 correct in calibration), published
    // at 0.95.
    if (strategy == k_strategy_relative_module || strategy == k_strategy_relative_package ||
        strategy == k_strategy_dotted_module || strategy == k_strategy_dotted_package) {
        return k_py_resolved_confidence;
    }
    if (strategy == k_strategy_external_relative) {
        return k_py_external_relative_confidence;
    }
    if (strategy == k_strategy_external_dotted) {
        return k_py_external_dotted_confidence;
    }
    // Unknown strategy: fail closed rather than inherit a neighbour's
    // number. A new taxonomy entry must be calibrated explicitly.
    return 0.0;
}

std::string reconstruct_go_resolved_by(std::string_view import_string, bool is_external)
{
    if (!is_external) {
        return std::string{k_strategy_go_internal};
    }
    // External: standard-library iff the first path segment carries no '.';
    // third-party otherwise (a domain-prefixed module path, github.com/...).
    const std::string_view first = import_string.substr(0, import_string.find('/'));
    const bool thirdparty = first.find('.') != std::string_view::npos;
    return std::string{thirdparty ? k_strategy_go_external_thirdparty
                                  : k_strategy_go_external_stdlib};
}

double go_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_go_internal) {
        return k_go_internal_confidence;
    }
    if (strategy == k_strategy_go_external_stdlib) {
        return k_go_external_stdlib_confidence;
    }
    if (strategy == k_strategy_go_external_thirdparty) {
        return k_go_external_thirdparty_confidence;
    }
    // Unknown strategy: fail closed rather than inherit a neighbour's number,
    // mirroring python_edge_confidence.
    return 0.0;
}

std::string reconstruct_rust_resolved_by(std::string_view kind, std::string_view import_string,
                                         bool is_external)
{
    if (kind == "mod") {
        return std::string{is_external ? k_strategy_rust_mod_unresolved : k_strategy_rust_mod};
    }
    // kind == "use": classify by the first `::`-separated path segment.
    const std::string_view first = import_string.substr(0, import_string.find("::"));
    if (first == "std" || first == "core" || first == "alloc") {
        return std::string{k_strategy_rust_use_std};
    }
    if (first == "crate" || first == "self" || first == "super" || first == "Self") {
        return std::string{k_strategy_rust_use_internal};
    }
    return std::string{k_strategy_rust_use_extern};
}

double rust_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_rust_mod) {
        return k_rust_mod_confidence;
    }
    if (strategy == k_strategy_rust_mod_unresolved) {
        return k_rust_mod_unresolved_confidence;
    }
    if (strategy == k_strategy_rust_use_std) {
        return k_rust_use_std_confidence;
    }
    if (strategy == k_strategy_rust_use_internal) {
        return k_rust_use_internal_confidence;
    }
    if (strategy == k_strategy_rust_use_extern) {
        return k_rust_use_extern_confidence;
    }
    return 0.0; // fail closed
}

std::string reconstruct_c_cpp_resolved_by(std::string_view import_string, bool is_external)
{
    // `path` iff the include string carries a directory part; the include
    // string is the same field whether resolved (import_ref) or external.
    const bool has_dir = import_string.find('/') != std::string_view::npos;
    if (is_external) {
        return std::string{has_dir ? k_strategy_cinclude_external_path
                                   : k_strategy_cinclude_external_bare};
    }
    return std::string{has_dir ? k_strategy_cinclude_resolved_path
                               : k_strategy_cinclude_resolved_bare};
}

double c_cpp_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_cinclude_resolved_path) {
        return k_cinclude_resolved_path_confidence;
    }
    if (strategy == k_strategy_cinclude_resolved_bare) {
        return k_cinclude_resolved_bare_confidence;
    }
    if (strategy == k_strategy_cinclude_external_path) {
        return k_cinclude_external_path_confidence;
    }
    if (strategy == k_strategy_cinclude_external_bare) {
        return k_cinclude_external_bare_confidence;
    }
    return 0.0; // fail closed
}

std::optional<EdgeFidelity> reconstruct_edge_fidelity(std::string_view source_path,
                                                      std::string_view kind,
                                                      std::string_view import_string,
                                                      std::string_view target_relpath,
                                                      bool is_external)
{
    // Dispatch on (source extension, edge kind). Each branch reconstructs a
    // language-specific strategy string and pairs it with that language's
    // calibrated confidence. Uncalibrated combinations return nullopt so the
    // exporter leaves the edge untouched. `include`/`require`/`use` kinds are
    // shared across languages, hence the extension gate.
    if (kind == "import" && ends_with(source_path, ".py")) {
        const std::string strategy =
            reconstruct_python_resolved_by(import_string, target_relpath, is_external);
        return EdgeFidelity{strategy, python_edge_confidence(strategy)};
    }
    if (kind == "import" && ends_with(source_path, ".go")) {
        const std::string strategy = reconstruct_go_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, go_edge_confidence(strategy)};
    }
    if ((kind == "use" || kind == "mod") && ends_with(source_path, ".rs")) {
        const std::string strategy = reconstruct_rust_resolved_by(kind, import_string, is_external);
        return EdgeFidelity{strategy, rust_edge_confidence(strategy)};
    }
    if (kind == "include" && ends_with_any(source_path, k_c_cpp_exts)) {
        const std::string strategy = reconstruct_c_cpp_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, c_cpp_edge_confidence(strategy)};
    }
    return std::nullopt;
}

namespace {

/// Per-language fidelity sub-block for Python import edges.
[[nodiscard]] nlohmann::json build_python_fidelity_json()
{
    nlohmann::json py;
    py["version"] = std::string{k_python_fidelity_version};
    py["scope"] = "python-import-edges";
    py["method"] = "per-strategy empirical precision vs manual ground truth (offline)";
    // Still provisional: the original labels were manual (2 projects), and the
    // `external-relative` stratum has no real-world coverage yet to confirm its
    // figure. A wider re-measure corroborated the numbers but with a weaker
    // (internal-consistency) oracle — not enough to de-provisionalize.
    py["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 2;
    corpus["labeled_edges"] = 112;
    py["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_relative_module}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_relative_package}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_dotted_module}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_dotted_package}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_external_relative}] = k_py_external_relative_confidence;
    expected[std::string{k_strategy_external_dotted}] = k_py_external_dotted_confidence;
    py["expected_precision"] = std::move(expected);
    return py;
}

/// Per-language fidelity sub-block for Go import edges.
[[nodiscard]] nlohmann::json build_go_fidelity_json()
{
    nlohmann::json go;
    go["version"] = std::string{k_go_fidelity_version};
    go["scope"] = "go-import-edges";
    go["method"] = "per-strategy precision vs go.mod spec oracle (offline), "
                   "validated by full-population recheck on an 11-project corpus";
    // Non-provisional: Go's oracle is fully mechanical (go.mod module-prefix +
    // on-disk package existence) and a widened 11-project corpus held at
    // 1.0 internal precision / 0 false-externals across both a stratified
    // sample and a full-population recheck (thousands of edges).
    go["provisional"] = false;

    nlohmann::json corpus;
    corpus["projects"] = 11;
    corpus["labeled_edges"] = 90;
    go["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_go_internal}] = k_go_internal_confidence;
    expected[std::string{k_strategy_go_external_stdlib}] = k_go_external_stdlib_confidence;
    expected[std::string{k_strategy_go_external_thirdparty}] = k_go_external_thirdparty_confidence;
    go["expected_precision"] = std::move(expected);
    return go;
}

/// Per-language fidelity sub-block for Rust use/mod edges.
[[nodiscard]] nlohmann::json build_rust_fidelity_json()
{
    nlohmann::json rust;
    rust["version"] = std::string{k_rust_fidelity_version};
    rust["scope"] = "rust-use-mod-edges";
    rust["method"] = "per-stratum precision / false-external rate vs Cargo-manifest + "
                     "in-tree module oracle (offline)";
    rust["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 3715;
    rust["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_rust_mod}] = k_rust_mod_confidence;
    expected[std::string{k_strategy_rust_mod_unresolved}] = k_rust_mod_unresolved_confidence;
    expected[std::string{k_strategy_rust_use_std}] = k_rust_use_std_confidence;
    expected[std::string{k_strategy_rust_use_internal}] = k_rust_use_internal_confidence;
    expected[std::string{k_strategy_rust_use_extern}] = k_rust_use_extern_confidence;
    rust["expected_precision"] = std::move(expected);
    return rust;
}

/// Per-language fidelity sub-block for C/C++ #include edges.
[[nodiscard]] nlohmann::json build_c_cpp_fidelity_json()
{
    nlohmann::json cpp;
    cpp["version"] = std::string{k_c_cpp_fidelity_version};
    cpp["scope"] = "c-cpp-include-edges";
    cpp["method"] = "per-stratum precision / false-external rate vs mechanical filesystem "
                    "oracle, full census (offline); only quoted #include is captured";
    cpp["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 4;
    corpus["labeled_edges"] = 4916;
    cpp["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_cinclude_resolved_path}] = k_cinclude_resolved_path_confidence;
    expected[std::string{k_strategy_cinclude_resolved_bare}] = k_cinclude_resolved_bare_confidence;
    expected[std::string{k_strategy_cinclude_external_path}] = k_cinclude_external_path_confidence;
    expected[std::string{k_strategy_cinclude_external_bare}] = k_cinclude_external_bare_confidence;
    cpp["expected_precision"] = std::move(expected);
    return cpp;
}

} // namespace

nlohmann::json build_fidelity_metadata_json()
{
    // Only the disclaimer is shared across languages. Per-language version /
    // scope / method / corpus / provisional live under `languages` so each can
    // be recalibrated — and de-provisionalized — independently.
    nlohmann::json meta;
    meta["caveat"] = "distribution-level expected reliability for repos resembling the "
                     "calibration corpus; NOT a per-repo guarantee";

    nlohmann::json languages;
    languages["python"] = build_python_fidelity_json();
    languages["go"] = build_go_fidelity_json();
    languages["rust"] = build_rust_fidelity_json();
    languages["c_cpp"] = build_c_cpp_fidelity_json();
    meta["languages"] = std::move(languages);
    return meta;
}

} // namespace vectis::code
