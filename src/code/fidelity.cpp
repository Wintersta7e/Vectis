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

nlohmann::json build_fidelity_metadata_json()
{
    nlohmann::json meta;
    meta["version"] = std::string{k_python_fidelity_version};
    meta["method"] = "per-strategy empirical precision vs manual ground truth (offline)";
    meta["scope"] = "python-import-edges";
    meta["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 2;
    corpus["labeled_edges"] = 112;
    meta["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_relative_module}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_relative_package}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_dotted_module}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_dotted_package}] = k_py_resolved_confidence;
    expected[std::string{k_strategy_external_relative}] = k_py_external_relative_confidence;
    expected[std::string{k_strategy_external_dotted}] = k_py_external_dotted_confidence;
    meta["expected_precision"] = std::move(expected);

    meta["caveat"] = "distribution-level expected reliability for repos resembling the "
                     "calibration corpus; NOT a per-repo guarantee";
    return meta;
}

} // namespace vectis::code
