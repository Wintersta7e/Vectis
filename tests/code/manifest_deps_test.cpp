#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "code/manifest_deps.h"

namespace {

using ::vectis::code::deps::extract_cargo;
using ::vectis::code::deps::extract_composer;
using ::vectis::code::deps::extract_gemfile;
using ::vectis::code::deps::extract_go_mod;
using ::vectis::code::deps::extract_npm;
using ::vectis::code::deps::extract_pyproject;
using ::vectis::code::deps::extract_requirements_txt;
using ::vectis::code::deps::extract_setup_py;

class ManifestDepsFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Per-test-instance tmp dir so parallel ctest workers (CI uses
        // -j) don't fight over the same path. The counter increments on
        // every SetUp so successive tests in the same process also get
        // distinct scratch space.
        static std::atomic<std::uint64_t> counter{0};
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        m_tmp =
            std::filesystem::temp_directory_path() /
            ("vectis-manifest-deps-test-" + std::to_string(::getpid()) + "-" + std::to_string(seq));
        std::filesystem::remove_all(m_tmp);
        std::filesystem::create_directories(m_tmp);
    }

    void TearDown() override { std::filesystem::remove_all(m_tmp); }

    [[nodiscard]] std::filesystem::path write_file(std::string_view name, std::string_view contents)
    {
        const auto path = m_tmp / name;
        std::ofstream out(path);
        out << contents;
        return path;
    }

    std::filesystem::path m_tmp;
};

// ----- extract_npm -------------------------------------------------------

TEST_F(ManifestDepsFixture, NpmExtractsDependencies)
{
    const auto path = write_file("package.json", R"({
        "name": "x",
        "dependencies": {
            "react": "^18.0.0",
            "react-dom": "^18.0.0"
        }
    })");
    const auto deps = extract_npm(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "react"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "react-dom"), deps.end());
}

TEST_F(ManifestDepsFixture, NpmMergesAllFourCategories)
{
    const auto path = write_file("package.json", R"({
        "dependencies":         { "react": "*" },
        "devDependencies":      { "typescript": "*" },
        "peerDependencies":     { "vue": "*" },
        "optionalDependencies": { "fsevents": "*" }
    })");
    const auto deps = extract_npm(path);
    EXPECT_EQ(deps.size(), 4U);
    EXPECT_NE(std::ranges::find(deps, "react"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "typescript"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "vue"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "fsevents"), deps.end());
}

TEST_F(ManifestDepsFixture, NpmDeduplicatesAcrossCategories)
{
    const auto path = write_file("package.json", R"({
        "dependencies":      { "lodash": "^4.0.0" },
        "devDependencies":   { "lodash": "^4.0.0" },
        "peerDependencies":  { "lodash": "*" }
    })");
    const auto deps = extract_npm(path);
    EXPECT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps.front(), "lodash");
}

TEST_F(ManifestDepsFixture, NpmPreservesScopedNamesVerbatim)
{
    const auto path = write_file("package.json", R"({
        "dependencies": {
            "@types/node":  "^20.0.0",
            "@nestjs/core": "^10.0.0"
        }
    })");
    const auto deps = extract_npm(path);
    EXPECT_NE(std::ranges::find(deps, "@types/node"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "@nestjs/core"), deps.end());
}

TEST_F(ManifestDepsFixture, NpmMissingDependenciesKeyReturnsEmpty)
{
    const auto path = write_file("package.json", R"({ "name": "x", "version": "1.0.0" })");
    EXPECT_TRUE(extract_npm(path).empty());
}

TEST_F(ManifestDepsFixture, NpmNonObjectDependenciesValueIsIgnored)
{
    // A malformed package.json with `"dependencies": "react"` shouldn't
    // crash the extractor — skip the bad node and keep going.
    const auto path = write_file("package.json", R"({
        "dependencies":    "react",
        "devDependencies": { "typescript": "*" }
    })");
    const auto deps = extract_npm(path);
    EXPECT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps.front(), "typescript");
}

TEST_F(ManifestDepsFixture, NpmMalformedJsonReturnsEmpty)
{
    const auto path = write_file("package.json", "{ this is not json");
    EXPECT_TRUE(extract_npm(path).empty());
}

