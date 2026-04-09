#include "modes/code/dependency_graph.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/code/code_index.h"
#include "modes/code/dependency.h"
#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::CodeIndex;
using vectis::modes::code::Dependency;
using vectis::modes::code::DependencyCycle;
using vectis::modes::code::detect_cycles;
using vectis::modes::code::FileEntry;
using vectis::modes::code::Language;

std::int64_t add_file(CodeIndex& idx, const std::string& path)
{
    FileEntry f;
    f.path_relative = path;
    f.language      = Language::Cpp;
    return idx.add_file(std::move(f));
}

void add_dep(CodeIndex& idx, std::int64_t src, std::int64_t dst)
{
    Dependency d;
    d.source_file_id = src;
    d.target_file_id = dst;
    d.kind           = "include";
    idx.add_dependency(std::move(d));
}

/// True if `cycle` contains every id in `expected`, in any order.
bool cycle_contains_all(const DependencyCycle& cycle,
                        const std::vector<std::int64_t>& expected)
{
    if (cycle.file_ids.size() != expected.size()) {
        return false;
    }
    for (const std::int64_t id : expected) {
        if (std::find(cycle.file_ids.begin(), cycle.file_ids.end(), id) ==
            cycle.file_ids.end()) {
            return false;
        }
    }
    return true;
}

TEST(DependencyGraphTest, LinearGraph_NoCycles)
{
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    const auto b = add_file(idx, "b.cpp");
    const auto c = add_file(idx, "c.cpp");
    add_dep(idx, a, b);
    add_dep(idx, b, c);

    EXPECT_TRUE(detect_cycles(idx).empty());
}

TEST(DependencyGraphTest, DiamondShape_NoCycles)
{
    // a -> b, a -> c, b -> d, c -> d
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    const auto b = add_file(idx, "b.cpp");
    const auto c = add_file(idx, "c.cpp");
    const auto d = add_file(idx, "d.cpp");
    add_dep(idx, a, b);
    add_dep(idx, a, c);
    add_dep(idx, b, d);
    add_dep(idx, c, d);

    EXPECT_TRUE(detect_cycles(idx).empty());
}

TEST(DependencyGraphTest, TwoNodeCycle_Detected)
{
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    const auto b = add_file(idx, "b.cpp");
    add_dep(idx, a, b);
    add_dep(idx, b, a);

    const auto cycles = detect_cycles(idx);
    ASSERT_EQ(cycles.size(), 1U);
    EXPECT_TRUE(cycle_contains_all(cycles[0], {a, b}));
}

TEST(DependencyGraphTest, ThreeNodeCycle_Detected)
{
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    const auto b = add_file(idx, "b.cpp");
    const auto c = add_file(idx, "c.cpp");
    add_dep(idx, a, b);
    add_dep(idx, b, c);
    add_dep(idx, c, a);

    const auto cycles = detect_cycles(idx);
    ASSERT_EQ(cycles.size(), 1U);
    EXPECT_TRUE(cycle_contains_all(cycles[0], {a, b, c}));
}

TEST(DependencyGraphTest, SelfLoop_DetectedAsCycle)
{
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    add_dep(idx, a, a);

    const auto cycles = detect_cycles(idx);
    ASSERT_EQ(cycles.size(), 1U);
    ASSERT_EQ(cycles[0].file_ids.size(), 1U);
    EXPECT_EQ(cycles[0].file_ids[0], a);
}

TEST(DependencyGraphTest, IndependentCycles_DetectedSeparately)
{
    // {a <-> b} and {c <-> d} are two disjoint cycles.
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    const auto b = add_file(idx, "b.cpp");
    const auto c = add_file(idx, "c.cpp");
    const auto d = add_file(idx, "d.cpp");
    add_dep(idx, a, b);
    add_dep(idx, b, a);
    add_dep(idx, c, d);
    add_dep(idx, d, c);

    const auto cycles = detect_cycles(idx);
    EXPECT_EQ(cycles.size(), 2U);
}

TEST(DependencyGraphTest, ExternalEdges_Ignored)
{
    // target 0 means external — shouldn't form a cycle even if the
    // same source appears in multiple external deps.
    CodeIndex idx;
    const auto a = add_file(idx, "a.cpp");
    add_dep(idx, a, 0);
    add_dep(idx, a, 0);

    EXPECT_TRUE(detect_cycles(idx).empty());
}

} // namespace
