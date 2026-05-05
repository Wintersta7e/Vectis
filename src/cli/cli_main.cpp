#include "cli/cli_main.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/mcp_server.h"
#include "code/code_index.h"
#include "code/code_index_store.h"
#include "code/digest_exporter.h"
#include "code/exclude_dirs.h"
#include "code/explain.h"
#include "code/gitignore.h"
#include "code/parser.h"
#include "code/scanner.h"
#include "core/log.h"
#include "core/task_queue.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::cli {

namespace {

constexpr const char* k_usage = R"(vectis — portable developer intelligence tool

USAGE
    vectis digest <path> [opts]   Scan <path> and emit a structured digest.
    vectis explain <path> [opts]  Scan <path> and print a short narrative
                                  summary (architecture, scale, hotspots,
                                  dependencies). Plain text, ~20 lines.
    vectis mcp                    Start a Model Context Protocol server on
                                  stdio. Exposes the `digest` and `explain`
                                  tools to MCP clients (Claude Code, etc.).
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
    vectis mcp                                        # MCP server on stdio
)";

void print_usage()
{
    std::fputs(k_usage, stdout);
}

/// Parse `--format` value. Returns false on unknown value.
bool parse_format(std::string_view v, vectis::code::DigestFormat& out)
{
    using vectis::code::DigestFormat;
    if (v == "json") {
        out = DigestFormat::Json;
        return true;
    }
    if (v == "slim" || v == "slim-json") {
        out = DigestFormat::SlimJson;
        return true;
    }
    if (v == "md" || v == "markdown") {
        out = DigestFormat::Markdown;
        return true;
    }
    return false;
}