TEST_F(ManifestDepsFixture, NpmTopLevelArrayReturnsEmpty)
{
    // package.json must be an object at the top level. Anything else
    // (array, scalar) is malformed; return empty rather than throw.
    const auto path = write_file("package.json", R"(["react", "vue"])");
    EXPECT_TRUE(extract_npm(path).empty());
}

TEST_F(ManifestDepsFixture, NpmAcceptsCStyleComments)
{
    // Some toolchains template-generate package.json with header
    // comments. nlohmann/json with ignore_comments=true accepts them.
    const auto path = write_file("package.json", R"({
        // generated by tooling — do not edit
        "dependencies": { "react": "*" }
    })");
    const auto deps = extract_npm(path);
    EXPECT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps.front(), "react");
}

TEST(ManifestDepsTest, NpmFileNotFoundReturnsEmpty)
{
    const std::filesystem::path nowhere =
        std::filesystem::temp_directory_path() / "vectis-no-such-file-package.json";
    std::filesystem::remove(nowhere);
    EXPECT_TRUE(extract_npm(nowhere).empty());
}

// ----- extract_pyproject -------------------------------------------------

TEST_F(ManifestDepsFixture, PyprojectExtractsPep621Dependencies)
{
    const auto path = write_file("pyproject.toml", R"(
[project]
name = "x"
dependencies = [
    "django>=4.0",
    "requests~=2.31",
    "click; python_version >= '3.8'",
]
)");
    const auto deps = extract_pyproject(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "requests"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "click"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectExtractsOptionalDependencies)
{
    const auto path = write_file("pyproject.toml", R"(
[project]
name = "x"
dependencies = ["base-dep"]

[project.optional-dependencies]
dev  = ["pytest>=7", "mypy"]
docs = ["sphinx"]
)");
    const auto deps = extract_pyproject(path);
    EXPECT_EQ(deps.size(), 4U);
    EXPECT_NE(std::ranges::find(deps, "base-dep"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "pytest"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "mypy"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "sphinx"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectExtractsPoetryDependencies)
{
    const auto path = write_file("pyproject.toml", R"(
[tool.poetry]
name = "x"

[tool.poetry.dependencies]
python = "^3.10"
django = "^4.0"
requests = { version = "^2.31", extras = ["socks"] }

[tool.poetry.dev-dependencies]
pytest = "^7.0"
)");
    const auto deps = extract_pyproject(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_EQ(std::ranges::find(deps, "python"), deps.end())
        << "python is the interpreter, not a dep";
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "requests"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "pytest"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectExtractsPoetryGroupDependencies)
{
    const auto path = write_file("pyproject.toml", R"(
[tool.poetry.dependencies]
python = "^3.10"
flask = "^2.0"

[tool.poetry.group.dev.dependencies]
pytest = "^7.0"
mypy   = "^1.0"

[tool.poetry.group.docs.dependencies]
sphinx = "^7.0"
)");
    const auto deps = extract_pyproject(path);
    EXPECT_EQ(deps.size(), 4U);
    EXPECT_NE(std::ranges::find(deps, "flask"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "pytest"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "mypy"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "sphinx"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectMergesPep621AndPoetry)
{
    // Unusual but legal — a project might declare PEP 621 and Poetry
    // tables simultaneously. Both contribute to the dep set.
    const auto path = write_file("pyproject.toml", R"(
[project]
name = "x"
dependencies = ["fastapi"]

[tool.poetry.dependencies]
python = "^3.10"
uvicorn = "^0.27"
)");
    const auto deps = extract_pyproject(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "fastapi"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "uvicorn"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectMalformedTomlReturnsEmpty)
{
    const auto path = write_file("pyproject.toml", "[project\nthis is broken =");
    EXPECT_TRUE(extract_pyproject(path).empty());
}

TEST_F(ManifestDepsFixture, PyprojectEmptyFileReturnsEmpty)
{
    const auto path = write_file("pyproject.toml", "");
    EXPECT_TRUE(extract_pyproject(path).empty());
}

// ----- extract_setup_py --------------------------------------------------

TEST_F(ManifestDepsFixture, SetupPyExtractsInstallRequiresLiteral)
{
    const auto path = write_file("setup.py", R"(from setuptools import setup
setup(
    name="x",
    install_requires=[
        "django>=4.0",
        "requests~=2.31",
        "click",
    ],
)
)");
    const auto deps = extract_setup_py(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "requests"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "click"), deps.end());
}

TEST_F(ManifestDepsFixture, SetupPyAcceptsSingleQuotes)
{
    const auto path = write_file("setup.py", R"(setup(install_requires=['django', 'requests']))");
    const auto deps = extract_setup_py(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "requests"), deps.end());
}

TEST_F(ManifestDepsFixture, SetupPyDynamicAssignmentReturnsEmpty)
{
    // Reading a requirements.txt or computing the list dynamically
    // is unsupported — we don't run Python. Returning empty is the
    // honest result, and any framework hint built on top will simply
    // not fire.
    const auto path = write_file("setup.py", R"(
def read_requirements():
    return open("requirements.txt").read().splitlines()

setup(install_requires=read_requirements())
)");
    EXPECT_TRUE(extract_setup_py(path).empty());
}

TEST_F(ManifestDepsFixture, SetupPyNoInstallRequiresReturnsEmpty)
{
    const auto path = write_file("setup.py", R"(from setuptools import setup; setup(name="x"))");
    EXPECT_TRUE(extract_setup_py(path).empty());
}

TEST_F(ManifestDepsFixture, SetupPyDoesNotMatchRequiresInsideInstallRequires)
{
    // Regression: a naive search for "requires" matches the trailing 8
    // chars of "install_requires". The keyword scanner must anchor on
    // a non-identifier boundary so the second pass advances past the
    // first matched list instead of re-parsing the same brackets.
    const auto path = write_file("setup.py", R"(setup(install_requires=["flask"]))");
    const auto deps = extract_setup_py(path);
    EXPECT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps.front(), "flask");
}

TEST_F(ManifestDepsFixture, SetupPyPicksUpStandaloneRequiresKeyword)
{
    // Setup.py historically used `requires=` before `install_requires`
    // became dominant. The boundary anchor must still accept the
    // standalone form.
    const auto path = write_file("setup.py", R"(setup(name="x", requires=["click", "rich"]))");
    const auto deps = extract_setup_py(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "click"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "rich"), deps.end());
}

// ----- extract_requirements_txt ------------------------------------------

TEST_F(ManifestDepsFixture, RequirementsTxtExtractsCanonicalNames)
{
    // Real-world Django requirements.txt mixes capitalisation, pin
    // styles, extras, env markers, comments, and -r includes. The
    // extractor must return the PEP 503 canonical short name for
    // each spec so the matcher's lowercase table can find them.
    const auto path =
        write_file("requirements.txt", "# top-level deps\n"
                                       "Django==1.10.5\n"
                                       "django-cors-middleware==1.3.1  # inline\n"
                                       "djangorestframework>=3.4,<4.0\n"
                                       "requests~=2.31\n"
                                       "flask[async]==2.3.0\n"
                                       "package_with_underscores\n"
                                       "Some.Dotted.Name==1.0\n"
                                       "uvicorn ; python_version >= \"3.8\"\n"
                                       "\n"
                                       "-r dev-requirements.txt\n"
                                       "-e .\n"
                                       "git+https://github.com/example/pkg.git@main#egg=pkg\n"
                                       "http://example.com/wheel.whl\n");
    const auto deps = extract_requirements_txt(path);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "django-cors-middleware"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "djangorestframework"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "requests"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "flask"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "package-with-underscores"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "some-dotted-name"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "uvicorn"), deps.end());
    // Comments / blank lines / -r / -e / VCS / URL lines are skipped.
    EXPECT_EQ(std::ranges::find(deps, ""), deps.end());
    EXPECT_EQ(std::ranges::find(deps, "pkg"), deps.end());
}

TEST_F(ManifestDepsFixture, RequirementsTxtMissingFileReturnsEmpty)
{
    const auto deps = extract_requirements_txt(m_tmp / "missing-requirements.txt");
    EXPECT_TRUE(deps.empty());
}

TEST_F(ManifestDepsFixture, PyprojectPoetryDjangoCanonicalises)
{
    // Real-world Poetry projects often write Django with its canonical
    // capitalisation. The framework table is keyed lowercase, so the
    // extractor must emit the PEP 503-normalised short name.
    const auto path = write_file("pyproject.toml", R"(
[tool.poetry.dependencies]
Django = "^4.2"
)");
    const auto deps = extract_pyproject(path);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_EQ(std::ranges::find(deps, "Django"), deps.end());
}

TEST_F(ManifestDepsFixture, PyprojectPep621FlaskCanonicalises)
{
    const auto path = write_file("pyproject.toml", R"(
[project]
dependencies = ["Flask>=2.0", "FastAPI~=0.110"]
)");
    const auto deps = extract_pyproject(path);
    EXPECT_NE(std::ranges::find(deps, "flask"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "fastapi"), deps.end());
}

TEST_F(ManifestDepsFixture, SetupPyDjangoCanonicalises)
{
    const auto path =
        write_file("setup.py", R"(setup(install_requires=['Django', 'Some.Dotted.Name']))");
    const auto deps = extract_setup_py(path);
    EXPECT_NE(std::ranges::find(deps, "django"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "some-dotted-name"), deps.end());
}

TEST_F(ManifestDepsFixture, RequirementsTxtLeadingAndTrailingSeparatorsStripped)
{
    // Real packages exist with leading/trailing separator characters
    // (e.g. `_pytest`, the internal pytest helpers package). Canonical
    // PEP 503 output drops them so the matcher sees the bare name.
    const auto path = write_file("requirements.txt", "_pytest==7.0\n"
                                                     ".dotleading\n"
                                                     "trailing-\n");
    const auto deps = extract_requirements_txt(path);
    EXPECT_NE(std::ranges::find(deps, "pytest"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "dotleading"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "trailing"), deps.end());
}

TEST_F(ManifestDepsFixture, RequirementsTxtConsecutiveSeparatorsCollapsed)
{
    // Runs of `[_.-]` collapse to a single `-` so `some__pkg` and
    // `some..pkg` and `some--pkg` all canonicalise identically.
    const auto path = write_file("requirements.txt", "some__pkg==1.0\n"
                                                     "some..pkg==1.0\n"
                                                     "some--pkg==1.0\n");
    const auto deps = extract_requirements_txt(path);
    EXPECT_EQ(deps.size(), 1U);
    EXPECT_NE(std::ranges::find(deps, "some-pkg"), deps.end());
}

// ----- extract_cargo -----------------------------------------------------

TEST_F(ManifestDepsFixture, CargoExtractsSimpleDependencies)
{
    const auto path = write_file("Cargo.toml", R"(
[package]
name = "x"

[dependencies]
tokio  = "1.0"
serde  = { version = "1.0", features = ["derive"] }
clap   = { version = "4.0" }
)");
    const auto deps = extract_cargo(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "tokio"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "serde"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "clap"), deps.end());
}

