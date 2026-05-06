#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/parser.h"
#include "code/scanner.h"
#include "core/task_queue.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::ScanConfig;
using vectis::code::Scanner;
using vectis::code::ScanProgress;
using vectis::code::ScanSummary;
using vectis::code::TreeSitterParser;
using vectis::core::CancellationToken;
using vectis::core::TaskQueue;

/// Build a disposable temp directory that is cleaned up on test exit.
class ScannerFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_root =
            std::filesystem::temp_directory_path() / (std::string{"vectis_scanner_"} + test_name);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);

        m_parser.register_builtin_languages();
    }

    void TearDown() override { std::filesystem::remove_all(m_root); }

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
        cfg.root = m_root;
        cfg.exclude_dir_names = excludes;
        cfg.epoch = 1;

        std::atomic<std::int64_t> epoch{1};
        const CancellationToken token{}; // default = never cancelled

        const auto result = Scanner::run(
            cfg, index, m_parser, [](const ScanProgress&) {}, [](const ScanSummary&) {}, token,
            epoch);
        return result.has_value();
    }

    std::filesystem::path m_root;
    TreeSitterParser m_parser;
};

TEST_F(ScannerFixture, ScansSimpleTreeAndExtractsSymbols)
{
    write("a.py", "def alpha():\n    return 1\n");
    write("b.py", "class Beta:\n    pass\n");
    write("nested/c.ts", "export function gamma(): number { return 2; }\n");

    CodeIndex index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 3U);
    EXPECT_GE(index.symbol_count(), 3U);   // alpha, Beta, gamma at minimum
    EXPECT_EQ(index.language_count(), 2U); // Python + TypeScript
}

TEST_F(ScannerFixture, SkipsUnknownExtensionsAndNonSource)
{
    write("real.py", "def foo(): pass\n");
    write("notes.md", "# just a doc\n");
    write("image.bin", "garbage");
    write("Makefile", "all:\n\techo hi\n");

    CodeIndex index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 1U);
}

TEST_F(ScannerFixture, RespectsExcludeDirectoryNames)
{
    write("kept.py", "def a(): pass\n");
    write("node_modules/m.js", "function excluded() {}\n");
    write("build/artifact.cpp", "int main() { return 0; }\n");
    write("deep/nested/build/b.py", "def also_excluded(): pass\n");

    CodeIndex index;
    const bool ok = run_scan(index, {"node_modules", "build"});
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 1U);

    const auto files = index.snapshot_files();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].path_relative.filename().string(), "kept.py");
}

TEST_F(ScannerFixture, SkipsVendoredJavaScriptByFilename)
{
    // Even when not in a known vendored directory, a `jquery-1.6.1.js`
    // or `prototype.js` filename signals "third-party bundle". Scanner
    // skips these so they don't bloat symbol counts or hotspots.
    write("src/app.js", "function userCode() {}\n");
    write("static/jquery-1.6.1.js", "function jqInternal() {}\n");
    write("static/prototype.js", "function pInternal() {}\n");
    write("static/foo.min.js", "function minified() {}\n");

    CodeIndex index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 1U);

    const auto files = index.snapshot_files();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].path_relative.filename().string(), "app.js");
}

TEST_F(ScannerFixture, SkipsBinaryFiles)
{
    // Put a NUL byte within the first 512 bytes of a file that has a
    // source-like extension, so the only thing keeping it out of the
    // index is the binary-file heuristic. Write the bytes via an array
    // rather than a string literal so the embedded NUL is unambiguous.
    const std::filesystem::path full = m_root / "fake.py";
    const std::array<char, 11> bytes = {'h', 'e', 'l', 'l', 'o', '\0', 'w', 'o', 'r', 'l', 'd'};
    {
        std::ofstream stream(full, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_EQ(std::filesystem::file_size(full), bytes.size());

    CodeIndex index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 0U);
}

TEST_F(ScannerFixture, EmptyDirectoryScansCleanly)
{
    CodeIndex index;
    const bool ok = run_scan(index);
    ASSERT_TRUE(ok);
    EXPECT_EQ(index.file_count(), 0U);
    EXPECT_EQ(index.symbol_count(), 0U);
}

TEST_F(ScannerFixture, SuccessfulScanReturnsSummary)
{
    // Regression guard for the Result<ScanSummary> refactor: the
    // returned value must carry the same counts the scanner wrote
    // into the index.
    write("a.py", "def alpha():\n    return 1\n");
    write("sub/b.py", "def beta(): pass\n");
    write("sub/c.ts", "export function gamma() {}\n");

    CodeIndex index;
    ScanConfig cfg;
    cfg.root = m_root;
    cfg.epoch = 1;

    std::atomic<std::int64_t> epoch{1};
    const CancellationToken token{};

    const auto result = Scanner::run(
        cfg, index, m_parser, [](const ScanProgress&) {}, [](const ScanSummary&) {}, token, epoch);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->file_count, 3U);
    EXPECT_EQ(result->file_count, index.file_count());
    EXPECT_EQ(result->symbol_count, index.symbol_count());
    EXPECT_EQ(result->language_count, index.language_count());
}

