#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "code/gitignore.h"

namespace {

namespace fs = std::filesystem;
using vectis::code::GitignorePatterns;
using vectis::code::read_gitignore_dir_patterns;
using vectis::code::wildcard_match;

/// Write `content` to `<dir>/.gitignore`.
void write_gitignore(const fs::path& dir, std::string_view content)
{
    std::ofstream out(dir / ".gitignore");
    out << content;
}

/// True if any glob in `patterns.glob_patterns` is exactly `pattern`.
bool has_glob(const GitignorePatterns& patterns, std::string_view pattern)
{
    for (const std::string& g : patterns.glob_patterns) {
        if (g == pattern) {
            return true;
        }
    }
    return false;
}

class GitignoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Use a unique temp dir per test so parallel runs don't collide.
        const auto base = fs::temp_directory_path();
        m_dir = base / fs::path{"vectis_gitignore_test_" +
                                std::to_string(reinterpret_cast<std::uintptr_t>(this))};
        fs::create_directories(m_dir);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    fs::path m_dir;
};

TEST_F(GitignoreTest, ReturnsEmptyWhenFileMissing)
{
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.exact_names.empty());
    EXPECT_TRUE(patterns.glob_patterns.empty());
}

TEST_F(GitignoreTest, ReturnsEmptyWhenFileEmpty)
{
    write_gitignore(m_dir, "");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.exact_names.empty());
    EXPECT_TRUE(patterns.glob_patterns.empty());
}

