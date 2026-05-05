#include <algorithm>
#include <cstdint>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/pagerank.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::compute_pagerank;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::PageRankOptions;

/// Helper: add a file with `path` and return its id.
std::int64_t add_file(CodeIndex& index, const char* path)
{
    FileEntry f;
    f.path_relative = path;
    f.language = Language::Cpp;
    f.size = 100;
    f.line_count = 10;
    return index.add_file(std::move(f));
}

void add_dep(CodeIndex& index, std::int64_t src, std::int64_t tgt)
{
    Dependency d{};
    d.source_file_id = src;
    d.target_file_id = tgt;
    d.kind = "import";
    d.import_string = "x";
    index.add_dependency(d);
}

TEST(PageRankTest, EmptyIndexReturnsEmpty)
{
    CodeIndex index;
    const auto result = compute_pagerank(index);
    EXPECT_TRUE(result.empty());
}

TEST(PageRankTest, IsolatedNodesShareRankUniformly)
{
    CodeIndex index;
    add_file(index, "a.cpp");
    add_file(index, "b.cpp");
    add_file(index, "c.cpp");

    const auto result = compute_pagerank(index);
    ASSERT_EQ(result.size(), 3U);
    // No edges → all dangling → distribution stays uniform at 1/N.
    for (const auto& r : result) {
        EXPECT_NEAR(r.score, 1.0 / 3.0, 1e-6);
    }
}

TEST(PageRankTest, HubGetsHighestRank)
{
    // a → hub, b → hub, c → hub. The hub is imported three times and
    // imports nothing; everyone else votes for it.
    CodeIndex index;
    const auto a = add_file(index, "a.cpp");
    const auto b = add_file(index, "b.cpp");
    const auto c = add_file(index, "c.cpp");
    const auto hub = add_file(index, "hub.cpp");
    add_dep(index, a, hub);
    add_dep(index, b, hub);
    add_dep(index, c, hub);

    const auto result = compute_pagerank(index);
    ASSERT_EQ(result.size(), 4U);
    EXPECT_EQ(result.front().file_id, hub) << "hub should rank #1";
    // hub's score should exceed every importer's by a clear margin.
    for (const auto& r : result) {
        if (r.file_id == hub) {
            continue;
        }
        EXPECT_LT(r.score, result.front().score);
    }
}

TEST(PageRankTest, ScoresSumToApproximatelyOne)
{
    CodeIndex index;
    const auto a = add_file(index, "a.cpp");
    const auto b = add_file(index, "b.cpp");
    const auto c = add_file(index, "c.cpp");
    add_dep(index, a, b);
    add_dep(index, b, c);
    add_dep(index, c, a);

    const auto result = compute_pagerank(index);
    double total = 0.0;
    for (const auto& r : result) {
        total += r.score;
    }
    EXPECT_NEAR(total, 1.0, 1e-3);
}

TEST(PageRankTest, ParallelEdgesDoNotInflateScores)
{
    // Adding a dependency twice (e.g. via different import strings)
    // should not let the importer give a double vote.
    CodeIndex left;
    CodeIndex right;
    const auto la = add_file(left, "a.cpp");
    const auto lb = add_file(left, "b.cpp");
    add_dep(left, la, lb);

    const auto ra = add_file(right, "a.cpp");
    const auto rb = add_file(right, "b.cpp");
    add_dep(right, ra, rb);
    add_dep(right, ra, rb); // duplicate

    const auto left_result = compute_pagerank(left);
    const auto right_result = compute_pagerank(right);
    ASSERT_EQ(left_result.size(), 2U);
    ASSERT_EQ(right_result.size(), 2U);
    // Same graph after de-dup → identical scores in matching slots.
    EXPECT_EQ(left_result[0].file_id, right_result[0].file_id);
    EXPECT_NEAR(left_result[0].score, right_result[0].score, 1e-9);
    EXPECT_NEAR(left_result[1].score, right_result[1].score, 1e-9);
}

TEST(PageRankTest, SelfLoopsAreIgnored)
{
    CodeIndex index;
    const auto a = add_file(index, "a.cpp");
    const auto b = add_file(index, "b.cpp");
    add_dep(index, a, a); // self-loop, dropped
    add_dep(index, a, b);

    const auto result = compute_pagerank(index);
    ASSERT_EQ(result.size(), 2U);
    // b is imported by a, a imports nothing else → b ranks higher.
    EXPECT_EQ(result.front().file_id, b);
}

TEST(PageRankTest, OutputSortedByScoreDescTiesByFileIdAsc)
{
    // Three identical leaves so ranks tie; tie-break uses file_id.
    CodeIndex index;
    const auto a = add_file(index, "a.cpp");
    const auto b = add_file(index, "b.cpp");
    const auto c = add_file(index, "c.cpp");

    const auto result = compute_pagerank(index);
    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result[0].file_id, a);
    EXPECT_EQ(result[1].file_id, b);
    EXPECT_EQ(result[2].file_id, c);
}

} // namespace
