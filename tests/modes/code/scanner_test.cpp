#include "modes/code/scanner.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/parser.h"

namespace {

using vectis::core::CancellationToken;
using vectis::core::TaskQueue;
using vectis::modes::code::CodeIndex;
using vectis::modes::code::ScanConfig;
using vectis::modes::code::Scanner;
using vectis::modes::code::ScanProgress;
using vectis::modes::code::ScanSummary;
using vectis::modes::code::TreeSitterParser;

/// Build a disposable temp directory that is cleaned up on test exit.
class ScannerFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_root = std::filesystem::temp_directory_path() /
                 (std::string{"vectis_scanner_"} + test_name);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);

        m_parser.register_builtin_languages();
    }

    void TearDown() override
    {
        std::filesystem::remove_all(m_root);
    }

    void write(const std::filesystem::path& relative, std::string_view content) const
    {
        const auto full = m_root / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream stream(full);
        stream << content;
    }

    bool run_scan(CodeIndex& index, const std::unordered_set<std::string>& excludes = {})
    {
        ScanConfig cfg;
        cfg.root              = m_root;
        cfg.exclude_dir_names = excludes;
        cfg.epoch             = 1;

        std::atomic<std::int64_t> epoch{1};
        const CancellationToken   token{}; // default = never cancelled

        return Scanner::run(
            cfg, index, m_parser,
            [](const ScanProgress&) {},
            [](const ScanSummary&) {},
            token,
            epoch);
    }

    std::filesystem::path m_root;
    TreeSitterParser      m_parser;
};

TEST_F(ScannerFixture, ScansSimpleTreeAndExtractsSymbols)
{
    write("a.py",      "def alpha():\n    return 1\n");
    write("b.py",      "class Beta:\n    pass\n");
    write("nested/c.ts", "export function gamma(): number { return 2; }\n");

    CodeIndex  index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 3U);
    EXPECT_GE(index.symbol_count(), 3U);  // alpha, Beta, gamma at minimum
    EXPECT_EQ(index.language_count(), 2U); // Python + TypeScript
}

TEST_F(ScannerFixture, SkipsUnknownExtensionsAndNonSource)
{
    write("real.py",      "def foo(): pass\n");
    write("notes.md",     "# just a doc\n");
    write("image.bin",    "garbage");
    write("Makefile",     "all:\n\techo hi\n");

    CodeIndex  index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 1U);
}

TEST_F(ScannerFixture, RespectsExcludeDirectoryNames)
{
    write("kept.py",                 "def a(): pass\n");
    write("node_modules/m.js",       "function excluded() {}\n");
    write("build/artifact.cpp",      "int main() { return 0; }\n");
    write("deep/nested/build/b.py",  "def also_excluded(): pass\n");

    CodeIndex  index;
    const bool ok = run_scan(index, {"node_modules", "build"});
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 1U);

    const auto files = index.snapshot_files();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].path_relative.filename().string(), "kept.py");
}

TEST_F(ScannerFixture, SkipsBinaryFiles)
{
    // Put a NUL byte within the first 512 bytes of a file that has a
    // source-like extension, so the only thing keeping it out of the
    // index is the binary-file heuristic. Write the bytes via an array
    // rather than a string literal so the embedded NUL is unambiguous.
    const std::filesystem::path full = m_root / "fake.py";
    const std::array<char, 11> bytes = {
        'h', 'e', 'l', 'l', 'o', '\0', 'w', 'o', 'r', 'l', 'd'};
    {
        std::ofstream stream(full, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_EQ(std::filesystem::file_size(full), bytes.size());

    CodeIndex  index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 0U);
}

TEST_F(ScannerFixture, EmptyDirectoryScansCleanly)
{
    CodeIndex  index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 0U);
    EXPECT_EQ(index.symbol_count(), 0U);
}

} // namespace
