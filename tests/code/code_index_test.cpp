#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;

/// Helper: make a FileEntry with minimal fields populated.
FileEntry make_file(const std::string& relative, Language language)
{
    FileEntry f;
    f.path_relative = relative;
    f.language = language;
    f.size = 0;
    f.line_count = 0;
    return f;
}

/// Helper: make a Symbol bound to a specific file_id.
Symbol make_symbol(std::int64_t file_id, const std::string& name, SymbolKind kind, int line = 1)
{
    Symbol s;
    s.file_id = file_id;
    s.name = name;
    s.kind = kind;
    s.line_start = line;
    s.line_end = line;
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
        make_symbol(fid, "FooBar", SymbolKind::Class),
        make_symbol(fid, "barrier", SymbolKind::Function),
        make_symbol(fid, "BAZ", SymbolKind::Function),
        make_symbol(fid, "initFoo", SymbolKind::Function),
    };
    idx.add_symbols(syms);

    const auto foo_hits = idx.search_symbols("foo");
    EXPECT_EQ(foo_hits.size(), 2U); // FooBar, initFoo

    const auto bar_hits = idx.search_symbols("BAR");
    EXPECT_EQ(bar_hits.size(), 2U); // FooBar, barrier

    const auto nothing = idx.search_symbols("zzz");
    EXPECT_TRUE(nothing.empty());
}

TEST(CodeIndexTest, SearchSymbols_EmptyQueryReturnsAllUpToLimit)
{
    CodeIndex idx;
    const auto fid = idx.add_file(make_file("a.py", Language::Python));
    std::array<Symbol, 3> syms = {
        make_symbol(fid, "one", SymbolKind::Function),
        make_symbol(fid, "two", SymbolKind::Function),
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

    idx.add_dependency(Dependency{fa, fb, "b.h", "include"});
    idx.add_dependency(Dependency{fa, fc, "c.h", "include"});
    idx.add_dependency(Dependency{fb, fc, "c.h", "include"});
    // External — target 0
    idx.add_dependency(Dependency{fa, 0, "<vector>", "include"});

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
    dep.import_string = "b.h";
    dep.kind = "include";
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
    EXPECT_EQ(idx.language_count(), 2U) << "Unknown should not contribute to the language count";
}

TEST(CodeIndexTest, RemoveFile_ClearsSymbolsAndDeps)
{
    CodeIndex idx;
    const auto fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const auto fb = idx.add_file(make_file("b.cpp", Language::Cpp));

    const std::array<Symbol, 2> syms = {
        make_symbol(fa, "func_a", SymbolKind::Function),
        make_symbol(fa, "func_a2", SymbolKind::Function),
    };
    idx.add_symbols(syms);

    const std::array<Symbol, 1> syms_b = {
        make_symbol(fb, "func_b", SymbolKind::Function),
    };
    idx.add_symbols(syms_b);

    Dependency dep;
    dep.source_file_id = fa;
    dep.target_file_id = fb;
    dep.import_string = "b.h";
    dep.kind = "include";
    idx.add_dependency(std::move(dep));

    EXPECT_EQ(idx.file_count(), 2U);
    EXPECT_EQ(idx.symbol_count(), 3U);
    EXPECT_EQ(idx.dependency_count(), 1U);

    idx.remove_file(fa);

    EXPECT_EQ(idx.file_count(), 1U);
    // Symbols from file a should be gone.
    EXPECT_TRUE(idx.symbols_in_file(fa).empty());
    // File b's symbols should remain.
    EXPECT_EQ(idx.symbols_in_file(fb).size(), 1U);
    // Snapshot should only contain file b.
    const auto files = idx.snapshot_files();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(files[0].path_relative, "b.cpp");
    // The dependency from a→b should be gone.
    EXPECT_TRUE(idx.all_dependencies().empty());
}

TEST(CodeIndexTest, SnapshotAllSymbols_ReturnsLiveOnly)
{
    CodeIndex idx;
    const auto fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const auto fb = idx.add_file(make_file("b.cpp", Language::Cpp));

    const std::array<Symbol, 2> a_syms = {
        make_symbol(fa, "a1", SymbolKind::Function),
        make_symbol(fa, "a2", SymbolKind::Function),
    };
    const std::array<Symbol, 1> b_syms = {
        make_symbol(fb, "b1", SymbolKind::Function),
    };
    idx.add_symbols(a_syms);
    idx.add_symbols(b_syms);

    EXPECT_EQ(idx.snapshot_all_symbols().size(), 3U);

    idx.remove_file(fa);

    // After remove_file the soft-deleted entries must not show up.
    const auto live = idx.snapshot_all_symbols();
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live[0].name, "b1");
}

