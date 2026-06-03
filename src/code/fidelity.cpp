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

// Rust strata: `mod` (path-resolved) vs `use` (split by first `::` segment
// into std, internal crate paths, and externs). crate/self/super `use`
// paths now resolve via the module graph; internal splits on resolved status.
constexpr std::string_view k_strategy_rust_mod = "rust-mod";
constexpr std::string_view k_strategy_rust_mod_unresolved = "rust-mod-unresolved";
constexpr std::string_view k_strategy_rust_use_std = "rust-use-std";
constexpr std::string_view k_strategy_rust_use_internal_resolved = "rust-use-internal-resolved";
constexpr std::string_view k_strategy_rust_use_internal_unresolved = "rust-use-internal-unresolved";
constexpr std::string_view k_strategy_rust_use_extern = "rust-use-extern";

// C/C++ #include strata: resolved/external × path/bare (directory part in the
// include string or not).
constexpr std::string_view k_strategy_cinclude_resolved_path = "cinclude-resolved-path";
constexpr std::string_view k_strategy_cinclude_resolved_bare = "cinclude-resolved-bare";
constexpr std::string_view k_strategy_cinclude_external_path = "cinclude-external-path";
constexpr std::string_view k_strategy_cinclude_external_bare = "cinclude-external-bare";

// JS/TS strata: only relative imports resolve; bare/alias never do. Alias
// (`@/`, `~`, `#`) externals almost always resolve in-tree — a detector.
constexpr std::string_view k_strategy_jsts_relative_resolved = "jsts-relative-resolved";
constexpr std::string_view k_strategy_jsts_relative_unresolved = "jsts-relative-unresolved";
constexpr std::string_view k_strategy_jsts_alias_unresolved = "jsts-alias-unresolved";
constexpr std::string_view k_strategy_jsts_bare_external = "jsts-bare-external";

// Java strata: resolved class vs bare-package (wildcard); external split into
// JDK, plain third-party, and inner-type/static imports.
constexpr std::string_view k_strategy_java_dotted_resolved = "java-dotted-resolved";
constexpr std::string_view k_strategy_java_wildcard_resolved = "java-wildcard-resolved";
constexpr std::string_view k_strategy_java_external_jdk = "java-external-jdk";
constexpr std::string_view k_strategy_java_external_thirdparty = "java-external-thirdparty";
constexpr std::string_view k_strategy_java_external_innertype = "java-external-innertype";

// C# strata: internal (exact namespace-index match) vs external split by a
// framework-root first segment.
constexpr std::string_view k_strategy_cs_internal = "csharp-internal";
constexpr std::string_view k_strategy_cs_external_system = "csharp-external-system";
constexpr std::string_view k_strategy_cs_external_thirdparty = "csharp-external-thirdparty";

// PHP strata: require/include (path) vs use (namespace). Resolved `use`
// splits into an exact PSR-4 match vs a lossy namespace-index fanout.
constexpr std::string_view k_strategy_php_require_resolved = "php-require-resolved";
constexpr std::string_view k_strategy_php_require_external = "php-require-external";
constexpr std::string_view k_strategy_php_use_psr4 = "php-use-psr4-exact";
constexpr std::string_view k_strategy_php_use_nsindex_fanout = "php-use-nsindex-fanout";
constexpr std::string_view k_strategy_php_use_external_global = "php-use-external-global";
constexpr std::string_view k_strategy_php_use_external_namespaced = "php-use-external-namespaced";

// Ruby strata: resolved (explicit-relative / multi-segment / single-segment)
// vs external (stdlib name vs gem).
constexpr std::string_view k_strategy_ruby_relative_explicit = "ruby-relative-explicit";
constexpr std::string_view k_strategy_ruby_resolved_multi = "ruby-resolved-multi";
constexpr std::string_view k_strategy_ruby_resolved_single = "ruby-resolved-single";
constexpr std::string_view k_strategy_ruby_external_stdlib = "ruby-external-stdlib";
constexpr std::string_view k_strategy_ruby_external_gem = "ruby-external-gem";

