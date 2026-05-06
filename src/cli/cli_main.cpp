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
#include "core/result.h"
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
    --format json | slim | md     Output format (default: slim).
                                  `slim` is structure-only and the
                                  most token-efficient for agent
                                  context. `json` adds hotspot body
                                  excerpts; `md` is human-readable.
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
    vectis digest ./my-project                        # Slim JSON to stdout
    vectis digest ./my-project --format json          # Full JSON with excerpts
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
    vectis::code::DigestFormat format = vectis::code::DigestFormat::SlimJson;
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

    int i = 3;
    while (i < argc) {
        const std::string_view arg{argv[i]};
        if ((arg == "--format" || arg == "-f") && i + 1 < argc) {
            if (!parse_format(argv[i + 1], out.format)) {
                std::fprintf(stderr, "error: unknown --format '%s'\n", argv[i + 1]);
                return false;
            }
            i += 2;
        }
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            out.output_path = argv[i + 1];
            i += 2;
        }
        else if (arg == "--cache") {
            out.use_cache = true;
            ++i;
        }
        else if (arg == "--cache-dir" && i + 1 < argc) {
            out.use_cache = true;
            out.cache_dir = argv[i + 1];
            i += 2;
        }
        else if (arg == "--quiet" || arg == "-q") {
            out.quiet = true;
            ++i;
        }
        else if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
            ++i;
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
[[nodiscard]] vectis::core::Result<vectis::code::ScanSummary>
run_uncached(const vectis::code::ScanConfig& config, vectis::code::TreeSitterParser& parser,
             vectis::code::CodeIndex& index)
{
    std::atomic<std::int64_t> epoch{1};
    vectis::core::CancellationToken token;
    const auto noop_progress = [](const auto&) {};
    vectis::code::ScanSummary summary;
    const auto on_complete = [&summary](const vectis::code::ScanSummary& s) { summary = s; };

    auto r =
        vectis::code::Scanner::run(config, index, parser, noop_progress, on_complete, token, epoch);
    if (!r) {
        std::fprintf(stderr, "error: scan failed: %s\n", r.error().message.c_str());
        return tl::unexpected{r.error()};
    }
    return summary;
}

/// Cached path: open the SQLite state, load any existing index, do a
/// hash-based incremental scan, persist the updated state.
[[nodiscard]] vectis::core::Result<vectis::code::ScanSummary>
run_cached(const vectis::code::ScanConfig& config, const std::filesystem::path& cache_dir,
           vectis::code::TreeSitterParser& parser, vectis::code::CodeIndex& index, bool quiet)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create cache dir %s: %s\n", cache_dir.string().c_str(),
                     ec.message().c_str());
        return vectis::core::make_error(vectis::core::ErrorKind::IoError, "cannot create cache dir",
                                        ec.message());
    }

    vectis::services::StorageEngine storage;
    const fs::path db_path = cache_dir / "vectis.db";
    if (auto r = storage.open(db_path); !r) {
        std::fprintf(stderr, "error: cannot open cache DB %s: %s\n", db_path.string().c_str(),
                     r.error().message.c_str());
        return tl::unexpected{r.error()};
    }
    if (auto r = storage.migrate(); !r) {
        std::fprintf(stderr, "error: migration failed: %s\n", r.error().message.c_str());
        return tl::unexpected{r.error()};
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
            // CodeIndex is move-disabled (shared_mutex); clear() in place.
            index.clear();
        }
    }

    vectis::code::ScanSummary summary;
    if (index.file_count() == 0) {
        const auto on_complete = [&summary](const vectis::code::ScanSummary& s) { summary = s; };
        auto r = vectis::code::Scanner::run(config, index, parser, noop_progress, on_complete,
                                            token, epoch);
        if (!r) {
            std::fprintf(stderr, "error: scan failed: %s\n", r.error().message.c_str());
            return tl::unexpected{r.error()};
        }
    }
    else {
        auto r = vectis::code::Scanner::run_incremental(config, index, parser, noop_progress, token,
                                                        epoch);
        if (!r) {
            std::fprintf(stderr, "error: incremental scan failed: %s\n", r.error().message.c_str());
            return tl::unexpected{r.error()};
        }
        summary.file_count = index.file_count();
        summary.symbol_count = index.symbol_count();
        summary.language_count = index.language_count();
        summary.files_skipped = r->files_skipped;
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
    return summary;
}

struct ExplainArgs
{
    std::filesystem::path project_root;
    bool use_cache = false;
    std::filesystem::path cache_dir;
    bool quiet = false;
    bool verbose = false;
};

