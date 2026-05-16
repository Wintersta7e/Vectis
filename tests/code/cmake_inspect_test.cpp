#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <unistd.h>

#include "code/cmake_inspect.h"

namespace {

using ::vectis::code::cmake::inspect_root;
using ::vectis::code::cmake::RootTargetShape;

class CMakeInspectFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<std::uint64_t> counter{0};
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        m_root =
            std::filesystem::temp_directory_path() /
            ("vectis-cmake-inspect-test-" + std::to_string(::getpid()) + "-" + std::to_string(seq));
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    void TearDown() override { std::filesystem::remove_all(m_root); }

    void write_root_cmake(std::string_view contents) const
    {
        std::ofstream(m_root / "CMakeLists.txt") << contents;
    }

    std::filesystem::path m_root;
};

TEST_F(CMakeInspectFixture, LibraryOnlyTarget)
{
    write_root_cmake("add_library(foo src/foo.cpp src/bar.cpp)\n");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

TEST_F(CMakeInspectFixture, ExecutableOnlyTarget)
{
    write_root_cmake("add_executable(app src/main.cpp)\n");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::ExecutableOnly);
}

TEST_F(CMakeInspectFixture, MixedTargets)
{
    write_root_cmake(R"(add_library(foo src/foo.cpp)
add_executable(foo_cli src/main.cpp)
)");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::Mixed);
}

TEST_F(CMakeInspectFixture, InterfaceLibraryCountsAsLibrary)
{
    // Header-only INTERFACE libraries are real targets, just with no
    // compiled sources. The detector should treat them as libraries.
    write_root_cmake("add_library(foo INTERFACE)\n");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

TEST_F(CMakeInspectFixture, AliasLibraryDoesNotCount)
{
    // `add_library(NAME ALIAS ...)` references an existing target, it
    // doesn't define a new one. Counted alone, it's not a library
    // declaration.
    write_root_cmake("add_library(my::foo ALIAS foo)\n");
    EXPECT_FALSE(inspect_root(m_root).has_value());
}

TEST_F(CMakeInspectFixture, ImportedLibraryDoesNotCount)
{
    // IMPORTED libraries reference external artifacts; the project
    // doesn't build them.
    write_root_cmake("add_library(zlib IMPORTED)\n");
    EXPECT_FALSE(inspect_root(m_root).has_value());
}

TEST_F(CMakeInspectFixture, LibraryThenAliasStillFiresLibraryOnly)
{
    // Real library + alias to it should still classify as LibraryOnly.
    write_root_cmake(R"(add_library(foo src/foo.cpp)
add_library(my::foo ALIAS foo)
)");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

TEST_F(CMakeInspectFixture, CommentedOutCallIsIgnored)
{
    write_root_cmake(R"(# add_executable(legacy_cli src/legacy.cpp)
add_library(foo src/foo.cpp)
)");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

TEST_F(CMakeInspectFixture, HashInsideStringIsNotAComment)
{
    // Quoted `#` characters must NOT be treated as comments.
    write_root_cmake(R"(set(BANNER "version #1.0")
add_library(foo src/foo.cpp)
)");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

TEST_F(CMakeInspectFixture, IdentifierPrefixDoesNotMatch)
{
    // `qmake_add_library` and similar prefixed identifiers must not
    // count as `add_library` calls.
    write_root_cmake(R"(qmake_add_library(legacy SHARED)
my_add_executable(legacy_cli)
)");
    EXPECT_FALSE(inspect_root(m_root).has_value());
}

TEST_F(CMakeInspectFixture, MissingCMakeListsReturnsNullopt)
{
    EXPECT_FALSE(inspect_root(m_root).has_value());
}

TEST_F(CMakeInspectFixture, EmptyCMakeListsReturnsNullopt)
{
    write_root_cmake("# empty project shell\n");
    EXPECT_FALSE(inspect_root(m_root).has_value());
}

TEST_F(CMakeInspectFixture, WhitespaceBeforeParen)
{
    // CMake accepts `add_library (foo ...)` with whitespace before
    // the open-paren. The parser should tolerate it.
    write_root_cmake("add_library  (foo src/foo.cpp)\n");
    const auto kind = inspect_root(m_root);
    ASSERT_TRUE(kind.has_value());
    EXPECT_EQ(*kind, RootTargetShape::LibraryOnly);
}

} // namespace
