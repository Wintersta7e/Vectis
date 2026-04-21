#include "cli/cli_main.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include "core/log.h"
#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/code_index_store.h"
#include "modes/code/digest_exporter.h"
#include "modes/code/gitignore.h"
#include "modes/code/parser.h"
#include "modes/code/scanner.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::cli {

namespace {

constexpr const char* k_usage = R"(vectis — portable developer intelligence tool

USAGE
    vectis                        Launch the GUI.
    vectis digest <path> [opts]   Scan <path> and emit a digest.
    vectis --help                 Show this text.

DIGEST OPTIONS
    --format json | slim | md     Output format (default: json).
    --output <file>               Write to <file>. Use '-' for stdout
                                  (default: stdout).
    --cache                       Reuse SQLite state between runs at
                                  <path>/vectis-data/vectis.db. First
                                  call populates; subsequent calls do
                                  a hash-based incremental scan.
    --cache-dir <dir>             Override the cache location (e.g. for
                                  read-only project dirs). Implies
                                  --cache.
    -q, --quiet                   Suppress non-error output on stderr.
    -v, --verbose                 Print scan stats (files, symbols,
                                  edges, elapsed ms) to stderr.

EXIT CODES
    0   success
    1   usage / argument error
    2   scan, export, or I/O failure

EXAMPLES
    vectis digest ./my-project                        # JSON to stdout, no cache
    vectis digest ./my-project --cache --format md    # Cached, markdown
    vectis digest . --cache-dir /tmp/vc --format slim # Custom cache dir
)";

void print_usage()
{
    std::fputs(k_usage, stdout);
}

/// Parse `--format` value. Returns false on unknown value.
bool parse_format(std::string_view v, vectis::modes::code::DigestFormat& out)
{
    using vectis::modes::code::DigestFormat;
    if (v == "json")                      { out = DigestFormat::Json;     return true; }
    if (v == "slim" || v == "slim-json")  { out = DigestFormat::SlimJson; return true; }
    if (v == "md"   || v == "markdown")   { out = DigestFormat::Markdown; return true; }
    return false;
}

struct DigestArgs {
    std::filesystem::path              project_root;
    vectis::modes::code::DigestFormat  format = vectis::modes::code::DigestFormat::Json;
    /// Empty = stdout. Otherwise a filesystem path.
    std::string                        output_path;
    /// Enable SQLite cache reuse. Implied by a non-empty `cache_dir`.
    bool                               use_cache = false;
    /// Override cache location. Empty = `<project_root>/vectis-data`.
    std::filesystem::path              cache_dir;
    /// Suppress warnings on stderr. Errors still print.
    bool                               quiet = false;
    /// Emit per-scan stats to stderr on success.
    bool                               verbose = false;
};

