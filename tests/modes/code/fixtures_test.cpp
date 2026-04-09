#include "modes/code/scanner.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/task_queue.h"
#include "modes/code/code_index.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"
#include "modes/code/symbol.h"

// VECTIS_FIXTURE_DIR is injected as a compile-time definition from
// tests/CMakeLists.txt so the tests can find `tests/fixtures/code/`
// regardless of where the binary is invoked from.
#ifndef VECTIS_FIXTURE_DIR
#error "VECTIS_FIXTURE_DIR is not defined — tests/CMakeLists.txt must pass it"
#endif

namespace {

using vectis::core::CancellationToken;
using vectis::modes::code::CodeIndex;
using vectis::modes::code::Language;
using vectis::modes::code::ScanConfig;
using vectis::modes::code::Scanner;
using vectis::modes::code::ScanProgress;
using vectis::modes::code::ScanSummary;
using vectis::modes::code::Symbol;
using vectis::modes::code::SymbolKind;
using vectis::modes::code::TreeSitterParser;

/// Helper: does the index contain a symbol by this name (any kind)?
bool index_has_symbol(const CodeIndex& index, std::string_view name)
{
    const auto matches = index.search_symbols(name);
    return std::any_of(matches.begin(), matches.end(), [&](const Symbol& s) {
        return s.name == name;
    });
}

/// Run a scan of a fixture subdirectory into the given index. Caller
/// owns `index` — we fill it in place because `CodeIndex` is
/// deliberately non-movable (its shared_mutex is pinned).
void scan_fixture(std::string_view fixture_name, CodeIndex& index)
{
    const std::filesystem::path fixture_root =
        std::filesystem::path{VECTIS_FIXTURE_DIR} / "code" / std::string{fixture_name};

    TreeSitterParser parser;
    parser.register_builtin_languages();

    ScanConfig cfg;
    cfg.root  = fixture_root;
    cfg.epoch = 1;

    std::atomic<std::int64_t> epoch{1};
    const CancellationToken   token{};

    const bool ok = Scanner::run(
        cfg, index, parser,
        [](const ScanProgress&) {},
        [](const ScanSummary&) {},
        token, epoch);
    EXPECT_TRUE(ok) << "scan of fixture '" << fixture_name << "' failed";
}

TEST(FixturesTest, SamplePython_ScansAndExtractsSymbols)
{
    CodeIndex index;
    scan_fixture("sample-python", index);

    // 3 files: main.py, models/user.py, utils/helpers.py
    EXPECT_EQ(index.file_count(), 3U);
    EXPECT_EQ(index.language_count(), 1U);

    // A handful of symbols we know should be there.
    EXPECT_TRUE(index_has_symbol(index, "run"));
    EXPECT_TRUE(index_has_symbol(index, "main"));
    EXPECT_TRUE(index_has_symbol(index, "User"));
    EXPECT_TRUE(index_has_symbol(index, "display_name"));
    EXPECT_TRUE(index_has_symbol(index, "format_greeting"));
    EXPECT_TRUE(index_has_symbol(index, "shout"));
}

TEST(FixturesTest, SampleTypeScript_ExtractsClassesAndInterfaces)
{
    CodeIndex index;
    scan_fixture("sample-typescript", index);

    // 3 .ts files under src/
    EXPECT_GE(index.file_count(), 3U);
    EXPECT_TRUE(index_has_symbol(index, "UserService"));
    EXPECT_TRUE(index_has_symbol(index, "findById"));
    EXPECT_TRUE(index_has_symbol(index, "User"));
    EXPECT_TRUE(index_has_symbol(index, "Repository"));
}

TEST(FixturesTest, SampleCpp_ExtractsWidgetClass)
{
    CodeIndex index;
    scan_fixture("sample-cpp", index);

    EXPECT_GE(index.file_count(), 3U);
    EXPECT_TRUE(index_has_symbol(index, "Widget"));
    EXPECT_TRUE(index_has_symbol(index, "Rect"));
    EXPECT_TRUE(index_has_symbol(index, "demo"));
}

TEST(FixturesTest, SampleRust_ExtractsTypesAndTraits)
{
    CodeIndex index;
    scan_fixture("sample-rust", index);

    EXPECT_GE(index.file_count(), 2U);
    EXPECT_TRUE(index_has_symbol(index, "Config"));
    EXPECT_TRUE(index_has_symbol(index, "Handler"));
    EXPECT_TRUE(index_has_symbol(index, "default_config"));
}

TEST(FixturesTest, MixedProject_RecognizesMultipleLanguages)
{
    CodeIndex index;
    scan_fixture("mixed", index);

    EXPECT_GE(index.file_count(), 4U);
    // Python server + TypeScript app + Java worker + SQL migration
    EXPECT_GE(index.language_count(), 3U);
}

} // namespace
