#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli/cli_main.h"

namespace {

namespace fs = std::filesystem;

/// Pack C-string literals into the `char**` shape `run()` needs. The
/// caller keeps `argv` alive for as long as the returned vector is used.
class ArgvHolder
{
public:
    explicit ArgvHolder(std::vector<std::string> args) : m_storage(std::move(args))
    {
        m_argv.reserve(m_storage.size() + 1);
        for (auto& s : m_storage) {
            m_argv.push_back(s.data());
        }
        m_argv.push_back(nullptr);
    }

    int argc() const { return static_cast<int>(m_storage.size()); }
    char* const* argv() const { return m_argv.data(); }

private:
    std::vector<std::string> m_storage;
    std::vector<char*> m_argv;
};

/// Build a minimal project tree under `dir` with one C++ source file.
/// Returns the absolute dir path.
fs::path stage_tiny_project(const fs::path& dir)
{
    fs::remove_all(dir);
    fs::create_directories(dir / "src");
    {
        std::ofstream out(dir / "src" / "hello.cpp");
        out << "int hello() { return 1; }\n";
    }
    return fs::absolute(dir);
}

/// Unique per-test scratch path under `/tmp` (or platform tmp).
fs::path unique_tmp_dir(std::string_view tag)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("vectis_cli_test_" + std::string(tag) + "_" + std::to_string(now));
}

} // namespace

TEST(CliCacheTest, FirstRunCreatesDbAndEmitsDigest)
{
    const fs::path project = unique_tmp_dir("cold");
    stage_tiny_project(project);
    const fs::path out = project / "digest.json";

    ArgvHolder args({
        "vectis",
        "digest",
        project.string(),
        "--cache",
        "--format",
        "slim",
        "--output",
        out.string(),
    });
    EXPECT_EQ(vectis::cli::run(args.argc(), const_cast<char**>(args.argv())), 0);

    // Cache DB should exist inside `<project>/vectis-data/`.
    EXPECT_TRUE(fs::exists(project / "vectis-data" / "vectis.db"));
    EXPECT_TRUE(fs::exists(out));
    EXPECT_GT(fs::file_size(out), 0U);

    fs::remove_all(project);
}

TEST(CliCacheTest, WarmRunPicksUpNewFile)
{
    const fs::path project = unique_tmp_dir("warm");
    stage_tiny_project(project);
    const fs::path out = project / "digest.json";

    const std::vector<std::string> common = {
        "vectis",   "digest", project.string(), "--cache",
        "--format", "slim",   "--output",       out.string(),
    };

    // Cold run populates the cache.
    {
        ArgvHolder args(common);
        ASSERT_EQ(vectis::cli::run(args.argc(), const_cast<char**>(args.argv())), 0);
    }

    // Adding a file must be reflected in the next run's digest — the
    // incremental path should pick up the new hash, not serve a stale
    // snapshot of the cache.
    {
        std::ofstream added(project / "src" / "world.cpp");
        added << "int world() { return 2; }\n";
    }

    {
        ArgvHolder args(common);
        ASSERT_EQ(vectis::cli::run(args.argc(), const_cast<char**>(args.argv())), 0);
    }

    // Read the second digest and make sure `world.cpp` is listed. The
    // slim JSON contains a `files` array of paths.
    std::ifstream in(out);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("world.cpp"), std::string::npos);

    fs::remove_all(project);
}

TEST(CliCacheTest, CacheDirOverrideKeepsProjectClean)
{
    const fs::path project = unique_tmp_dir("override");
    const fs::path cache_dir = unique_tmp_dir("override_cache");
    stage_tiny_project(project);

    ArgvHolder args({
        "vectis",
        "digest",
        project.string(),
        "--cache-dir",
        cache_dir.string(),
        "--format",
        "slim",
        "--output",
        "-",
    });
    // Send stdout nowhere: freopen `/dev/null` to keep the test output
    // clean. Windows uses `NUL` instead.
#ifdef _WIN32
    std::FILE* devnull = std::freopen("NUL", "w", stdout);
#else
    std::FILE* devnull = std::freopen("/dev/null", "w", stdout);
#endif
    const int rc = vectis::cli::run(args.argc(), const_cast<char**>(args.argv()));
    (void)devnull;
    ASSERT_EQ(rc, 0);

    EXPECT_TRUE(fs::exists(cache_dir / "vectis.db"));
    // Project directory must NOT have a vectis-data/ when --cache-dir
    // points elsewhere.
    EXPECT_FALSE(fs::exists(project / "vectis-data"));

    fs::remove_all(project);
    fs::remove_all(cache_dir);
}