struct DigestArgs
{
    std::filesystem::path project_root;
    vectis::code::DigestFormat format = vectis::code::DigestFormat::Json;
    /// Empty = stdout. Otherwise a filesystem path.
    std::string output_path;
    /// Enable SQLite cache reuse. Implied by a non-empty `cache_dir`.
    bool use_cache = false;
    /// Override cache location. Empty = `<project_root>/vectis-data`.
    std::filesystem::path cache_dir;
    /// Suppress warnings on stderr. Errors still print.
    bool quiet = false;
    /// Emit per-scan stats to stderr on success.
    bool verbose = false;
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
        }
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            out.output_path = argv[i + 1];
            ++i;
        }
        else if (arg == "--cache") {
            out.use_cache = true;
        }
        else if (arg == "--cache-dir" && i + 1 < argc) {
            out.use_cache = true;
            out.cache_dir = argv[i + 1];
            ++i;
        }
        else if (arg == "--quiet" || arg == "-q") {
            out.quiet = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
        }
        else {
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
/// `.gitignore`. False negatives are catastrophic: a 100-source
/// Python project mistakenly scanned with its virtualenv ballooned
/// to 2200+ files, with hotspots dominated by upstream packaging
/// tools rather than the project itself.
vectis::code::ScanConfig make_scan_config(const std::filesystem::path& abs_root)
{
    vectis::code::ScanConfig config;
    config.root = abs_root;
    config.epoch = 1;
    config.exclude_dir_names = vectis::code::default_scanner_exclude_dir_names();
    // Layer in .gitignore-derived patterns. Exact-name inserts are idempotent
    // — duplicates against the built-ins are silently absorbed. Glob patterns
    // are appended; duplication is harmless because the scanner short-circuits
    // on the first match.
    auto gi = vectis::code::read_gitignore_dir_patterns(abs_root);
    for (const auto& name : gi.exact_names) {
        config.exclude_dir_names.insert(name);
    }
    for (auto& pattern : gi.glob_patterns) {
        config.exclude_dir_globs.push_back(std::move(pattern));
    }
    return config;
}

/// Run a scan without touching disk. Every invocation starts with an
/// empty in-memory CodeIndex and does a full tree walk.
int run_uncached(const vectis::code::ScanConfig& config, vectis::code::TreeSitterParser& parser,
                 vectis::code::CodeIndex& index)
{
    std::atomic<std::int64_t> epoch{1};
    vectis::core::CancellationToken token;
    const auto noop_progress = [](const auto&) {};
    const auto noop_completion = [](const auto&) {};

    const auto r = vectis::code::Scanner::run(config, index, parser, noop_progress, noop_completion,
                                              token, epoch);
    if (!r) {
        std::fprintf(stderr, "error: scan failed: %s\n", r.error().message.c_str());
        return 2;
    }
    return 0;
}

/// Cached path: open the SQLite state, load any existing index, do a
/// hash-based incremental scan, persist the updated state.
int run_cached(const vectis::code::ScanConfig& config, const std::filesystem::path& cache_dir,
               vectis::code::TreeSitterParser& parser, vectis::code::CodeIndex& index, bool quiet)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create cache dir %s: %s\n", cache_dir.string().c_str(),
                     ec.message().c_str());
        return 2;
    }

    vectis::services::StorageEngine storage;
    const fs::path db_path = cache_dir / "vectis.db";
    if (auto r = storage.open(db_path); !r) {
        std::fprintf(stderr, "error: cannot open cache DB %s: %s\n", db_path.string().c_str(),
                     r.error().message.c_str());
        return 2;
    }
    if (auto r = storage.migrate(); !r) {
        std::fprintf(stderr, "error: migration failed: %s\n", r.error().message.c_str());
        return 2;
    }

    vectis::services::IndexEngine index_engine;
    index_engine.initialize(&storage);

    const bool have_existing_cache = vectis::code::has_cache_for(storage, config.root);

    std::atomic<std::int64_t> epoch{1};
    vectis::core::CancellationToken token;
    const auto noop_progress = [](const auto&) {};

    if (have_existing_cache) {
        if (auto r = vectis::code::load_index(storage, index); !r) {
            if (!quiet) {
                std::fprintf(stderr,
                             "warning: cache load failed (%s); falling back "
                             "to full scan\n",
                             r.error().message.c_str());
            }
            // clear() resets the in-memory index without move-assigning
            // (CodeIndex is move-disabled to keep shared_mutex safe).
            index.clear();
        }
    }

    if (index.file_count() == 0) {
        // Cold cache: full scan + save.
        const auto noop_completion = [](const auto&) {};
        const auto r = vectis::code::Scanner::run(config, index, parser, noop_progress,
                                                  noop_completion, token, epoch);
        if (!r) {
            std::fprintf(stderr, "error: scan failed: %s\n", r.error().message.c_str());
            return 2;
        }
    }
    else {
        // Warm cache: hash-based incremental.
        const auto r = vectis::code::Scanner::run_incremental(config, index, parser, noop_progress,
                                                              token, epoch);
        if (!r) {
            std::fprintf(stderr, "error: incremental scan failed: %s\n", r.error().message.c_str());
            return 2;
        }
    }

    vectis::code::CacheMetadata meta;
    meta.project_root = config.root;
    meta.scan_timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
    if (auto r = vectis::code::save_index(storage, index, meta); !r) {
        if (!quiet) {
            std::fprintf(stderr, "warning: cache save failed: %s\n", r.error().message.c_str());
        }
        // Not fatal — the digest is still correct for this run.
    }
    return 0;
}

struct ExplainArgs
{
    std::filesystem::path project_root;
    bool use_cache = false;
    std::filesystem::path cache_dir;
    bool quiet = false;
};

bool parse_explain_args(int argc, char** argv, ExplainArgs& out)
{
    if (argc < 3) {
        std::fprintf(stderr, "error: `explain` requires a path argument.\n");
        return false;
    }
    out.project_root = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--cache") {
            out.use_cache = true;
        }
        else if (arg == "--cache-dir" && i + 1 < argc) {
            out.use_cache = true;
            out.cache_dir = argv[i + 1];
            ++i;
        }
        else if (arg == "--quiet" || arg == "-q") {
            out.quiet = true;
        }
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

