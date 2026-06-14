#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/cli_main.h"
#include "cli/mcp_server.h"
#include "core/version.h"

namespace {

struct CapturedRun
{
    int code = 0;
    std::string out;
    std::string err;
};

// Invoke the CLI dispatcher with `args` (argv[0] included), capturing
// everything written to the C++ streams. The guide / version paths emit
// via std::cout only, so this is sufficient to assert stdout content and
// stderr silence on those paths. Owns the argument strings so it can hand
// `run` a mutable `char**` without const-casting string literals.
CapturedRun run_cli(std::vector<std::string> args)
{
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }

    std::ostringstream out;
    std::ostringstream err;
    std::streambuf* old_out = std::cout.rdbuf(out.rdbuf());
    std::streambuf* old_err = std::cerr.rdbuf(err.rdbuf());
    const int code = vectis::cli::run(static_cast<int>(argv.size()), argv.data());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return CapturedRun{.code = code, .out = out.str(), .err = err.str()};
}

TEST(CliVersion, FlagPrintsVersionToStdout)
{
    const CapturedRun r = run_cli({"vectis", "--version"});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.out, std::string{"vectis "} + vectis::core::k_vectis_version + "\n");
    EXPECT_TRUE(r.err.empty()) << "stderr: " << r.err;
}

TEST(CliVersion, SubcommandPrintsVersionToStdout)
{
    const CapturedRun r = run_cli({"vectis", "version"});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.out, std::string{"vectis "} + vectis::core::k_vectis_version + "\n");
    EXPECT_TRUE(r.err.empty()) << "stderr: " << r.err;
}

// `guide`, `version`, and `--version` are print-and-exit commands like `help`:
// they intentionally ignore trailing arguments (only argv[1] is inspected),
// unlike `digest` / `explain`, which reject unknown options.
TEST(CliVersion, ToleratesExtraArgs)
{
    const CapturedRun r = run_cli({"vectis", "--version", "extra"});
    EXPECT_EQ(r.code, 0);
}

// The MCP server info must report the same single-source version constant
// the CLI and digest output use — guards the default member initialiser.
TEST(CliVersion, McpServerInfoDefaultsToVersionConstant)
{
    const vectis::cli::McpServerInfo info;
    EXPECT_EQ(info.version, vectis::core::k_vectis_version);
}

// --- guide ---------------------------------------------------------------

TEST(CliGuide, PrintsGuideToStdout)
{
    const CapturedRun r = run_cli({"vectis", "guide"});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(r.err.empty()) << "stderr: " << r.err;
    // The guide must teach both entry points and how to read the output.
    EXPECT_NE(r.out.find("vectis explain"), std::string::npos);
    EXPECT_NE(r.out.find("vectis digest"), std::string::npos);
    EXPECT_NE(r.out.find("--format slim"), std::string::npos);
    EXPECT_NE(r.out.find("vectis mcp"), std::string::npos);
    EXPECT_NE(r.out.find("target_file_id"), std::string::npos);
    EXPECT_NE(r.out.find("VECTIS"), std::string::npos);
}

// Print-and-exit leniency (see CliVersion.ToleratesExtraArgs).
TEST(CliGuide, ToleratesExtraArgs)
{
    const CapturedRun r = run_cli({"vectis", "guide", "extra"});
    EXPECT_EQ(r.code, 0);
}

// --- help ----------------------------------------------------------------

TEST(CliHelp, ListsAllSubcommands)
{
    const CapturedRun r = run_cli({"vectis", "--help"});
    EXPECT_EQ(r.code, 0);
    for (const char* cmd : {"digest", "explain", "mcp", "guide", "--version"}) {
        EXPECT_NE(r.out.find(cmd), std::string::npos) << "missing: " << cmd;
    }
}

} // namespace