/// Parse `digest <path> [--format …] [--output …]`. Returns false on
/// malformed input; the caller prints usage on failure.
bool parse_digest_args(int argc, char** argv, DigestArgs& out)
{
    if (argc < 3) {
        std::fprintf(stderr, "error: `digest` requires a path argument.\n");
        return false;
    }
    out.project_root = argv[2];

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if ((arg == "--format" || arg == "-f") && i + 1 < argc) {
            if (!parse_format(argv[i + 1], out.format)) {
                std::fprintf(stderr, "error: unknown --format '%s'\n", argv[i + 1]);
                return false;
            }
            ++i;
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            out.output_path = argv[i + 1];
            ++i;
        } else if (arg == "--cache") {
            out.use_cache = true;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            out.use_cache = true;
            out.cache_dir = argv[i + 1];
            ++i;
        } else if (arg == "--quiet" || arg == "-q") {
            out.quiet = true;
        } else if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

/// Common ScanConfig used by both cached and uncached paths.
///
/// Two layers of exclude, merged into `config.exclude_dir_names`:
///
///   1. Built-in defaults — directory names that are almost always
///      generated artifacts, virtualenvs, caches, or IDE metadata. The
///      list is aggressive because a digest polluted with site-packages
///      or build intermediates is actively misleading to consumers.
///
///   2. Project `.gitignore` (simple name patterns only). Picks up
///      project-specific names the defaults miss (e.g. a user's
///      `build_venv/` or `third_party_vendored/`).
///
/// False positives (excluding something the user wanted indexed) are
/// rare and recoverable — the user can simply not list it in
/// `.gitignore`. False negatives (indexing site-packages) make the
/// digest worse than useless, as observed on a real external-python-project
/// scan that returned 2252 files where the project had ~100 sources
/// and hotspots dominated by pygments / PyInstaller / setuptools.
vectis::modes::code::ScanConfig
make_scan_config(const std::filesystem::path& abs_root)
{
    vectis::modes::code::ScanConfig config;
    config.root  = abs_root;
    config.epoch = 1;
    config.exclude_dir_names = {
        // VCS metadata
        ".git", ".hg", ".svn",
        // Language / framework build outputs
        "node_modules",
        "target", "build", "build-win", "out", "dist",
        "bin", "obj",
        "cmake-build-debug", "cmake-build-release",
        ".gradle",
        ".next", ".nuxt", ".svelte-kit",
        // Python virtualenvs + tool caches
        ".venv", "venv", "env", "virtualenv", "build_venv",
        "__pycache__", ".pytest_cache", ".mypy_cache",
        ".ruff_cache", ".tox",
        // Test / coverage artifacts
        "htmlcov",
        // Generic caches
        ".cache",
        // IDE metadata
        ".idea", ".vscode", ".vs",
        // Vectis's own cache dir so --cache doesn't re-scan its WAL.
        "vectis-data",
    };
    // Layer in .gitignore-derived names. insert() is idempotent — duplicates
    // against the built-ins are silently absorbed.
    auto gi = vectis::modes::code::read_gitignore_dir_patterns(abs_root);
    for (auto& name : gi) {
        config.exclude_dir_names.insert(std::move(name));
    }
    return config;
}

/// Run a scan without touching disk. Every invocation starts with an
/// empty in-memory CodeIndex and does a full tree walk.
int run_uncached(const std::filesystem::path&          abs_root,
                 vectis::modes::code::TreeSitterParser& parser,
                 vectis::modes::code::CodeIndex&       index)
{
    const auto config = make_scan_config(abs_root);
    std::atomic<std::int64_t>       epoch{1};
    vectis::core::CancellationToken token;
    const auto noop_progress   = [](const auto&) {};
    const auto noop_completion = [](const auto&) {};

    const auto r = vectis::modes::code::Scanner::run(
        config, index, parser, noop_progress, noop_completion, token, epoch);
    if (!r) {
        std::fprintf(stderr, "error: scan failed: %s\n",
                     r.error().message.c_str());
        return 2;
    }
    return 0;
}

/// Cached path: open the SQLite state, load any existing index, do a
/// hash-based incremental scan, persist the updated state.
int run_cached(const std::filesystem::path&          abs_root,
               const std::filesystem::path&          cache_dir,
               vectis::modes::code::TreeSitterParser& parser,
               vectis::modes::code::CodeIndex&       index,
               bool                                  quiet)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create cache dir %s: %s\n",
                     cache_dir.string().c_str(), ec.message().c_str());
        return 2;
    }

    vectis::services::StorageEngine storage;
    const fs::path db_path = cache_dir / "vectis.db";
    if (auto r = storage.open(db_path); !r) {
        std::fprintf(stderr, "error: cannot open cache DB %s: %s\n",
                     db_path.string().c_str(), r.error().message.c_str());
        return 2;
    }
    if (auto r = storage.migrate(); !r) {
        std::fprintf(stderr, "error: migration failed: %s\n",
                     r.error().message.c_str());
        return 2;
    }

    vectis::services::IndexEngine index_engine;
    index_engine.initialize(storage);

    const bool have_existing_cache =
        vectis::modes::code::has_cache_for(storage, abs_root);

    const auto config = make_scan_config(abs_root);
    std::atomic<std::int64_t>       epoch{1};
    vectis::core::CancellationToken token;
    const auto noop_progress = [](const auto&) {};

    if (have_existing_cache) {
        if (auto r = vectis::modes::code::load_index(storage, index); !r) {
            if (!quiet) {
                std::fprintf(stderr,
                             "warning: cache load failed (%s); falling back "
                             "to full scan\n", r.error().message.c_str());
            }
            // clear() resets the in-memory index without move-assigning
            // (CodeIndex is move-disabled to keep shared_mutex safe).
            index.clear();
        }
    }

    if (index.file_count() == 0) {
        // Cold cache: full scan + save.
        const auto noop_completion = [](const auto&) {};
        const auto r = vectis::modes::code::Scanner::run(
            config, index, parser,
            noop_progress, noop_completion, token, epoch);
        if (!r) {
            std::fprintf(stderr, "error: scan failed: %s\n",
                         r.error().message.c_str());
            return 2;
        }
    } else {
        // Warm cache: hash-based incremental.
        const auto r = vectis::modes::code::Scanner::run_incremental(
            config, index, parser, noop_progress, token, epoch);
        if (!r) {
            std::fprintf(stderr, "error: incremental scan failed: %s\n",
                         r.error().message.c_str());
            return 2;
        }
    }

    vectis::modes::code::CacheMetadata meta;
    meta.project_root   = abs_root;
    meta.scan_timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (auto r = vectis::modes::code::save_index(storage, index, meta); !r) {
        if (!quiet) {
            std::fprintf(stderr, "warning: cache save failed: %s\n",
                         r.error().message.c_str());
        }
        // Not fatal — the digest is still correct for this run.
    }
    return 0;
}