/// Run the explain pipeline and return the narrative body. `error_out`
/// is set to a one-line message on failure (for callers that don't have
/// stderr available, e.g. the MCP server). Return code matches CLI
/// conventions: 0 on success, 1 on usage / argument error, 2 on scan
/// or I/O failure.
int compute_explain_body(const ExplainArgs& args, std::string& body, std::string& error_out)
{
    namespace fs = std::filesystem;
    body.clear();
    error_out.clear();

    std::error_code ec;
    if (!fs::exists(args.project_root, ec) || !fs::is_directory(args.project_root, ec)) {
        error_out = "not a directory: " + args.project_root.string();
        return 1;
    }
    const fs::path abs_root = fs::absolute(args.project_root, ec);
    if (ec) {
        error_out = std::string{"cannot resolve absolute path: "} + ec.message();
        return 1;
    }

    vectis::code::TreeSitterParser parser;
    parser.register_builtin_languages();
    vectis::code::CodeIndex index;

    const auto scan_config = make_scan_config(abs_root);
    int scan_rc = 0;
    if (args.use_cache) {
        const fs::path cache_dir =
            args.cache_dir.empty() ? abs_root / "vectis-data" : fs::absolute(args.cache_dir, ec);
        scan_rc = run_cached(scan_config, cache_dir, parser, index, args.quiet);
    }
    else {
        scan_rc = run_uncached(scan_config, parser, index);
    }
    if (scan_rc != 0) {
        error_out = "scan failed";
        return scan_rc;
    }

    vectis::code::ExplainOptions explain_opts;
    explain_opts.project_root = abs_root;
    explain_opts.project_name = abs_root.filename().string();
    explain_opts.exclude_dir_names = scan_config.exclude_dir_names;
    body = vectis::code::build_explanation(index, explain_opts);
    return 0;
}

int run_explain(const ExplainArgs& args)
{
    std::string body;
    std::string error_out;
    const int rc = compute_explain_body(args, body, error_out);
    if (rc != 0) {
        std::fprintf(stderr, "error: %s\n", error_out.c_str());
        return rc;
    }
    std::fwrite(body.data(), 1, body.size(), stdout);
    if (!body.empty() && body.back() != '\n') {
        std::fputc('\n', stdout);
    }
    return 0;
}

/// Run the digest pipeline and return the body. `error_out` is set to
/// a one-line message on failure (for callers without a stderr — e.g.
/// the MCP server). Same exit-code convention as `compute_explain_body`.
int compute_digest_body(const DigestArgs& args, std::string& body, std::string& error_out)
{
    namespace fs = std::filesystem;
    body.clear();
    error_out.clear();

    std::error_code ec;
    if (!fs::exists(args.project_root, ec) || !fs::is_directory(args.project_root, ec)) {
        error_out = "not a directory: " + args.project_root.string();
        return 1;
    }
    const fs::path abs_root = fs::absolute(args.project_root, ec);
    if (ec) {
        error_out = std::string{"cannot resolve absolute path: "} + ec.message();
        return 1;
    }

    vectis::code::TreeSitterParser parser;
    parser.register_builtin_languages();
    vectis::code::CodeIndex index;

    const auto t_start = std::chrono::steady_clock::now();

    // Build the scan config once: the same exclude set seeds the
    // scanner walk and (via ExportOptions) the architecture detector's
    // disk walk further down.
    const auto scan_config = make_scan_config(abs_root);

    int scan_rc = 0;
    if (args.use_cache) {
        const fs::path cache_dir =
            args.cache_dir.empty() ? abs_root / "vectis-data" : fs::absolute(args.cache_dir, ec);
        scan_rc = run_cached(scan_config, cache_dir, parser, index, args.quiet);
    }
    else {
        scan_rc = run_uncached(scan_config, parser, index);
    }
    if (scan_rc != 0) {
        error_out = "scan failed";
        return scan_rc;
    }

    if (args.verbose && !args.quiet) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t_start)
                                    .count();
        std::fprintf(stderr, "vectis: scanned %zu files, %zu symbols, %zu deps in %lld ms\n",
                     index.file_count(), index.symbol_count(), index.dependency_count(),
                     static_cast<long long>(elapsed_ms));
    }

    vectis::code::ExportOptions export_opts;
    export_opts.format = args.format;
    export_opts.project_root = abs_root;
    export_opts.project_name = abs_root.filename().string();
    export_opts.exclude_dir_names = scan_config.exclude_dir_names;

    body = vectis::code::build_digest_string(index, export_opts);
    return 0;
}

