#include "modes/code/code_index.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/code/dependency.h"
#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::CodeIndex;
using vectis::modes::code::Dependency;
using vectis::modes::code::FileEntry;
using vectis::modes::code::Language;
using vectis::modes::code::Symbol;
using vectis::modes::code::SymbolKind;

/// Helper: make a FileEntry with minimal fields populated.
FileEntry make_file(const std::string& relative, Language language)
{
    FileEntry f;
    f.path_relative = relative;
    f.language      = language;
    f.size          = 0;
    f.line_count    = 0;
    return f;
}

/// Helper: make a Symbol bound to a specific file_id.
Symbol make_symbol(std::int64_t file_id, const std::string& name, SymbolKind kind, int line = 1)
{
    Symbol s;
    s.file_id    = file_id;
    s.name       = name;
    s.kind       = kind;
    s.line_start = line;
    s.line_end   = line;
    return s;
}

TEST(CodeIndexTest, AddFile_AssignsMonotonicIds)
{
    CodeIndex idx;
    const auto id1 = idx.add_file(make_file("a.py", Language::Python));
    const auto id2 = idx.add_file(make_file("b.ts", Language::TypeScript));
    const auto id3 = idx.add_file(make_file("c.cpp", Language::Cpp));
    EXPECT_EQ(id1, 1);
    EXPECT_EQ(id2, 2);
    EXPECT_EQ(id3, 3);
    EXPECT_EQ(idx.file_count(), 3U);
    EXPECT_EQ(idx.language_count(), 3U);
}

TEST(CodeIndexTest, SnapshotFiles_SortedByPath)
{
    CodeIndex idx;
    idx.add_file(make_file("zeta.py", Language::Python));
    idx.add_file(make_file("alpha.py", Language::Python));
    idx.add_file(make_file("beta.py", Language::Python));

    const auto files = idx.snapshot_files();
    ASSERT_EQ(files.size(), 3U);
    EXPECT_EQ(files[0].path_relative.string(), "alpha.py");
    EXPECT_EQ(files[1].path_relative.string(), "beta.py");
    EXPECT_EQ(files[2].path_relative.string(), "zeta.py");
}

TEST(CodeIndexTest, AddSymbols_GroupsByFile)
{
    CodeIndex idx;
    const auto f1 = idx.add_file(make_file("one.py", Language::Python));
    const auto f2 = idx.add_file(make_file("two.py", Language::Python));

    const std::array<Symbol, 3> batch1 = {
        make_symbol(f1, "foo", SymbolKind::Function, 10),
        make_symbol(f1, "Bar", SymbolKind::Class, 20),
        make_symbol(f1, "baz", SymbolKind::Function, 30),
    };
    idx.add_symbols(batch1);

    const std::array<Symbol, 1> batch2 = {
        make_symbol(f2, "Qux", SymbolKind::Class, 1),
    };
    idx.add_symbols(batch2);

    EXPECT_EQ(idx.symbol_count(), 4U);
    const auto one_syms = idx.symbols_in_file(f1);
    const auto two_syms = idx.symbols_in_file(f2);
    EXPECT_EQ(one_syms.size(), 3U);
    EXPECT_EQ(two_syms.size(), 1U);
    EXPECT_EQ(two_syms[0].name, "Qux");
}

TEST(CodeIndexTest, SearchSymbols_CaseInsensitiveSubstring)
{
    CodeIndex idx;
    const auto fid = idx.add_file(make_file("mod.py", Language::Python));
    const std::array<Symbol, 4> syms = {
        make_symbol(fid, "FooBar",   SymbolKind::Class),
        make_symbol(fid, "barrier",  SymbolKind::Function),
        make_symbol(fid, "BAZ",      SymbolKind::Function),
        make_symbol(fid, "initFoo",  SymbolKind::Function),
    };
    idx.add_symbols(syms);

    const auto foo_hits = idx.search_symbols("foo");
    EXPECT_EQ(foo_hits.size(), 2U);  // FooBar, initFoo

    const auto bar_hits = idx.search_symbols("BAR");
    EXPECT_EQ(bar_hits.size(), 2U);  // FooBar, barrier

    const auto nothing = idx.search_symbols("zzz");
    EXPECT_TRUE(nothing.empty());
}