TEST(CodeIndexTest, Compact_RemovesSoftDeletedEntriesAndPreservesLiveLookups)
{
    CodeIndex idx;
    const auto fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const auto fb = idx.add_file(make_file("b.cpp", Language::Cpp));
    const auto fc = idx.add_file(make_file("c.cpp", Language::Cpp));

    const std::array<Symbol, 1> a_syms = {make_symbol(fa, "a1", SymbolKind::Function)};
    const std::array<Symbol, 2> b_syms = {
        make_symbol(fb, "b1", SymbolKind::Function),
        make_symbol(fb, "b2", SymbolKind::Function),
    };
    const std::array<Symbol, 1> c_syms = {make_symbol(fc, "c1", SymbolKind::Function)};
    idx.add_symbols(a_syms);
    idx.add_symbols(b_syms);
    idx.add_symbols(c_syms);

    Dependency dep_ab;
    dep_ab.source_file_id = fa;
    dep_ab.target_file_id = fb;
    dep_ab.kind = "import";
    idx.add_dependency(std::move(dep_ab));

    Dependency dep_bc;
    dep_bc.source_file_id = fb;
    dep_bc.target_file_id = fc;
    dep_bc.kind = "import";
    idx.add_dependency(std::move(dep_bc));

    // Soft-delete the middle file (and its dep edges).
    idx.remove_file(fb);
    EXPECT_EQ(idx.file_count(), 2U);
    EXPECT_EQ(idx.symbol_count(), 2U);
    EXPECT_EQ(idx.dependency_count(), 0U);

    idx.compact();

    // Counters and queries unchanged after compaction.
    EXPECT_EQ(idx.file_count(), 2U);
    EXPECT_EQ(idx.symbol_count(), 2U);
    EXPECT_EQ(idx.dependency_count(), 0U);
    const auto files = idx.snapshot_files();
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].path_relative, "a.cpp");
    EXPECT_EQ(files[1].path_relative, "c.cpp");
    EXPECT_EQ(idx.snapshot_all_symbols().size(), 2U);
    // Per-file lookups must still work — m_by_file was rebuilt.
    EXPECT_EQ(idx.symbols_in_file(fa).size(), 1U);
    EXPECT_EQ(idx.symbols_in_file(fc).size(), 1U);
    EXPECT_TRUE(idx.symbols_in_file(fb).empty());
}

TEST(CodeIndexTest, SnapshotAllSymbols_EmptyIndexReturnsEmpty)
{
    CodeIndex idx;
    EXPECT_TRUE(idx.snapshot_all_symbols().empty());
}

TEST(CodeIndexTest, Compact_NoOpOnEmptyIndex)
{
    CodeIndex idx;
    idx.compact();
    EXPECT_EQ(idx.file_count(), 0U);
    EXPECT_EQ(idx.symbol_count(), 0U);
    EXPECT_EQ(idx.dependency_count(), 0U);
    EXPECT_TRUE(idx.snapshot_files().empty());
    EXPECT_TRUE(idx.snapshot_all_symbols().empty());
}

TEST(CodeIndexTest, Compact_AddFileAfterPreservesIdMonotonicity)
{
    // After compact, m_next_file_id and m_next_symbol_id must keep
    // counting forward — newly-added files should not collide with
    // ids that lived before the compaction.
    CodeIndex idx;
    const auto fa = idx.add_file(make_file("a.cpp", Language::Cpp));
    const auto fb = idx.add_file(make_file("b.cpp", Language::Cpp));
    const auto fc = idx.add_file(make_file("c.cpp", Language::Cpp));

    const std::array<Symbol, 1> a_syms = {make_symbol(fa, "a1", SymbolKind::Function)};
    const std::array<Symbol, 1> b_syms = {make_symbol(fb, "b1", SymbolKind::Function)};
    const std::array<Symbol, 1> c_syms = {make_symbol(fc, "c1", SymbolKind::Function)};
    idx.add_symbols(a_syms);
    idx.add_symbols(b_syms);
    idx.add_symbols(c_syms);

    idx.remove_file(fb);
    idx.compact();

    const auto fd = idx.add_file(make_file("d.cpp", Language::Cpp));
    EXPECT_GT(fd, fc) << "post-compact ids must keep climbing past the highest pre-compact id";

    const std::array<Symbol, 1> d_syms = {make_symbol(fd, "d1", SymbolKind::Function)};
    idx.add_symbols(d_syms);

    // The new file's symbol resolves; the surviving original files still resolve.
    ASSERT_EQ(idx.symbols_in_file(fd).size(), 1U);
    EXPECT_EQ(idx.symbols_in_file(fd)[0].name, "d1");
    EXPECT_EQ(idx.symbols_in_file(fa).size(), 1U);
    EXPECT_EQ(idx.symbols_in_file(fc).size(), 1U);
    EXPECT_EQ(idx.file_count(), 3U);
}

} // namespace