int run_digest(const DigestArgs& args)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(args.project_root, ec) ||
        !fs::is_directory(args.project_root, ec))
    {
        std::fprintf(stderr, "error: not a directory: %s\n",
                     args.project_root.string().c_str());
        return 1;
    }
    const fs::path abs_root = fs::absolute(args.project_root, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot resolve absolute path: %s\n",
                     ec.message().c_str());
        return 1;
    }

    vectis::modes::code::TreeSitterParser parser;
    parser.register_builtin_languages();
    vectis::modes::code::CodeIndex index;

    const auto t_start = std::chrono::steady_clock::now();

    int scan_rc = 0;
    if (args.use_cache) {
        const fs::path cache_dir = args.cache_dir.empty()
            ? abs_root / "vectis-data"
            : fs::absolute(args.cache_dir, ec);
        scan_rc = run_cached(abs_root, cache_dir, parser, index, args.quiet);
    } else {
        scan_rc = run_uncached(abs_root, parser, index);
    }
    if (scan_rc != 0) {
        return scan_rc;
    }

    if (args.verbose && !args.quiet) {
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_start).count();
        std::fprintf(stderr,
                     "vectis: scanned %zu files, %zu symbols, %zu deps in %lld ms\n",
                     index.file_count(),
                     index.symbol_count(),
                     index.dependency_count(),
                     static_cast<long long>(elapsed_ms));
    }

    vectis::modes::code::ExportOptions export_opts;
    export_opts.format       = args.format;
    export_opts.project_root = abs_root;
    export_opts.project_name = abs_root.filename().string();

    const std::string body =
        vectis::modes::code::build_digest_string(index, export_opts);

    // Output: stdout if --output missing or '-', else the named file.
    if (args.output_path.empty() || args.output_path == "-") {
        std::fwrite(body.data(), 1, body.size(), stdout);
        if (!body.empty() && body.back() != '\n') {
            std::fputc('\n', stdout);
        }
    } else {
        std::ofstream out(args.output_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "error: cannot write to %s\n",
                         args.output_path.c_str());
            return 2;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    return 0;
}

} // namespace

int run(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string_view cmd{argv[1]};
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "digest") {
        DigestArgs args;
        if (!parse_digest_args(argc, argv, args)) {
            print_usage();
            return 1;
        }
        return run_digest(args);
    }

    std::fprintf(stderr, "error: unknown subcommand '%s'\n", argv[1]);
    print_usage();
    return 1;
}

} // namespace vectis::cli