TEST(CodeIndexTest, SearchSymbols_EmptyQueryReturnsAllUpToLimit)
{
    CodeIndex idx;
    const auto fid = idx.add_file(make_file("a.py", Language::Python));
    std::array<Symbol, 3> syms = {
        make_symbol(fid, "one",   SymbolKind::Function),
        make_symbol(fid, "two",   SymbolKind::Function),
        make_symbol(fid, "three", SymbolKind::Function),
    };
    idx.add_symbols(syms);

    const auto all = idx.search_symbols("");
    EXPECT_EQ(all.size(), 3U);
}

TEST(CodeIndexTest, Clear_ResetsEverything)
{
    CodeIndex idx;
    idx.add_file(make_file("a.py", Language::Python));
    const std::array<Symbol, 1> batch = {
        make_symbol(1, "fn", SymbolKind::Function),
    };
    idx.add_symbols(batch);

    idx.clear();
    EXPECT_EQ(idx.file_count(), 0U);
    EXPECT_EQ(idx.symbol_count(), 0U);
    EXPECT_EQ(idx.language_count(), 0U);
    EXPECT_TRUE(idx.snapshot_files().empty());

    // After clear, new ids start from 1 again.
    const auto fid = idx.add_file(make_file("fresh.py", Language::Python));
    EXPECT_EQ(fid, 1);
}

TEST(CodeIndexTest, Dependencies_RoundTripOutgoingAndIncoming)
{
    CodeIndex idx;
    const auto fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const auto fb = idx.add_file(make_file("b.cpp", Language::Cpp));
    const auto fc = idx.add_file(make_file("c.cpp", Language::Cpp));

    idx.add_dependency(Dependency{fa, fb, "b.h",        "include"});
    idx.add_dependency(Dependency{fa, fc, "c.h",        "include"});
    idx.add_dependency(Dependency{fb, fc, "c.h",        "include"});
    // External — target 0
    idx.add_dependency(Dependency{fa, 0,  "<vector>",   "include"});

    EXPECT_EQ(idx.dependency_count(), 4U);

    // fa depends on fb, fc, and one external.
    const auto fa_deps = idx.dependencies_of(fa);
    EXPECT_EQ(fa_deps.size(), 3U);

    // fb depends on fc only.
    const auto fb_deps = idx.dependencies_of(fb);
    ASSERT_EQ(fb_deps.size(), 1U);
    EXPECT_EQ(fb_deps[0].target_file_id, fc);

    // fc has no outgoing deps.
    EXPECT_TRUE(idx.dependencies_of(fc).empty());

    // fc has two incoming (fa and fb).
    const auto fc_dependents = idx.dependents_of(fc);
    EXPECT_EQ(fc_dependents.size(), 2U);

    // fb has one incoming (fa).
    const auto fb_dependents = idx.dependents_of(fb);
    ASSERT_EQ(fb_dependents.size(), 1U);
    EXPECT_EQ(fb_dependents[0].source_file_id, fa);

    // External dep does NOT appear in any dependents_of (target is 0).
    EXPECT_TRUE(idx.dependents_of(0).empty());
}

TEST(CodeIndexTest, Clear_AlsoClearsDependencies)
{
    CodeIndex idx;
    const std::int64_t fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const std::int64_t fb = idx.add_file(make_file("b.cpp", Language::Cpp));
    Dependency dep;
    dep.source_file_id = fa;
    dep.target_file_id = fb;
    dep.import_string  = "b.h";
    dep.kind           = "include";
    idx.add_dependency(std::move(dep));
    EXPECT_EQ(idx.dependency_count(), 1U);

    idx.clear();
    EXPECT_EQ(idx.dependency_count(), 0U);
    EXPECT_TRUE(idx.all_dependencies().empty());
}

TEST(CodeIndexTest, LanguageCount_CountsDistinctLanguages)
{
    CodeIndex idx;
    idx.add_file(make_file("a.py", Language::Python));
    idx.add_file(make_file("b.py", Language::Python));
    EXPECT_EQ(idx.language_count(), 1U);

    idx.add_file(make_file("c.ts", Language::TypeScript));
    EXPECT_EQ(idx.language_count(), 2U);

    idx.add_file(make_file("README", Language::Unknown));
    EXPECT_EQ(idx.language_count(), 2U)
        << "Unknown should not contribute to the language count";
}

} // namespace
