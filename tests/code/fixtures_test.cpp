#include "code/scanner.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/task_queue.h"
#include "code/code_index.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"

// VECTIS_FIXTURE_DIR is injected as a compile-time definition from
// tests/CMakeLists.txt so the tests can find `tests/fixtures/code/`
// regardless of where the binary is invoked from.
#ifndef VECTIS_FIXTURE_DIR
#error "VECTIS_FIXTURE_DIR is not defined — tests/CMakeLists.txt must pass it"
#endif

namespace {

using vectis::core::CancellationToken;
using vectis::code::CodeIndex;
using vectis::code::Language;
using vectis::code::ScanConfig;
using vectis::code::Scanner;
using vectis::code::ScanProgress;
using vectis::code::ScanSummary;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::TreeSitterParser;

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

    const auto result = Scanner::run(
        cfg, index, parser,
        [](const ScanProgress&) {},
        [](const ScanSummary&) {},
        token, epoch);
    EXPECT_TRUE(result.has_value())
        << "scan of fixture '" << fixture_name << "' failed";
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

TEST(FixturesTest, SampleCpp_ScannerPopulatesDependencies)
{
    CodeIndex index;
    scan_fixture("sample-cpp", index);

    // widget.cpp includes widget.hpp, main.cpp includes widget.hpp.
    // The scanner should register both as dependency edges.
    EXPECT_GE(index.dependency_count(), 2U)
        << "expected at least 2 #include edges in sample-cpp";

    // Find the widget.hpp file id and assert someone depends on it.
    std::int64_t widget_hpp_id = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.path_relative.filename().string() == "widget.hpp") {
            widget_hpp_id = f.id;
            break;
        }
    }
    ASSERT_NE(widget_hpp_id, 0);
    const auto dependents = index.dependents_of(widget_hpp_id);
    EXPECT_GE(dependents.size(), 2U)
        << "widget.hpp should be included by both main.cpp and widget.cpp";
}

TEST(FixturesTest, SamplePython_ScannerResolvesImports)
{
    CodeIndex index;
    scan_fixture("sample-python", index);

    // main.py imports models.user and utils.helpers; utils/helpers.py
    // imports models.user. All three should resolve internally.
    std::int64_t user_py_id = 0;
    for (const auto& f : index.snapshot_files()) {
        if (f.path_relative.generic_string() == "models/user.py") {
            user_py_id = f.id;
            break;
        }
    }
    ASSERT_NE(user_py_id, 0);
    const auto user_dependents = index.dependents_of(user_py_id);
    // Both main.py and helpers.py should depend on models/user.py.
    EXPECT_GE(user_dependents.size(), 2U);
}

} // namespace