bool parse_explain_args(int argc, char** argv, ExplainArgs& out)
{
    if (argc < 3) {
        std::fprintf(stderr, "error: `explain` requires a path argument.\n");
        return false;
    }
    out.project_root = argv[2];
    int i = 3;
    while (i < argc) {
        const std::string_view arg{argv[i]};
        if (arg == "--cache") {
            out.use_cache = true;
            ++i;
        }
        else if (arg == "--cache-dir" && i + 1 < argc) {
            out.use_cache = true;
            out.cache_dir = argv[i + 1];
            i += 2;
        }
        else if (arg == "--quiet" || arg == "-q") {
            out.quiet = true;
            out.verbose = false;
            ++i;
        }
        else if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
            out.quiet = false;
            ++i;
        }
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

struct ScanInputs
{
    std::filesystem::path abs_root;
    vectis::code::ScanConfig scan_config;
    std::uint64_t files_skipped = 0;
};

[[nodiscard]] vectis::core::Result<ScanInputs>
prepare_and_run_scan(const std::filesystem::path& project_root, bool use_cache,
                     const std::filesystem::path& cache_dir, bool quiet,
                     vectis::code::TreeSitterParser& parser, vectis::code::CodeIndex& index)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::is_directory(project_root, ec) || ec) {
        return vectis::core::make_error(vectis::core::ErrorKind::IoError, "not a directory",
                                        project_root.string());
    }
    fs::path abs_root = fs::absolute(project_root, ec);
    if (ec) {
        return vectis::core::make_error(vectis::core::ErrorKind::IoError,
                                        "cannot resolve absolute path", ec.message());
    }

    parser.register_builtin_languages();
    auto scan_config = make_scan_config(abs_root);

    vectis::core::Result<vectis::code::ScanSummary> scan_result =
        use_cache
            ? run_cached(scan_config,
                         cache_dir.empty() ? abs_root / "vectis-data" : fs::absolute(cache_dir, ec),
                         parser, index, quiet)
            : run_uncached(scan_config, parser, index);
    if (!scan_result) {
        // run_cached / run_uncached already log to stderr; the Result lets
        // non-CLI callers (e.g. the MCP server) see the failure too.
        return tl::unexpected{scan_result.error()};
    }

    return ScanInputs{
        .abs_root = std::move(abs_root),
        .scan_config = std::move(scan_config),
        .files_skipped = scan_result->files_skipped,
    };
}

[[nodiscard]] vectis::core::Result<std::string> compute_explain_body(const ExplainArgs& args)
{
    vectis::code::TreeSitterParser parser;
    vectis::code::CodeIndex index;

    const auto t_start = std::chrono::steady_clock::now();
    auto scan = prepare_and_run_scan(args.project_root, args.use_cache, args.cache_dir, args.quiet,
                                     parser, index);
    if (!scan) {
        return tl::unexpected{scan.error()};
    }

    if (args.verbose && !args.quiet) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t_start)
                                    .count();
        std::fprintf(
            stderr, "vectis: scanned %zu files (%llu skipped), %zu symbols, %zu deps in %lld ms\n",
            index.file_count(), static_cast<unsigned long long>(scan->files_skipped),
            index.symbol_count(), index.dependency_count(), static_cast<long long>(elapsed_ms));
    }

    vectis::code::ExplainOptions explain_opts;
    explain_opts.project_root = scan->abs_root;
    explain_opts.project_name = scan->abs_root.filename().string();
    explain_opts.exclude_dir_names = scan->scan_config.exclude_dir_names;
    return vectis::code::build_explanation(index, explain_opts);
}

int run_explain(const ExplainArgs& args)
{
    auto body = compute_explain_body(args);
    if (!body) {
        const auto& err = body.error();
        std::fprintf(stderr, "error: %s%s%s\n", err.message.c_str(),
                     err.context.empty() ? "" : ": ", err.context.c_str());
        return err.kind == vectis::core::ErrorKind::IoError ? 2 : 1;
    }
    std::fwrite(body->data(), 1, body->size(), stdout);
    if (!body->empty() && body->back() != '\n') {
        std::fputc('\n', stdout);
    }
    return 0;
}

