#include "cli/cli_main.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include "core/log.h"
#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/digest_exporter.h"
#include "modes/code/parser.h"
#include "modes/code/scanner.h"

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

EXAMPLES
    vectis digest ./my-project                    # JSON to stdout
    vectis digest ./my-project --format slim      # Slim JSON to stdout
    vectis digest . --format md --output DOC.md   # Markdown to file
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
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
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

    vectis::modes::code::ScanConfig config;
    config.root  = abs_root;
    config.epoch = 1;
    // Default excludes mirror CodeMode's opinion — skip build outputs,
    // package caches, and VCS metadata so the digest reflects source
    // intent rather than generated files.
    config.exclude_dir_names = {
        ".git", ".hg", ".svn",
        "node_modules", "target", "build", "build-win",
        "dist", "out", ".venv", "venv", "__pycache__",
        ".idea", ".vscode", ".vs",
    };

    std::atomic<std::int64_t>             epoch{1};
    vectis::core::CancellationToken       token;
    const auto                            noop_progress   = [](const auto&) {};
    const auto                            noop_completion = [](const auto&) {};

    const auto scan_result = vectis::modes::code::Scanner::run(
        config, index, parser, noop_progress, noop_completion, token, epoch);
    if (!scan_result) {
        std::fprintf(stderr, "error: scan failed: %s\n",
                     scan_result.error().message.c_str());
        return 2;
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