TEST_F(ManifestDepsFixture, CargoMergesDevAndBuildDependencies)
{
    const auto path = write_file("Cargo.toml", R"(
[dependencies]
tokio = "1.0"

[dev-dependencies]
proptest = "1.0"

[build-dependencies]
cc = "1.0"
)");
    const auto deps = extract_cargo(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "tokio"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "proptest"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "cc"), deps.end());
}

TEST_F(ManifestDepsFixture, CargoPathAndGitDepsKeepCrateName)
{
    const auto path = write_file("Cargo.toml", R"(
[dependencies]
my_lib   = { path = "../my_lib" }
some_pkg = { git = "https://example/some_pkg" }
shared   = { workspace = true }
)");
    const auto deps = extract_cargo(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "my_lib"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "some_pkg"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "shared"), deps.end());
}

TEST_F(ManifestDepsFixture, CargoMissingTablesReturnsEmpty)
{
    const auto path = write_file("Cargo.toml", R"(
[package]
name = "lonely"
)");
    EXPECT_TRUE(extract_cargo(path).empty());
}

TEST_F(ManifestDepsFixture, CargoMalformedTomlReturnsEmpty)
{
    const auto path = write_file("Cargo.toml", "[dependencies\nbroken");
    EXPECT_TRUE(extract_cargo(path).empty());
}