// First-path-segment names of the Ruby standard library + default gems. A
// require whose first segment is in this set is treated as a genuine stdlib
// import rather than an unscanned in-tree file. Representative, not exhaustive.
constexpr std::array<std::string_view, 72> k_ruby_stdlib = {
    "abbrev",       "base64",      "benchmark",  "bigdecimal",   "cgi",      "coverage",
    "csv",          "date",        "delegate",   "did_you_mean", "digest",   "drb",
    "english",      "erb",         "etc",        "fcntl",        "fiddle",   "fileutils",
    "find",         "forwardable", "gdbm",       "getoptlong",   "io",       "ipaddr",
    "irb",          "json",        "logger",     "matrix",       "mkmf",     "monitor",
    "mutex_m",      "net",         "nkf",        "objspace",     "observer", "open3",
    "openssl",      "optparse",    "ostruct",    "pathname",     "pp",       "prettyprint",
    "prime",        "pstore",      "psych",      "rake",         "rbconfig", "rdoc",
    "readline",     "reline",      "resolv",     "rinda",        "ripper",   "rss",
    "securerandom", "set",         "shellwords", "singleton",    "socket",   "stringio",
    "strscan",      "syslog",      "tempfile",   "time",         "timeout",  "tmpdir",
    "tsort",        "uri",         "weakref",    "yaml",         "zlib",     "fiber"};

// Source-file extension sets for languages whose edge `kind` is shared with
// other languages (so dispatch must gate on extension).
constexpr std::array<std::string_view, 10> k_c_cpp_exts = {".c",   ".h",  ".cc",  ".cpp", ".cxx",
                                                           ".hpp", ".hh", ".hxx", ".inl", ".ipp"};
constexpr std::array<std::string_view, 6> k_jsts_exts = {".ts",  ".tsx", ".js",
                                                         ".jsx", ".mjs", ".cjs"};
// .NET framework namespace roots: a first segment in this set marks a
// standard-library/framework using rather than a third-party one.
constexpr std::array<std::string_view, 5> k_csharp_framework_roots = {
    "System", "Microsoft", "Windows", "Mono", "Internal"};

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

/// True if `s` starts with an ASCII uppercase letter. Java convention:
/// type names are PascalCase, package segments lowercase.
[[nodiscard]] bool starts_upper(std::string_view s)
{
    return !s.empty() && s.front() >= 'A' && s.front() <= 'Z';
}

/// The dotted segment immediately before the last (e.g. `Outer` in
/// `pkg.Outer.Inner`); empty if the string has fewer than two segments.
[[nodiscard]] std::string_view second_last_dotted_segment(std::string_view s)
{
    const auto last_dot = s.rfind('.');
    if (last_dot == std::string_view::npos || last_dot == 0) {
        return {};
    }
    const auto prev_dot = s.rfind('.', last_dot - 1);
    const std::size_t start = (prev_dot == std::string_view::npos) ? 0 : prev_dot + 1;
    return s.substr(start, last_dot - start);
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
        return std::string{is_external ? k_strategy_rust_use_internal_unresolved
                                       : k_strategy_rust_use_internal_resolved};
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
    if (strategy == k_strategy_rust_use_internal_resolved) {
        return k_rust_use_internal_resolved_confidence;
    }
    if (strategy == k_strategy_rust_use_internal_unresolved) {
        return k_rust_use_internal_unresolved_confidence;
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

std::string reconstruct_jsts_resolved_by(std::string_view import_string, bool is_external)
{
    if (!is_external) {
        return std::string{k_strategy_jsts_relative_resolved}; // every resolved edge is relative
    }
    if (import_string.starts_with("./") || import_string.starts_with("../")) {
        return std::string{k_strategy_jsts_relative_unresolved};
    }
    // tsconfig path-alias roots; `@/` is decisive (npm scopes are `@scope/pkg`).
    if (import_string.starts_with("@/") || import_string.starts_with("~") ||
        import_string.starts_with("#")) {
        return std::string{k_strategy_jsts_alias_unresolved};
    }
    return std::string{k_strategy_jsts_bare_external};
}

double jsts_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_jsts_relative_resolved) {
        return k_jsts_relative_resolved_confidence;
    }
    if (strategy == k_strategy_jsts_relative_unresolved) {
        return k_jsts_relative_unresolved_confidence;
    }
    if (strategy == k_strategy_jsts_alias_unresolved) {
        return k_jsts_alias_unresolved_confidence;
    }
    if (strategy == k_strategy_jsts_bare_external) {
        return k_jsts_bare_external_confidence;
    }
    return 0.0; // fail closed
}