int run_digest(const DigestArgs& args)
{
    std::string body;
    std::string error_out;
    const int rc = compute_digest_body(args, body, error_out);
    if (rc != 0) {
        std::fprintf(stderr, "error: %s\n", error_out.c_str());
        return rc;
    }

    // Output: stdout if --output missing or '-', else the named file.
    if (args.output_path.empty() || args.output_path == "-") {
        std::fwrite(body.data(), 1, body.size(), stdout);
        if (!body.empty() && body.back() != '\n') {
            std::fputc('\n', stdout);
        }
    }
    else {
        std::ofstream out(args.output_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "error: cannot write to %s\n", args.output_path.c_str());
            return 2;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    return 0;
}

/// Build the digest tool's MCP handler. Captures nothing — the lambda
/// re-runs the full scan on each call. Most clients invoke `digest`
/// once per session, so caching across calls would just complicate the
/// memory model without helping.
[[nodiscard]] McpTool make_digest_tool()
{
    return McpTool{
        .name = "digest",
        .description =
            "Generate a structured digest of a source tree. Returns architecture label, "
            "scale, languages, hotspots, central files (PageRank), and dependency graph. "
            "Default format is `slim` JSON (~5-50KB depending on project size); use `md` "
            "for human-readable Markdown or `json` for the full digest with symbol bodies.",
        .input_schema_json = R"({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute or relative path to the project root to scan."
                },
                "format": {
                    "type": "string",
                    "enum": ["slim", "json", "md"],
                    "default": "slim",
                    "description": "Output format. `slim` is the agent-optimised JSON; `json` is the full digest; `md` is human-readable Markdown."
                }
            },
            "required": ["path"]
        })",
        .handler =
            [](const std::string& arguments_json) -> std::string {
            nlohmann::json args;
            try {
                args = nlohmann::json::parse(arguments_json);
            }
            catch (const nlohmann::json::parse_error& e) {
                throw McpHandlerError{-32602,
                                      std::string{"arguments not valid JSON: "} + e.what()};
            }
            if (!args.contains("path") || !args["path"].is_string()) {
                throw McpHandlerError{-32602, "missing required string argument `path`"};
            }
            DigestArgs cli_args;
            cli_args.project_root = args["path"].get<std::string>();
            cli_args.format = vectis::code::DigestFormat::SlimJson; // default
            cli_args.quiet = true; // suppress the scan progress to stderr
            if (args.contains("format")) {
                if (!args["format"].is_string()) {
                    throw McpHandlerError{-32602, "`format` must be a string"};
                }
                if (!parse_format(args["format"].get<std::string>(), cli_args.format)) {
                    throw McpHandlerError{-32602,
                                          "unknown format (expected slim|json|md)"};
                }
            }
            std::string body;
            std::string error_out;
            const int rc = compute_digest_body(cli_args, body, error_out);
            if (rc != 0) {
                throw McpHandlerError{-32603, std::move(error_out)};
            }
            return body;
        },
    };
}

[[nodiscard]] McpTool make_explain_tool()
{
    return McpTool{
        .name = "explain",
        .description =
            "Plain-text narrative summary of a source tree (~20 lines). Architecture "
            "label, scale, languages, top hotspots, most central files (PageRank), "
            "decorators, and dependency-graph stats. Designed for direct agent reading "
            "without JSON parsing.",
        .input_schema_json = R"({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Absolute or relative path to the project root to scan."
                }
            },
            "required": ["path"]
        })",
        .handler =
            [](const std::string& arguments_json) -> std::string {
            nlohmann::json args;
            try {
                args = nlohmann::json::parse(arguments_json);
            }
            catch (const nlohmann::json::parse_error& e) {
                throw McpHandlerError{-32602,
                                      std::string{"arguments not valid JSON: "} + e.what()};
            }
            if (!args.contains("path") || !args["path"].is_string()) {
                throw McpHandlerError{-32602, "missing required string argument `path`"};
            }
            ExplainArgs cli_args;
            cli_args.project_root = args["path"].get<std::string>();
            cli_args.quiet = true;
            std::string body;
            std::string error_out;
            const int rc = compute_explain_body(cli_args, body, error_out);
            if (rc != 0) {
                throw McpHandlerError{-32603, std::move(error_out)};
            }
            return body;
        },
    };
}

int run_mcp_subcommand()
{
    // Quiet-by-default — MCP clients communicate over stdio and any
    // stray stderr noise from the scanner ends up in the client's log.
    // The handlers themselves set quiet=true on each invocation.
    McpServerInfo info;
    info.name = "vectis";
    info.version = "0.1.0";
    const std::vector<McpTool> tools{make_digest_tool(), make_explain_tool()};
    return run_mcp_server(std::cin, std::cout, info, tools);
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
    if (cmd == "explain") {
        ExplainArgs args;
        if (!parse_explain_args(argc, argv, args)) {
            print_usage();
            return 1;
        }
        return run_explain(args);
    }
    if (cmd == "mcp") {
        return run_mcp_subcommand();
    }

    std::fprintf(stderr, "error: unknown subcommand '%s'\n", argv[1]);
    print_usage();
    return 1;
}

} // namespace vectis::cli