// ----- extract_go_mod ----------------------------------------------------

TEST_F(ManifestDepsFixture, GoModExtractsRequireBlock)
{
    const auto path = write_file("go.mod", R"(module example.com/x

go 1.21

require (
    github.com/gin-gonic/gin v1.9.1
    github.com/stretchr/testify v1.8.4
    golang.org/x/sync v0.6.0 // indirect
)
)");
    const auto deps = extract_go_mod(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "github.com/gin-gonic/gin"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "github.com/stretchr/testify"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "golang.org/x/sync"), deps.end());
}

TEST_F(ManifestDepsFixture, GoModExtractsSingleLineRequire)
{
    const auto path = write_file("go.mod", R"(module example.com/x

require github.com/labstack/echo/v4 v4.11.0
require github.com/spf13/cobra v1.7.0
)");
    const auto deps = extract_go_mod(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "github.com/labstack/echo/v4"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "github.com/spf13/cobra"), deps.end());
}

TEST_F(ManifestDepsFixture, GoModIgnoresLineComments)
{
    const auto path = write_file("go.mod", R"(module example.com/x

require (
    // a header comment line
    github.com/foo/bar v1.0.0 // a trailing comment
    // another
    github.com/baz/qux v0.5.0
)
)");
    const auto deps = extract_go_mod(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "github.com/foo/bar"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "github.com/baz/qux"), deps.end());
}