std::string reconstruct_java_resolved_by(std::string_view import_string, bool is_external)
{
    if (!is_external) {
        // Last segment lowercase => a bare package (wildcard, resolved via the
        // namespace index); Uppercase => a specific class. The trailing `.*`
        // of a wildcard is dropped at parse, so this is the only signal.
        const auto last_dot = import_string.rfind('.');
        const std::string_view last =
            last_dot == std::string_view::npos ? import_string : import_string.substr(last_dot + 1);
        return std::string{starts_upper(last) ? k_strategy_java_dotted_resolved
                                              : k_strategy_java_wildcard_resolved};
    }
    const std::string_view first = import_string.substr(0, import_string.find('.'));
    if (first == "java" || first == "javax") {
        return std::string{k_strategy_java_external_jdk};
    }
    // Non-JDK: an inner-type or static import (`Outer.Inner`) has an Uppercase
    // second-to-last segment — the one genuinely low-precision external class.
    if (starts_upper(second_last_dotted_segment(import_string))) {
        return std::string{k_strategy_java_external_innertype};
    }
    return std::string{k_strategy_java_external_thirdparty};
}

double java_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_java_dotted_resolved) {
        return k_java_dotted_resolved_confidence;
    }
    if (strategy == k_strategy_java_wildcard_resolved) {
        return k_java_wildcard_resolved_confidence;
    }
    if (strategy == k_strategy_java_external_jdk) {
        return k_java_external_jdk_confidence;
    }
    if (strategy == k_strategy_java_external_thirdparty) {
        return k_java_external_thirdparty_confidence;
    }
    if (strategy == k_strategy_java_external_innertype) {
        return k_java_external_innertype_confidence;
    }
    return 0.0; // fail closed
}

std::string reconstruct_csharp_resolved_by(std::string_view import_string, bool is_external)
{
    if (!is_external) {
        return std::string{k_strategy_cs_internal};
    }
    const std::string_view first = import_string.substr(0, import_string.find('.'));
    const bool framework =
        std::ranges::find(k_csharp_framework_roots, first) != k_csharp_framework_roots.end();
    return std::string{framework ? k_strategy_cs_external_system
                                 : k_strategy_cs_external_thirdparty};
}

double csharp_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_cs_internal) {
        return k_cs_internal_confidence;
    }
    if (strategy == k_strategy_cs_external_system) {
        return k_cs_external_system_confidence;
    }
    if (strategy == k_strategy_cs_external_thirdparty) {
        return k_cs_external_thirdparty_confidence;
    }
    return 0.0; // fail closed
}

std::string reconstruct_php_resolved_by(std::string_view import_string,
                                        std::string_view target_relpath, std::string_view kind,
                                        bool is_external)
{
    if (kind != "use") {
        // require / include: path-based.
        return std::string{is_external ? k_strategy_php_require_external
                                       : k_strategy_php_require_resolved};
    }
    if (is_external) {
        const bool namespaced = import_string.find('\\') != std::string_view::npos;
        return std::string{namespaced ? k_strategy_php_use_external_namespaced
                                      : k_strategy_php_use_external_global};
    }
    // Resolved `use`: exact PSR-4 path match iff the target ends with the
    // namespace mapped to a path (`A\B\C` -> `A/B/C.php`), else the lossy
    // namespace-index fanout.
    std::string want;
    want.reserve(import_string.size() + 4);
    for (const char ch : import_string) {
        want.push_back(ch == '\\' ? '/' : ch);
    }
    want += ".php";
    return std::string{ends_with(target_relpath, want) ? k_strategy_php_use_psr4
                                                       : k_strategy_php_use_nsindex_fanout};
}

double php_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_php_require_resolved) {
        return k_php_require_resolved_confidence;
    }
    if (strategy == k_strategy_php_require_external) {
        return k_php_require_external_confidence;
    }
    if (strategy == k_strategy_php_use_psr4) {
        return k_php_use_psr4_confidence;
    }
    if (strategy == k_strategy_php_use_nsindex_fanout) {
        return k_php_use_nsindex_fanout_confidence;
    }
    if (strategy == k_strategy_php_use_external_global) {
        return k_php_use_external_global_confidence;
    }
    if (strategy == k_strategy_php_use_external_namespaced) {
        return k_php_use_external_namespaced_confidence;
    }
    return 0.0; // fail closed
}

