#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "code/framework_hints.h"

namespace {

using ::vectis::code::hints::Ecosystem;
using ::vectis::code::hints::FrameworkHint;
using ::vectis::code::hints::hint_signal;
using ::vectis::code::hints::match;
using ::vectis::code::hints::match_annotations;

[[nodiscard]] bool has(const std::vector<FrameworkHint>& hits, FrameworkHint h)
{
    return std::ranges::find(hits, h) != hits.end();
}

// ----- signal-token round-trip ------------------------------------------

TEST(FrameworkHintsTest, HintSignalTokensAreStable)
{
    EXPECT_EQ(hint_signal(FrameworkHint::WebBackend), "hint:web-backend");
    EXPECT_EQ(hint_signal(FrameworkHint::WebFrontend), "hint:web-frontend");
    EXPECT_EQ(hint_signal(FrameworkHint::DesktopUI), "hint:desktop-ui");
}

TEST(FrameworkHintsTest, EmptyDepsListReturnsEmpty)
{
    EXPECT_TRUE(match(Ecosystem::Npm, std::vector<std::string>{}).empty());
}

// ----- npm --------------------------------------------------------------

TEST(FrameworkHintsTest, NpmReactFiresWebFrontend)
{
    const std::vector<std::string> deps = {"react", "react-dom", "lodash"};
    const auto hits = match(Ecosystem::Npm, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebFrontend));
    EXPECT_FALSE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, NpmExpressFiresWebBackend)
{
    const std::vector<std::string> deps = {"express", "body-parser"};
    const auto hits = match(Ecosystem::Npm, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
    EXPECT_FALSE(has(hits, FrameworkHint::WebFrontend));
}

TEST(FrameworkHintsTest, NpmFullstackFiresBothHints)
{
    // Common shape: a Next.js app with an embedded Express server, or
    // a NestJS API consumed by a React frontend in the same repo.
    const std::vector<std::string> deps = {"@nestjs/core", "react"};
    const auto hits = match(Ecosystem::Npm, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
    EXPECT_TRUE(has(hits, FrameworkHint::WebFrontend));
}

TEST(FrameworkHintsTest, NpmElectronFiresDesktopUI)
{
    const std::vector<std::string> deps = {"electron", "react"};
    const auto hits = match(Ecosystem::Npm, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
    EXPECT_TRUE(has(hits, FrameworkHint::WebFrontend));
}

// ----- pyproject --------------------------------------------------------

TEST(FrameworkHintsTest, PythonDjangoFlaskFastapiAllFireWebBackend)
{
    for (const auto& name : {"django", "flask", "fastapi", "sanic", "tornado"}) {
        const std::vector<std::string> deps = {name};
        const auto hits = match(Ecosystem::Pyproject, deps);
        EXPECT_TRUE(has(hits, FrameworkHint::WebBackend)) << "no WebBackend hit for " << name;
    }
}

TEST(FrameworkHintsTest, PythonUnknownDepDoesNotFire)
{
    const std::vector<std::string> deps = {"requests", "numpy", "click"};
    EXPECT_TRUE(match(Ecosystem::Pyproject, deps).empty());
}

// ----- cargo ------------------------------------------------------------

TEST(FrameworkHintsTest, CargoActixRocketAxumFireWebBackend)
{
    for (const auto& name : {"actix-web", "rocket", "axum", "warp", "tide"}) {
        const std::vector<std::string> deps = {name};
        const auto hits = match(Ecosystem::Cargo, deps);
        EXPECT_TRUE(has(hits, FrameworkHint::WebBackend)) << "no WebBackend hit for " << name;
    }
}

// ----- go.mod -----------------------------------------------------------

TEST(FrameworkHintsTest, GoModStripsVersionSuffix)
{
    const std::vector<std::string> deps = {
        "github.com/labstack/echo/v4", // major-version suffix
        "github.com/gofiber/fiber/v2", // major-version suffix
        "github.com/gin-gonic/gin",    // no suffix
    };
    const auto hits = match(Ecosystem::GoMod, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, GoModNonVersionSuffixIsNotStripped)
{
    // `/vendor` is not a Go major-version suffix — must NOT be stripped.
    const std::vector<std::string> deps = {"example.com/foo/vendor"};
    EXPECT_TRUE(match(Ecosystem::GoMod, deps).empty());
}

// ----- composer ---------------------------------------------------------

TEST(FrameworkHintsTest, ComposerLaravelSymfonyFireWebBackend)
{
    const std::vector<std::string> deps = {"laravel/framework", "guzzlehttp/guzzle"};
    const auto hits = match(Ecosystem::Composer, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

// ----- gemfile ----------------------------------------------------------

TEST(FrameworkHintsTest, GemfileRailsSinatraFireWebBackend)
{
    for (const auto& name : {"rails", "sinatra", "hanami"}) {
        const std::vector<std::string> deps = {name};
        const auto hits = match(Ecosystem::Gemfile, deps);
        EXPECT_TRUE(has(hits, FrameworkHint::WebBackend)) << "no WebBackend hit for " << name;
    }
}

// ----- maven ------------------------------------------------------------

TEST(FrameworkHintsTest, MavenStripsVersionFromGAV)
{
    // Maven dep import_strings are groupId:artifactId:version. The
    // matcher must normalise to groupId:artifactId before lookup.
    const std::vector<std::string> deps = {
        "org.springframework.boot:spring-boot-starter-web:3.1.0",
        "com.fasterxml.jackson.core:jackson-databind:2.15.0",
    };
    const auto hits = match(Ecosystem::Maven, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, MavenWithoutVersionAlsoMatches)
{
    const std::vector<std::string> deps = {"org.springframework.boot:spring-boot-starter-webflux"};
    const auto hits = match(Ecosystem::Maven, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, MavenJavaFxFiresDesktopUI)
{
    const std::vector<std::string> deps = {"org.openjfx:javafx-controls:21"};
    const auto hits = match(Ecosystem::Maven, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

// ----- dotnet -----------------------------------------------------------

TEST(FrameworkHintsTest, DotNetAspNetCoreFiresWebBackend)
{
    // csproj-package dep edges are formatted "PackageId:Version" —
    // matcher strips the version.
    const std::vector<std::string> deps = {"Microsoft.AspNetCore.App:8.0.0",
                                           "Newtonsoft.Json:13.0.3"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, DotNetAvaloniaFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Avalonia:11.0.0", "Avalonia.Desktop:11.0.0"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetMauiFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.Maui.Controls:8.0.0"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetWinUI2FiresDesktopUI)
{
    // Hybrid WPF + WinUI apps ship WinUI 2 via the standalone
    // Microsoft.UI.Xaml package rather than WindowsAppSDK, so the
    // hint must fire on that key too.
    const std::vector<std::string> deps = {"Microsoft.UI.Xaml:2.8.6"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetWpfBehaviorsFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.Xaml.Behaviors.Wpf:1.1.135"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetSdkOnlyWpfFiresDesktopUI)
{
    // The csproj handler emits versionless synthetic markers when a
    // project sets <UseWPF>true</UseWPF> with no nuget refs. They land
    // in the deps stream alongside real PackageReferences and must
    // still fire DesktopUI.
    const std::vector<std::string> deps = {"Microsoft.NET.Sdk.WindowsDesktop.WPF"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetSdkOnlyWinFormsFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.NET.Sdk.WindowsDesktop.WindowsForms"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetWindowsDesktopSdkFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.NET.Sdk.WindowsDesktop"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetWinUI3FiresDesktopUI)
{
    // Modern WinUI 3 apps opt into the
    // framework via <UseWinUI>true</UseWinUI>, not a Sdk attribute.
    const std::vector<std::string> deps = {"Microsoft.NET.Sdk.WinUI"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetToolkitWin32UISdkFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.Toolkit.Win32.UI.SDK:6.1.0"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

TEST(FrameworkHintsTest, DotNetToolkitWpfUiControlsFiresDesktopUI)
{
    const std::vector<std::string> deps = {"Microsoft.Toolkit.Wpf.UI.Controls:6.1.3"};
    const auto hits = match(Ecosystem::DotNet, deps);
    EXPECT_TRUE(has(hits, FrameworkHint::DesktopUI));
}

// ----- match_annotations ------------------------------------------------

TEST(FrameworkHintsTest, AnnotationsBelowThresholdDoNotFire)
{
    // Two route-handler annotations is below the ≥3 threshold; no
    // hint should fire even though both keywords match. Guards
    // against single-stub false positives.
    const std::vector<std::string> annotations = {"GetMapping(\"/users\")",
                                                  "PostMapping(\"/login\")"};
    EXPECT_TRUE(match_annotations(annotations).empty());
}

TEST(FrameworkHintsTest, AnnotationsAtThresholdFiresWebBackend)
{
    const std::vector<std::string> annotations = {"RestController", "GetMapping(\"/users\")",
                                                  "PostMapping(\"/login\")"};
    const auto hits = match_annotations(annotations);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, AnnotationsStripsCallExpression)
{
    // Flask + FastAPI decorators carry call expressions; the matcher
    // strips them. Verifies the normalisation for both forms.
    const std::vector<std::string> annotations = {"app.route(\"/users\")", "router.get(\"/api\")",
                                                  "router.post(\"/items\")"};
    const auto hits = match_annotations(annotations);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, AnnotationsMixedAcrossLanguages)
{
    // A polyglot codebase (Java backend + Python micro-service) can
    // hit the threshold across languages — the matcher doesn't care
    // about the source language as long as the names are unambiguous.
    const std::vector<std::string> annotations = {"RestController", "HttpGet(\"/api/users\")",
                                                  "app.route(\"/health\")"};
    const auto hits = match_annotations(annotations);
    EXPECT_TRUE(has(hits, FrameworkHint::WebBackend));
}

TEST(FrameworkHintsTest, AnnotationsUnknownDecoratorsAreIgnored)
{
    // Generic / non-web decorators must NOT contribute to the tally.
    const std::vector<std::string> annotations = {"pytest.fixture", "dataclass", "staticmethod",
                                                  "tokio::main", "derive(Debug)"};
    EXPECT_TRUE(match_annotations(annotations).empty());
}

TEST(FrameworkHintsTest, AnnotationsEmptyInputReturnsEmpty)
{
    EXPECT_TRUE(match_annotations(std::vector<std::string>{}).empty());
}

} // namespace
