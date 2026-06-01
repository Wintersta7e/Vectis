#include "code/fidelity.h"

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

/// True if `path` names a Python package init file (`__init__.py`),
/// which distinguishes a package-resolved import from a module one.
[[nodiscard]] bool ends_with_init_py(std::string_view path)
{
    constexpr std::string_view k_init = "__init__.py";
    return path.size() >= k_init.size() && path.substr(path.size() - k_init.size()) == k_init;
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

namespace {

/// Per-language fidelity sub-block for Python import edges.
[[nodiscard]] nlohmann::json build_python_fidelity_json()
{
    nlohmann::json py;
    py["version"] = std::string{k_python_fidelity_version};
    py["scope"] = "python-import-edges";
    py["method"] = "per-strategy empirical precision vs manual ground truth (offline)";

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
    go["method"] = "per-strategy precision vs go.mod spec oracle + anomaly review (offline)";

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 90;
    go["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_go_internal}] = k_go_internal_confidence;
    expected[std::string{k_strategy_go_external_stdlib}] = k_go_external_stdlib_confidence;
    expected[std::string{k_strategy_go_external_thirdparty}] = k_go_external_thirdparty_confidence;
    go["expected_precision"] = std::move(expected);
    return go;
}

} // namespace

nlohmann::json build_fidelity_metadata_json()
{
    // Shared across languages: the small-corpus disclaimer applies to every
    // stratum. Per-language version / scope / method / corpus live under
    // `languages` so each can be recalibrated independently.
    nlohmann::json meta;
    meta["provisional"] = true;
    meta["caveat"] = "distribution-level expected reliability for repos resembling the "
                     "calibration corpus; NOT a per-repo guarantee";

    nlohmann::json languages;
    languages["python"] = build_python_fidelity_json();
    languages["go"] = build_go_fidelity_json();
    meta["languages"] = std::move(languages);
    return meta;
}

} // namespace vectis::code
