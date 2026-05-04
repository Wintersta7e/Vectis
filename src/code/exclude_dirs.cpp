#include "code/exclude_dirs.h"

namespace vectis::code {

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
