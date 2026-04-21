#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "modes/code/gitignore.h"

namespace {

namespace fs = std::filesystem;
using vectis::modes::code::read_gitignore_dir_patterns;

/// Write `content` to `<dir>/.gitignore`.
void write_gitignore(const fs::path& dir, std::string_view content)
{
    std::ofstream out(dir / ".gitignore");
    out << content;
}

class GitignoreTest : public ::testing::Test {
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
    EXPECT_TRUE(patterns.empty());
}

TEST_F(GitignoreTest, ReturnsEmptyWhenFileEmpty)
{
    write_gitignore(m_dir, "");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.empty());
}

TEST_F(GitignoreTest, TrailingSlashDirectoryPattern)
{
    write_gitignore(m_dir, "build/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count("build"));
}

TEST_F(GitignoreTest, BareNameWithoutSlash)
{
    write_gitignore(m_dir, ".venv\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count(".venv"));
}

TEST_F(GitignoreTest, LeadingSlashStripped)
{
    write_gitignore(m_dir, "/target/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count("target"));
}

TEST_F(GitignoreTest, CommentsSkipped)
{
    write_gitignore(m_dir, "# a comment\nbuild/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_FALSE(patterns.count("# a comment"));
    EXPECT_TRUE(patterns.count("build"));
}

TEST_F(GitignoreTest, EmptyLinesSkipped)
{
    write_gitignore(m_dir, "\n\nbuild/\n\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count("build"));
}

TEST_F(GitignoreTest, NegationSkipped)
{
    write_gitignore(m_dir, "!keep/\nbuild/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_FALSE(patterns.count("keep"));
    EXPECT_TRUE(patterns.count("build"));
}

TEST_F(GitignoreTest, WildcardsSkipped)
{
    write_gitignore(m_dir, "*.egg-info/\nbuild/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count("build"));
}

TEST_F(GitignoreTest, EmbeddedSlashPathPatternSkipped)
{
    // "docs/build/" would need two-level matching; we deliberately drop it.
    write_gitignore(m_dir, "docs/build/\nplain/\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 1U);
    EXPECT_TRUE(patterns.count("plain"));
}

TEST_F(GitignoreTest, CRLFLineEndingsHandled)
{
    write_gitignore(m_dir, "build/\r\n.venv\r\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 2U);
    EXPECT_TRUE(patterns.count("build"));
    EXPECT_TRUE(patterns.count(".venv"));
}

TEST_F(GitignoreTest, LeadingTrailingWhitespaceTrimmed)
{
    write_gitignore(m_dir, "  build/  \n\t.venv\t\n");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_EQ(patterns.size(), 2U);
    EXPECT_TRUE(patterns.count("build"));
    EXPECT_TRUE(patterns.count(".venv"));
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
dist/

# Wildcards we can't handle
*.egg-info/
!src/should_not_match_negation/
docs/build/
)");
    const auto patterns = read_gitignore_dir_patterns(m_dir);
    EXPECT_TRUE(patterns.count("__pycache__"));
    EXPECT_TRUE(patterns.count("build_venv"));
    EXPECT_TRUE(patterns.count(".venv"));
    EXPECT_TRUE(patterns.count(".pytest_cache"));
    EXPECT_TRUE(patterns.count("target"));
    EXPECT_TRUE(patterns.count("build"));
    EXPECT_TRUE(patterns.count("dist"));
    EXPECT_FALSE(patterns.count("*.egg-info"));
    EXPECT_FALSE(patterns.count("should_not_match_negation"));
    // Path-pattern "docs/build/" is skipped — top-level "build/" still contributes "build".
}

} // namespace
