#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/config_manager.h"
#include "core/result.h"

namespace {

using vectis::core::ConfigManager;
using vectis::core::ErrorKind;

/// Fixture that gives each test a unique temp file path it owns
/// and cleans up after itself, exercising the real `toml::parse_file`
/// path rather than in-memory parsing.
class ConfigManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_path = std::filesystem::temp_directory_path() /
                 (std::string{"vectis_cfg_"} + test_name + ".toml");
        std::filesystem::remove(m_path);
    }

    void TearDown() override { std::filesystem::remove(m_path); }

    void write(std::string_view contents) const
    {
        std::ofstream stream(m_path);
        stream << contents;
    }

    std::filesystem::path m_path;
};

TEST_F(ConfigManagerTest, Defaults_WhenNoFile)
{
    ConfigManager cfg;
    // Path intentionally does not exist — missing file is a soft fallback.
    const auto result = cfg.load(m_path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(cfg.loaded_from_file());
    EXPECT_EQ(cfg.get_string("general.theme", "dark"), "dark");
    EXPECT_EQ(cfg.get_int("http.proxy_port", 8080), 8080);
    EXPECT_TRUE(cfg.get_bool("code.digest.include_signatures", true));
}

TEST_F(ConfigManagerTest, ParseError_WhenMalformed)
{
    // A [section with no closing bracket is a hard TOML syntax error.
    write("[general\ntheme = \"dark\"\n");

    ConfigManager cfg;
    const auto result = cfg.load(m_path);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::ConfigError);
    EXPECT_FALSE(cfg.loaded_from_file());
}

TEST_F(ConfigManagerTest, TypedGetters_RoundTrip)
{
    write(R"(
[general]
theme = "light"

[http]
proxy_port = 9090
max_captures = 5000

[code]
exclude = ["node_modules", "dist", "target"]

[code.digest]
include_signatures = false
confidence = 0.875
)");

    ConfigManager cfg;
    ASSERT_TRUE(cfg.load(m_path).has_value());
    EXPECT_TRUE(cfg.loaded_from_file());

    EXPECT_EQ(cfg.get_string("general.theme", "dark"), "light");
    EXPECT_EQ(cfg.get_int("http.proxy_port", 0), 9090);
    EXPECT_EQ(cfg.get_int("http.max_captures", 0), 5000);
    EXPECT_FALSE(cfg.get_bool("code.digest.include_signatures", true));
    EXPECT_DOUBLE_EQ(cfg.get_double("code.digest.confidence", 0.0), 0.875);

    const std::vector<std::string> exclude = cfg.get_string_array("code.exclude", {"fallback"});
    ASSERT_EQ(exclude.size(), 3U);
    EXPECT_EQ(exclude[0], "node_modules");
    EXPECT_EQ(exclude[1], "dist");
    EXPECT_EQ(exclude[2], "target");
}

TEST_F(ConfigManagerTest, EnvOverride_PassThrough)
{
    constexpr const char* k_env_name = "VECTIS_TEST_ENV_VAR_42";
    constexpr const char* k_env_value = "hello-from-env";

#if defined(_WIN32)
    _putenv_s(k_env_name, k_env_value);
#else
    ::setenv(k_env_name, k_env_value, 1);
#endif

    ConfigManager cfg;
    ASSERT_TRUE(cfg.load(m_path).has_value());

    const auto value = cfg.get_env(k_env_name);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, k_env_value);

    const auto missing = cfg.get_env("VECTIS_TEST_ENV_VAR_DOES_NOT_EXIST");
    EXPECT_FALSE(missing.has_value());

#if defined(_WIN32)
    _putenv_s(k_env_name, "");
#else
    ::unsetenv(k_env_name);
#endif
}

} // namespace