TEST_F(ScannerFixture, MissingRootReturnsIoError)
{
    // Non-existent root should surface as an IoError, not a bool false.
    CodeIndex index;
    ScanConfig cfg;
    cfg.root = m_root / "does-not-exist-xyz";
    cfg.epoch = 1;

    std::atomic<std::int64_t> epoch{1};
    const CancellationToken token{};

    const auto result = Scanner::run(
        cfg, index, m_parser, [](const ScanProgress&) {}, [](const ScanSummary&) {}, token, epoch);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, vectis::core::ErrorKind::IoError);
}

TEST_F(ScannerFixture, CancelledBeforeStartReturnsCancelledError)
{
    // If the epoch has already moved by the time run() is called,
    // the scan aborts immediately with an ErrorKind::Cancelled.
    write("a.py", "def alpha(): pass\n");

    CodeIndex index;
    ScanConfig cfg;
    cfg.root = m_root;
    cfg.epoch = 1;

    // Seed epoch higher than config.epoch so the preemption check
    // fires on the first iteration.
    std::atomic<std::int64_t> epoch{2};
    const CancellationToken token{};

    const auto result = Scanner::run(
        cfg, index, m_parser, [](const ScanProgress&) {}, [](const ScanSummary&) {}, token, epoch);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, vectis::core::ErrorKind::Cancelled);
}

TEST_F(ScannerFixture, IncrementalScanRunsCompactWhenFilesAreDeleted)
{
    using vectis::code::IncrementalScanResult;

    // Initial state: three files, full scan, three live entries.
    write("a.py", "def alpha(): pass\n");
    write("b.py", "def beta(): pass\n");
    write("c.py", "def gamma(): pass\n");

    CodeIndex index;
    ASSERT_TRUE(run_scan(index));
    EXPECT_EQ(index.file_count(), 3U);
    const std::size_t symbols_before = index.symbol_count();
    EXPECT_GE(symbols_before, 3U);

    // Delete `b.py` from disk and run an incremental scan. compact()
    // is hooked at the end of run_incremental whenever files were
    // deleted or updated, so the surviving index should be tombstone-
    // free and the per-file lookups still consistent.
    std::filesystem::remove(m_root / "b.py");

    ScanConfig cfg;
    cfg.root = m_root;
    cfg.epoch = 1;
    std::atomic<std::int64_t> epoch{1};
    const CancellationToken token{};

    const auto result =
        Scanner::run_incremental(cfg, index, m_parser, [](const ScanProgress&) {}, token, epoch);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->files_deleted, 1U);
    EXPECT_EQ(index.file_count(), 2U);

    // Compaction invariant: the remaining symbols still resolve via
    // their per-file lookups (m_by_file was rebuilt with fresh indices)
    // and snapshot_files reports only live paths.
    const auto files = index.snapshot_files();
    ASSERT_EQ(files.size(), 2U);
    for (const auto& f : files) {
        EXPECT_FALSE(index.symbols_in_file(f.id).empty());
        EXPECT_NE(f.path_relative.filename().string(), "b.py");
    }
    // Verify the surviving symbols by NAME — a count assertion would
    // be brittle to tree-sitter grammar updates that emit additional
    // module-level captures. The named survivors come from the
    // surviving files (alpha, gamma) and beta must NOT reappear.
    const auto syms = index.snapshot_all_symbols();
    const auto has_name = [&](std::string_view n) {
        return std::any_of(syms.begin(), syms.end(), [&](const auto& s) { return s.name == n; });
    };
    EXPECT_TRUE(has_name("alpha"));
    EXPECT_TRUE(has_name("gamma"));
    EXPECT_FALSE(has_name("beta")) << "tombstoned symbol must not survive compact";
    EXPECT_LT(syms.size(), symbols_before);
}

} // namespace