TEST_F(GitignoreTest, TrailingSlashDirectoryPattern)
{
    write_gitignore(m_dir, "build/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count("build"));
    EXPECT_TRUE(patterns.glob_patterns.empty());
}

TEST_F(GitignoreTest, BareNameWithoutSlash)
{
    write_gitignore(m_dir, ".venv\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count(".venv"));
}

TEST_F(GitignoreTest, LeadingSlashStripped)
{
    write_gitignore(m_dir, "/target/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count("target"));
}

TEST_F(GitignoreTest, CommentsSkipped)
{
    write_gitignore(m_dir, "# a comment\nbuild/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_FALSE(patterns.exact_names.count("# a comment"));
    EXPECT_TRUE(patterns.exact_names.count("build"));
}

TEST_F(GitignoreTest, EmptyLinesSkipped)
{
    write_gitignore(m_dir, "\n\nbuild/\n\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count("build"));
}

TEST_F(GitignoreTest, NegationSkipped)
{
    write_gitignore(m_dir, "!keep/\nbuild/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_FALSE(patterns.exact_names.count("keep"));
    EXPECT_TRUE(patterns.exact_names.count("build"));
    EXPECT_TRUE(patterns.glob_patterns.empty());
}

TEST_F(GitignoreTest, GlobsAreCapturedAsPatterns)
{
    write_gitignore(m_dir, "build-*/\ncmake-build-?/\n*.egg-info\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.exact_names.empty());
    EXPECT_EQ(patterns.glob_patterns.size(), 3U);
    EXPECT_TRUE(has_glob(patterns, "build-*"));
    EXPECT_TRUE(has_glob(patterns, "cmake-build-?"));
    EXPECT_TRUE(has_glob(patterns, "*.egg-info"));
}

TEST_F(GitignoreTest, BracketExpressionsStillSkipped)
{
    // `[abc]` is not supported by wildcard_match; we drop the pattern
    // rather than risk a wrong match.
    write_gitignore(m_dir, "build[12]/\nplain/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count("plain"));
    EXPECT_TRUE(patterns.glob_patterns.empty());
}

TEST_F(GitignoreTest, EmbeddedSlashPathPatternSkipped)
{
    // "docs/build/" would need two-level matching; we deliberately drop it.
    write_gitignore(m_dir, "docs/build/\nplain/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 1U);
    EXPECT_TRUE(patterns.exact_names.count("plain"));
}

TEST_F(GitignoreTest, CRLFLineEndingsHandled)
{
    write_gitignore(m_dir, "build/\r\n.venv\r\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 2U);
    EXPECT_TRUE(patterns.exact_names.count("build"));
    EXPECT_TRUE(patterns.exact_names.count(".venv"));
}

TEST_F(GitignoreTest, LeadingTrailingWhitespaceTrimmed)
{
    write_gitignore(m_dir, "  build/  \n\t.venv\t\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.exact_names.size(), 2U);
    EXPECT_TRUE(patterns.exact_names.count("build"));
    EXPECT_TRUE(patterns.exact_names.count(".venv"));
}

TEST_F(GitignoreTest, RealWorldMix)
{
    write_gitignore(m_dir, R"(# Python
__pycache__/
*.py[cod]
build_venv/
.venv
.pytest_cache/

# Build
/target/
build/
build-*/
dist/

# Wildcards we can handle
*.egg-info/
!src/should_not_match_negation/
docs/build/
)");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.exact_names.count("__pycache__"));
    EXPECT_TRUE(patterns.exact_names.count("build_venv"));
    EXPECT_TRUE(patterns.exact_names.count(".venv"));
    EXPECT_TRUE(patterns.exact_names.count(".pytest_cache"));
    EXPECT_TRUE(patterns.exact_names.count("target"));
    EXPECT_TRUE(patterns.exact_names.count("build"));
    EXPECT_TRUE(patterns.exact_names.count("dist"));
    EXPECT_TRUE(has_glob(patterns, "build-*"));
    EXPECT_TRUE(has_glob(patterns, "*.egg-info"));
    // Bracket expression `*.py[cod]` still drops because we don't
    // support `[..]`. Negation `!src/...` and path-prefix `docs/build/`
    // also drop.
    EXPECT_FALSE(has_glob(patterns, "*.py[cod]"));
    EXPECT_FALSE(patterns.exact_names.count("should_not_match_negation"));
}

TEST(WildcardMatch, ExactStringWithoutGlob)
{
    EXPECT_TRUE(wildcard_match("build", "build"));
    EXPECT_FALSE(wildcard_match("build", "build-x"));
    EXPECT_FALSE(wildcard_match("build", "buil"));
}

TEST(WildcardMatch, StarMatchesArbitraryRunIncludingEmpty)
{
    EXPECT_TRUE(wildcard_match("build-*", "build-"));
    EXPECT_TRUE(wildcard_match("build-*", "build-release"));
    EXPECT_TRUE(wildcard_match("build-*", "build-debug-extra"));
    EXPECT_FALSE(wildcard_match("build-*", "build"));
    EXPECT_FALSE(wildcard_match("build-*", "tools-debug"));
}

TEST(WildcardMatch, QuestionMarkIsExactlyOneChar)
{
    EXPECT_TRUE(wildcard_match("build-?", "build-1"));
    EXPECT_TRUE(wildcard_match("build-?", "build-x"));
    EXPECT_FALSE(wildcard_match("build-?", "build-12"));
    EXPECT_FALSE(wildcard_match("build-?", "build-"));
}

TEST(WildcardMatch, SuffixGlobs)
{
    EXPECT_TRUE(wildcard_match("*.egg-info", "foo.egg-info"));
    EXPECT_TRUE(wildcard_match("*.egg-info", ".egg-info")); // empty prefix is fine
    EXPECT_FALSE(wildcard_match("*.egg-info", "foo.egg-information"));
}

TEST(WildcardMatch, MultipleStarsAndBacktracking)
{
    EXPECT_TRUE(wildcard_match("a*b*c", "abc"));
    EXPECT_TRUE(wildcard_match("a*b*c", "axxbyyc"));
    EXPECT_TRUE(wildcard_match("a*b*c", "ababc"));
    EXPECT_FALSE(wildcard_match("a*b*c", "abca")); // no trailing c — actually false
    EXPECT_FALSE(wildcard_match("a*b*c", "ab"));
}

TEST(WildcardMatch, EmptyName)
{
    EXPECT_TRUE(wildcard_match("", ""));
    EXPECT_TRUE(wildcard_match("*", ""));
    EXPECT_TRUE(wildcard_match("***", ""));
    EXPECT_FALSE(wildcard_match("a", ""));
    EXPECT_FALSE(wildcard_match("a*", ""));
}

TEST(WildcardMatch, EmptyPattern)
{
    EXPECT_TRUE(wildcard_match("", ""));
    EXPECT_FALSE(wildcard_match("", "x"));
}

} // namespace