[[nodiscard]] vectis::core::Result<std::string> compute_digest_body(const DigestArgs& args)
{
    vectis::code::TreeSitterParser parser;
    vectis::code::CodeIndex index;

    const auto t_start = std::chrono::steady_clock::now();
    auto scan = prepare_and_run_scan(args.project_root, args.use_cache, args.cache_dir, args.quiet,
                                     parser, index);
    if (!scan) {
        return tl::unexpected{scan.error()};
    }

    if (args.verbose && !args.quiet) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t_start)
                                    .count();
        std::fprintf(
            stderr, "vectis: scanned %zu files (%llu skipped), %zu symbols, %zu deps in %lld ms\n",
            index.file_count(), static_cast<unsigned long long>(scan->files_skipped),
            index.symbol_count(), index.dependency_count(), static_cast<long long>(elapsed_ms));
    }

    vectis::code::ExportOptions export_opts;
    export_opts.format = args.format;
    export_opts.project_root = scan->abs_root;
    export_opts.project_name = scan->abs_root.filename().string();
    export_opts.exclude_dir_names = scan->scan_config.exclude_dir_names;

    return vectis::code::build_digest_string(index, export_opts);
}

int run_digest(const DigestArgs& args)
{
    auto body = compute_digest_body(args);
    if (!body) {
        const auto& err = body.error();
        std::fprintf(stderr, "error: %s%s%s\n", err.message.c_str(),
                     err.context.empty() ? "" : ": ", err.context.c_str());
        return err.kind == vectis::core::ErrorKind::IoError ? 2 : 1;
    }

    if (args.output_path.empty() || args.output_path == "-") {
        std::fwrite(body->data(), 1, body->size(), stdout);
        if (!body->empty() && body->back() != '\n') {
            std::fputc('\n', stdout);
        }
        return 0;
    }
    std::ofstream out(args.output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "error: cannot write to %s\n", args.output_path.c_str());
        return 2;
    }
    out.write(body->data(), static_cast<std::streamsize>(body->size()));
    return 0;
}

// MCP handlers re-run the full scan on each call. Most clients invoke
// `digest` once per session, so caching across calls would just complicate
// the memory model without helping.

[[nodiscard]] nlohmann::json parse_mcp_arguments(const std::string& arguments_json)
{
    try {
        return nlohmann::json::parse(arguments_json);
    }
    catch (const nlohmann::json::parse_error& e) {
        throw McpHandlerError{-32602, std::string{"arguments not valid JSON: "} + e.what()};
    }
}

[[nodiscard]] std::string require_string_arg(const nlohmann::json& args, const char* key)
{
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string()) {
        throw McpHandlerError{-32602,
                              std::string{"missing required string argument `"} + key + "`"};
    }
    return it->get<std::string>();
}

/// Map a Result error to McpHandlerError. IoError maps to JSON-RPC's
/// "Invalid params" since the caller-supplied path is the usual root
/// cause; everything else is "Internal error".
[[noreturn]] void throw_handler_error(const vectis::core::Error& err)
{
    const int code = err.kind == vectis::core::ErrorKind::IoError ? -32602 : -32603;
    std::string msg = err.message;
    if (!err.context.empty()) {
        msg += ": " + err.context;
    }
    throw McpHandlerError{code, std::move(msg)};
}

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
        .handler = [](const std::string& arguments_json) -> std::string {
            const nlohmann::json args = parse_mcp_arguments(arguments_json);
            DigestArgs cli_args;
            cli_args.project_root = require_string_arg(args, "path");
            cli_args.format = vectis::code::DigestFormat::SlimJson;
            cli_args.quiet = true;
            if (const auto fmt_it = args.find("format"); fmt_it != args.end()) {
                if (!fmt_it->is_string()) {
                    throw McpHandlerError{-32602, "`format` must be a string"};
                }
                if (!parse_format(fmt_it->get<std::string>(), cli_args.format)) {
                    throw McpHandlerError{-32602, "unknown format (expected slim|json|md)"};
                }
            }
            auto body = compute_digest_body(cli_args);
            if (!body) {
                throw_handler_error(body.error());
            }
            return std::move(*body);
        },
    };
}

[[nodiscard]] McpTool make_explain_tool()
{
    return McpTool{
        .name = "explain",
        .description = "Plain-text narrative summary of a source tree (~20 lines). Architecture "
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
        .handler = [](const std::string& arguments_json) -> std::string {
            const nlohmann::json args = parse_mcp_arguments(arguments_json);
            ExplainArgs cli_args;
            cli_args.project_root = require_string_arg(args, "path");
            cli_args.quiet = true;
            auto body = compute_explain_body(cli_args);
            if (!body) {
                throw_handler_error(body.error());
            }
            return std::move(*body);
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
