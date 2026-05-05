#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/explain.h"
#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::build_explanation;
using vectis::code::CodeIndex;
using vectis::code::ExplainOptions;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::Visibility;

void populate_test_index(CodeIndex& index)
{
    FileEntry f1;
    f1.path_relative = "src/app.py";
    f1.language = Language::Python;
    f1.size = 4096;
    f1.line_count = 120;
    const auto fid1 = index.add_file(std::move(f1));

    FileEntry f2;
    f2.path_relative = "src/_internal.py";
    f2.language = Language::Python;
    f2.size = 2048;
    f2.line_count = 60;
    const auto fid2 = index.add_file(std::move(f2));

    const std::array<Symbol, 3> a_syms = {
        Symbol{.file_id = fid1,
               .name = "Server",
               .kind = SymbolKind::Class,
               .line_start = 1,
               .line_end = 80,
               .visibility = Visibility::Public},
        Symbol{.file_id = fid1,
               .name = "handle_request",
               .kind = SymbolKind::Function,
               .line_start = 30,
               .line_end = 70,
               .signature = "def handle_request(req)",
               .complexity = 18,
               .visibility = Visibility::Public},
        Symbol{.file_id = fid1,
               .name = "_helper",
               .kind = SymbolKind::Function,
               .line_start = 90,
               .line_end = 95,
               .signature = "def _helper()",
               .complexity = 1,
               .visibility = Visibility::Private},
    };
    const std::array<Symbol, 1> b_syms = {
        Symbol{.file_id = fid2,
               .name = "_internal_init",
               .kind = SymbolKind::Function,
               .line_start = 1,
               .line_end = 10,
               .signature = "def _internal_init()",
               .complexity = 1,
               .visibility = Visibility::Private},
    };
    index.add_symbols(a_syms);
    index.add_symbols(b_syms);
}

TEST(ExplainTest, ProducesHeaderArchitectureScaleAndApiSurface)
{
    CodeIndex idx;
    populate_test_index(idx);
    ExplainOptions opts;
    opts.project_root = "/fake/project";
    opts.project_name = "demo";

    const std::string out = build_explanation(idx, opts);

    // Must include the project name and an architecture label.
    EXPECT_NE(out.find("demo"), std::string::npos);
    EXPECT_NE(out.find("confidence"), std::string::npos);
    // Scale line.
    EXPECT_NE(out.find("Scale:"), std::string::npos);
    EXPECT_NE(out.find("2 files"), std::string::npos);
    EXPECT_NE(out.find("4 symbols"), std::string::npos);
    // Languages line names Python.
    EXPECT_NE(out.find("Python"), std::string::npos);
    // API surface counts public + private correctly (2 public, 2 private).
    EXPECT_NE(out.find("API surface:"), std::string::npos);
    EXPECT_NE(out.find("2 public"), std::string::npos);
    EXPECT_NE(out.find("2 private"), std::string::npos);
}

TEST(ExplainTest, IncludesHotspotsForHighComplexity)
{
    CodeIndex idx;
    populate_test_index(idx);
    ExplainOptions opts;
    opts.project_root = "/fake/project";
    opts.project_name = "demo";

    const std::string out = build_explanation(idx, opts);

    // handle_request has complexity 18 — well above the "interesting"
    // threshold. The other symbols have complexity 0 or 1 so they
    // should not appear in the hotspots block.
    EXPECT_NE(out.find("Top hotspots"), std::string::npos);
    EXPECT_NE(out.find("handle_request"), std::string::npos);
    EXPECT_NE(out.find("complexity 18"), std::string::npos);
    EXPECT_NE(out.find("src/app.py:30"), std::string::npos);
}

TEST(ExplainTest, EmptyIndexProducesGracefulOutput)
{
    CodeIndex idx;
    ExplainOptions opts;
    opts.project_root = "/fake/empty";
    opts.project_name = "empty";

    const std::string out = build_explanation(idx, opts);

    // Should still produce *something* coherent — the architecture
    // label will be Unknown but the explanation must not crash or
    // return an empty string.
    EXPECT_FALSE(out.empty());
    EXPECT_NE(out.find("empty"), std::string::npos);
    EXPECT_NE(out.find("Scale:"), std::string::npos);
    // No hotspots block when there are no high-complexity symbols.
    EXPECT_EQ(out.find("Top hotspots"), std::string::npos);
}

} // namespace
