#include "code/exclude_dirs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>

namespace vectis::code {

namespace {

[[nodiscard]] std::string ascii_lower(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

bool looks_like_vendored_js(std::string_view filename) noexcept
{
    // Anything with a .min.js extension is by convention a built /
    // minified bundle, not source the user authored.
    const std::string lower = ascii_lower(filename);
    if (lower.size() > 7 && lower.ends_with(".min.js")) {
        return true;
    }

    // Common library bundle filenames. Matches both the bare name and
    // version-suffixed forms (jquery-1.6.1.js, jquery-3.7.1.min.js,
    // angular-1.x.js, etc.). Patterns ordered prefix → exact.
    static constexpr std::array<std::string_view, 14> k_prefixes = {
        "jquery-",  "jquery.",     "prototype",   "require", "requirejs", "angular-",   "angular.",
        "backbone", "underscore-", "underscore.", "lodash-", "lodash.",   "bootstrap.", "dhtmlx",
    };
    static constexpr std::array<std::string_view, 8> k_exact = {
        "jquery.js",     "prototype.js", "require.js", "requirejs.js",
        "underscore.js", "lodash.js",    "ember.js",   "backbone.js",
    };

    if (std::ranges::any_of(k_exact, [&](std::string_view e) { return lower == e; })) {
        return true;
    }
    return std::ranges::any_of(k_prefixes, [&](std::string_view p) {
        return lower.starts_with(p) && (lower.ends_with(".js") || lower.ends_with(".js.gz"));
    });
}

const std::unordered_set<std::string>& default_scanner_exclude_dir_names()
{
    // Why err on the side of more exclusion: false positives (skipping
    // a dir the user wanted indexed) are rare and recoverable. False
    // negatives — indexing an upstream virtualenv or a coverage report —
    // balloon digest size and skew hotspots toward third-party code.
    static const std::unordered_set<std::string> k_default = {
        // VCS metadata
        ".git",
        ".hg",
        ".svn",
        // Language / framework build outputs
        "node_modules",
        "bower_components",
        "vendor",
        "third_party",
        "third-party",
        "overlays",
        "target",
        "build",
        "build-win",
        "out",
        "dist",
        "bin",
        "obj",
        "cmake-build-debug",
        "cmake-build-release",
        ".gradle",
        ".next",
        ".nuxt",
        ".svelte-kit",
        // Python virtualenvs + tool caches
        ".venv",
        "venv",
        "env",
        "virtualenv",
        "build_venv",
        "__pycache__",
        ".pytest_cache",
        ".mypy_cache",
        ".ruff_cache",
        ".tox",
        // Test / coverage artifacts
        "htmlcov",
        "coverage",
        // Runtime / scratch
        "tmp",
        ".tmp",
        "log",
        "logs",
        // Generic caches
        ".cache",
        // IDE metadata
        ".idea",
        ".vscode",
        ".vs",
        // Vectis's own cache dir so --cache doesn't re-scan its WAL.
        "vectis-data",
    };
    return k_default;
}

} // namespace vectis::code
