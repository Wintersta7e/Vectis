#include "platform/file_io.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "core/result.h"

namespace {

using vectis::core::ErrorKind;
using vectis::platform::default_data_dir;
using vectis::platform::ensure_dir;
using vectis::platform::executable_dir;
using vectis::platform::executable_path;
using vectis::platform::read_file;
using vectis::platform::write_file;

TEST(FileIoTest, ExecutablePath_NonEmpty)
{
    const auto result = executable_path();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->empty());
    EXPECT_TRUE(result->is_absolute());
    EXPECT_TRUE(std::filesystem::exists(*result));
}

TEST(FileIoTest, ExecutableDir_ContainsBinary)
{
    const auto exe = executable_path();
    ASSERT_TRUE(exe.has_value());

    const auto dir = executable_dir();
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(*dir, exe->parent_path());
}

TEST(FileIoTest, DefaultDataDir_NextToExe)
{
    const auto dir      = executable_dir();
    const auto data_dir = default_data_dir();
    ASSERT_TRUE(dir.has_value());
    ASSERT_TRUE(data_dir.has_value());
    EXPECT_EQ(data_dir->parent_path(), *dir);
    EXPECT_EQ(data_dir->filename(), "vectis-data");
}

TEST(FileIoTest, EnsureDir_Idempotent)
{
    const auto target =
        std::filesystem::temp_directory_path() / "vectis_ensure_dir_test" / "nested" / "path";
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "vectis_ensure_dir_test");

    ASSERT_TRUE(ensure_dir(target).has_value());
    EXPECT_TRUE(std::filesystem::is_directory(target));

    // Calling again must succeed without error.
    ASSERT_TRUE(ensure_dir(target).has_value());
    EXPECT_TRUE(std::filesystem::is_directory(target));

    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "vectis_ensure_dir_test");
}

TEST(FileIoTest, ReadWrite_RoundTrip)
{
    const auto temp_path =
        std::filesystem::temp_directory_path() / "vectis_read_write_roundtrip.txt";
    std::filesystem::remove(temp_path);

    constexpr std::string_view payload = "line one\nline two\nfinal line";

    ASSERT_TRUE(write_file(temp_path, payload).has_value());
    const auto read_result = read_file(temp_path);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, std::string{payload});

    std::filesystem::remove(temp_path);
}

TEST(FileIoTest, ReadFile_MissingReturnsError)
{
    const auto missing = std::filesystem::temp_directory_path() /
                         "vectis_file_that_definitely_does_not_exist_42.bin";
    std::filesystem::remove(missing);

    const auto result = read_file(missing);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::IoError);
}

} // namespace