TEST_F(ManifestDepsFixture, GoModNoRequireReturnsEmpty)
{
    const auto path = write_file("go.mod", R"(module example.com/x

go 1.21
)");
    EXPECT_TRUE(extract_go_mod(path).empty());
}

// ----- extract_composer --------------------------------------------------

TEST_F(ManifestDepsFixture, ComposerExtractsRequireKeys)
{
    const auto path = write_file("composer.json", R"({
        "require": {
            "php":               "^8.1",
            "symfony/console":   "^6.0",
            "guzzlehttp/guzzle": "^7.5",
            "ext-mbstring":      "*"
        },
        "require-dev": {
            "phpunit/phpunit": "^10.0"
        }
    })");
    const auto deps = extract_composer(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_EQ(std::ranges::find(deps, "php"), deps.end()) << "php is excluded";
    EXPECT_EQ(std::ranges::find(deps, "ext-mbstring"), deps.end()) << "ext-* is excluded";
    EXPECT_NE(std::ranges::find(deps, "symfony/console"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "guzzlehttp/guzzle"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "phpunit/phpunit"), deps.end());
}

TEST_F(ManifestDepsFixture, ComposerMissingRequireReturnsEmpty)
{
    const auto path = write_file("composer.json", R"({ "name": "x/y" })");
    EXPECT_TRUE(extract_composer(path).empty());
}

TEST_F(ManifestDepsFixture, ComposerMalformedJsonReturnsEmpty)
{
    const auto path = write_file("composer.json", "{ broken");
    EXPECT_TRUE(extract_composer(path).empty());
}

// ----- extract_gemfile ---------------------------------------------------

TEST_F(ManifestDepsFixture, GemfileExtractsSimpleGems)
{
    const auto path = write_file("Gemfile", R"(source "https://rubygems.org"

gem "rails", "~> 7.1"
gem 'puma', "~> 6.0"
gem "sidekiq"
)");
    const auto deps = extract_gemfile(path);
    EXPECT_EQ(deps.size(), 3U);
    EXPECT_NE(std::ranges::find(deps, "rails"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "puma"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "sidekiq"), deps.end());
}

TEST_F(ManifestDepsFixture, GemfileFlattenAcrossGroups)
{
    const auto path = write_file("Gemfile", R"(source "https://rubygems.org"

gem "rails"

group :development, :test do
  gem "rspec-rails"
  gem "factory_bot_rails"
end

group :development do
  gem "listen", "~> 3.8"
end
)");
    const auto deps = extract_gemfile(path);
    EXPECT_EQ(deps.size(), 4U);
    EXPECT_NE(std::ranges::find(deps, "rails"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "rspec-rails"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "factory_bot_rails"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "listen"), deps.end());
}

TEST_F(ManifestDepsFixture, GemfileIgnoresHashComments)
{
    const auto path = write_file("Gemfile", R"(# top-level comment
gem "rails", "~> 7.1" # inline comment
# gem "commented-out"
gem 'sidekiq'
)");
    const auto deps = extract_gemfile(path);
    EXPECT_EQ(deps.size(), 2U);
    EXPECT_NE(std::ranges::find(deps, "rails"), deps.end());
    EXPECT_NE(std::ranges::find(deps, "sidekiq"), deps.end());
}

TEST_F(ManifestDepsFixture, GemfileEmptyReturnsEmpty)
{
    const auto path = write_file("Gemfile", "source \"https://rubygems.org\"\n");
    EXPECT_TRUE(extract_gemfile(path).empty());
}

} // namespace