std::string reconstruct_ruby_resolved_by(std::string_view import_string, bool is_external)
{
    if (!is_external) {
        if (import_string.starts_with("./") || import_string.starts_with("../")) {
            return std::string{k_strategy_ruby_relative_explicit};
        }
        // A multi-segment require (`sinatra/base`) is self-disambiguating;
        // a bare single segment (`set`) is the stdlib-shadow risk.
        return std::string{import_string.find('/') != std::string_view::npos
                               ? k_strategy_ruby_resolved_multi
                               : k_strategy_ruby_resolved_single};
    }
    // External: classify by the first path segment (after any `./`/`../`).
    std::string_view first = import_string;
    if (first.starts_with("./")) {
        first.remove_prefix(2);
    }
    else if (first.starts_with("../")) {
        first.remove_prefix(3);
    }
    first = first.substr(0, first.find('/'));
    const bool stdlib = std::ranges::find(k_ruby_stdlib, first) != k_ruby_stdlib.end();
    return std::string{stdlib ? k_strategy_ruby_external_stdlib : k_strategy_ruby_external_gem};
}

double ruby_edge_confidence(std::string_view strategy)
{
    if (strategy == k_strategy_ruby_relative_explicit) {
        return k_ruby_relative_explicit_confidence;
    }
    if (strategy == k_strategy_ruby_resolved_multi) {
        return k_ruby_resolved_multi_confidence;
    }
    if (strategy == k_strategy_ruby_resolved_single) {
        return k_ruby_resolved_single_confidence;
    }
    if (strategy == k_strategy_ruby_external_stdlib) {
        return k_ruby_external_stdlib_confidence;
    }
    if (strategy == k_strategy_ruby_external_gem) {
        return k_ruby_external_gem_confidence;
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
    if ((kind == "import" || kind == "require") && ends_with_any(source_path, k_jsts_exts)) {
        const std::string strategy = reconstruct_jsts_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, jsts_edge_confidence(strategy)};
    }
    if (kind == "import" && ends_with(source_path, ".java")) {
        const std::string strategy = reconstruct_java_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, java_edge_confidence(strategy)};
    }
    if (kind == "use" && ends_with(source_path, ".cs")) {
        const std::string strategy = reconstruct_csharp_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, csharp_edge_confidence(strategy)};
    }
    if ((kind == "require" || kind == "include" || kind == "use") &&
        ends_with(source_path, ".php")) {
        const std::string strategy =
            reconstruct_php_resolved_by(import_string, target_relpath, kind, is_external);
        return EdgeFidelity{strategy, php_edge_confidence(strategy)};
    }
    if (kind == "require" && ends_with(source_path, ".rb")) {
        const std::string strategy = reconstruct_ruby_resolved_by(import_string, is_external);
        return EdgeFidelity{strategy, ruby_edge_confidence(strategy)};
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
    rust["method"] = "per-stratum target-correctness / false-external rate vs an "
                     "independent mod-graph oracle + audited sample (offline)";
    rust["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 1164;
    rust["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_rust_mod}] = k_rust_mod_confidence;
    expected[std::string{k_strategy_rust_mod_unresolved}] = k_rust_mod_unresolved_confidence;
    expected[std::string{k_strategy_rust_use_std}] = k_rust_use_std_confidence;
    expected[std::string{k_strategy_rust_use_internal_resolved}] =
        k_rust_use_internal_resolved_confidence;
    expected[std::string{k_strategy_rust_use_internal_unresolved}] =
        k_rust_use_internal_unresolved_confidence;
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

/// Per-language fidelity sub-block for JavaScript/TypeScript import/require
/// edges. The same calibration applies to both languages (one resolver).
[[nodiscard]] nlohmann::json build_jsts_fidelity_json()
{
    nlohmann::json jsts;
    jsts["version"] = std::string{k_jsts_fidelity_version};
    jsts["scope"] = "javascript-typescript-import-edges";
    jsts["method"] = "per-strategy precision vs Node/TS resolution + tsconfig-paths "
                     "oracle (offline)";
    jsts["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 8;
    corpus["labeled_edges"] = 11981;
    jsts["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_jsts_relative_resolved}] = k_jsts_relative_resolved_confidence;
    expected[std::string{k_strategy_jsts_relative_unresolved}] =
        k_jsts_relative_unresolved_confidence;
    expected[std::string{k_strategy_jsts_alias_unresolved}] = k_jsts_alias_unresolved_confidence;
    expected[std::string{k_strategy_jsts_bare_external}] = k_jsts_bare_external_confidence;
    jsts["expected_precision"] = std::move(expected);
    return jsts;
}

/// Per-language fidelity sub-block for Java import edges.
[[nodiscard]] nlohmann::json build_java_fidelity_json()
{
    nlohmann::json java;
    java["version"] = std::string{k_java_fidelity_version};
    java["scope"] = "java-import-edges";
    java["method"] = "per-strategy precision vs source-parsed FQCN/package oracle + "
                     "Maven/Gradle dep check (offline)";
    java["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 3949;
    java["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_java_dotted_resolved}] = k_java_dotted_resolved_confidence;
    expected[std::string{k_strategy_java_wildcard_resolved}] = k_java_wildcard_resolved_confidence;
    expected[std::string{k_strategy_java_external_jdk}] = k_java_external_jdk_confidence;
    expected[std::string{k_strategy_java_external_thirdparty}] =
        k_java_external_thirdparty_confidence;
    expected[std::string{k_strategy_java_external_innertype}] =
        k_java_external_innertype_confidence;
    java["expected_precision"] = std::move(expected);
    return java;
}

/// Per-language fidelity sub-block for C# using edges.
[[nodiscard]] nlohmann::json build_csharp_fidelity_json()
{
    nlohmann::json cs;
    cs["version"] = std::string{k_csharp_fidelity_version};
    cs["scope"] = "csharp-using-edges";
    cs["method"] = "per-strategy precision / false-external rate vs source-derived "
                   "namespace oracle (offline); using static / aliased using not captured";
    cs["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 6;
    corpus["labeled_edges"] = 191526;
    cs["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_cs_internal}] = k_cs_internal_confidence;
    expected[std::string{k_strategy_cs_external_system}] = k_cs_external_system_confidence;
    expected[std::string{k_strategy_cs_external_thirdparty}] = k_cs_external_thirdparty_confidence;
    cs["expected_precision"] = std::move(expected);
    return cs;
}

/// Per-language fidelity sub-block for PHP require/include/use edges.
[[nodiscard]] nlohmann::json build_php_fidelity_json()
{
    nlohmann::json php;
    php["version"] = std::string{k_php_fidelity_version};
    php["scope"] = "php-import-edges";
    php["method"] = "per-strategy precision vs independent FQCN/path oracle (offline)";
    php["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 157;
    php["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_php_require_resolved}] = k_php_require_resolved_confidence;
    expected[std::string{k_strategy_php_require_external}] = k_php_require_external_confidence;
    expected[std::string{k_strategy_php_use_psr4}] = k_php_use_psr4_confidence;
    expected[std::string{k_strategy_php_use_nsindex_fanout}] = k_php_use_nsindex_fanout_confidence;
    expected[std::string{k_strategy_php_use_external_global}] =
        k_php_use_external_global_confidence;
    expected[std::string{k_strategy_php_use_external_namespaced}] =
        k_php_use_external_namespaced_confidence;
    php["expected_precision"] = std::move(expected);
    return php;
}

/// Per-language fidelity sub-block for Ruby require edges.
[[nodiscard]] nlohmann::json build_ruby_fidelity_json()
{
    nlohmann::json ruby;
    ruby["version"] = std::string{k_ruby_fidelity_version};
    ruby["scope"] = "ruby-require-edges";
    ruby["method"] = "per-stratum precision + false-external audit vs filesystem oracle, "
                     "full-population recheck (offline)";
    ruby["provisional"] = true;

    nlohmann::json corpus;
    corpus["projects"] = 3;
    corpus["labeled_edges"] = 121;
    ruby["corpus"] = std::move(corpus);

    nlohmann::json expected;
    expected[std::string{k_strategy_ruby_relative_explicit}] = k_ruby_relative_explicit_confidence;
    expected[std::string{k_strategy_ruby_resolved_multi}] = k_ruby_resolved_multi_confidence;
    expected[std::string{k_strategy_ruby_resolved_single}] = k_ruby_resolved_single_confidence;
    expected[std::string{k_strategy_ruby_external_stdlib}] = k_ruby_external_stdlib_confidence;
    expected[std::string{k_strategy_ruby_external_gem}] = k_ruby_external_gem_confidence;
    ruby["expected_precision"] = std::move(expected);
    return ruby;
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
    // JS and TS share one calibration; register under both names so a
    // consumer can look up by the concrete source language.
    const nlohmann::json jsts = build_jsts_fidelity_json();
    languages["javascript"] = jsts;
    languages["typescript"] = jsts;
    languages["java"] = build_java_fidelity_json();
    languages["csharp"] = build_csharp_fidelity_json();
    languages["php"] = build_php_fidelity_json();
    languages["ruby"] = build_ruby_fidelity_json();
    meta["languages"] = std::move(languages);
    return meta;
}

} // namespace vectis::code
