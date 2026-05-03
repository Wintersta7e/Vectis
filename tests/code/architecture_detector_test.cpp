#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "code/architecture_detector.h"
#include "code/code_index.h"
#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::ArchitectureDescription;
using vectis::code::ArchitectureLabel;
using vectis::code::CodeIndex;
using vectis::code::detect_architecture;
using vectis::code::FileEntry;
using vectis::code::Language;

void add_file(CodeIndex& idx, const std::string& path, Language lang = Language::Cpp)
{
    FileEntry f;
    f.path_relative = path;
    f.language = lang;
    f.line_count = 10;
    idx.add_file(std::move(f));
}

TEST(ArchitectureDetectorTest, EmptyProject_IsUnknown)
{
    CodeIndex idx;
    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Unknown);
}

TEST(ArchitectureDetectorTest, ClassicMvc_DetectsMvc)
{
    CodeIndex idx;
    add_file(idx, "models/user.py", Language::Python);
    add_file(idx, "views/user_view.py", Language::Python);
    add_file(idx, "controllers/user_ctrl.py", Language::Python);
    add_file(idx, "main.py", Language::Python);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Mvc);
    EXPECT_GE(result.confidence, 80);
}

TEST(ArchitectureDetectorTest, Layered_DetectsLayered)
{
    CodeIndex idx;
    add_file(idx, "src/controllers/user.cpp", Language::Cpp);
    add_file(idx, "src/services/user.cpp", Language::Cpp);
    add_file(idx, "src/repositories/user.cpp", Language::Cpp);
    add_file(idx, "main.cpp", Language::Cpp);

    const auto result = detect_architecture(idx, "/fake");
    // Three layered signals (controllers, services, repositories)
    // without the views trio should trigger Layered, not MVC.
    EXPECT_EQ(result.label, ArchitectureLabel::Layered);
    EXPECT_NE(result.reasoning.find("controllers"), std::string::npos);
}

TEST(ArchitectureDetectorTest, FrontendSpa_DetectsSpaByDirectoryStructure)
{
    CodeIndex idx;
    add_file(idx, "src/components/Button.tsx", Language::TypeScript);
    add_file(idx, "src/components/Card.tsx", Language::TypeScript);
    add_file(idx, "src/pages/index.tsx", Language::TypeScript);
    add_file(idx, "src/pages/about.tsx", Language::TypeScript);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::FrontendSpa);
}

TEST(ArchitectureDetectorTest, FrontendSpa_DetectsSpaByConfigFile)
{
    // SPA-config probe is filesystem-based (matches the manifest
    // detectors), so the marker file has to actually exist on disk.
    namespace fs = std::filesystem;
    static std::uint64_t counter = 0;
    ++counter;
    const fs::path root =
        fs::temp_directory_path() / ("vectis_arch_test_spa_cfg_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    std::ofstream(root / "vite.config.ts") << "export default {}\n";

    CodeIndex idx;
    add_file(idx, "src/App.tsx", Language::TypeScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::FrontendSpa);
    EXPECT_NE(result.reasoning.find("vite"), std::string::npos);

    fs::remove_all(root, ec);
}

TEST(ArchitectureDetectorTest, Monorepo_DetectsPackagesPlusMultipleMains)
{
    CodeIndex idx;
    add_file(idx, "packages/api/main.go", Language::Go);
    add_file(idx, "packages/worker/main.go", Language::Go);
    add_file(idx, "packages/shared/util.go", Language::Go);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_GE(result.confidence, 80);
}

TEST(ArchitectureDetectorTest, PluginModularLayering_DetectedAsLayered)
{
    // Regression guard for the bug the Codex digest-review surfaced.
    // Vectis's own src/ layout (core/ + modes/ + platform/ + ui/) is
    // a plugin-modular layered architecture. Before this fix the
    // detector only knew "core" and "platform" from the signal list
    // and missed "modes" and "ui", bringing the match count below the
    // threshold and mislabeling the project as Monolith.
    CodeIndex idx;
    add_file(idx, "core/app.cpp");
    add_file(idx, "core/config_manager.cpp");
    add_file(idx, "modes/indexer.cpp");
    add_file(idx, "modes/reporter.cpp");
    add_file(idx, "platform/file_io.cpp");
    add_file(idx, "ui/theme.cpp");
    add_file(idx, "main.cpp");

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Layered);
    EXPECT_GE(result.confidence, 70);
    EXPECT_NE(result.reasoning.find("core"), std::string::npos);
    EXPECT_NE(result.reasoning.find("modes"), std::string::npos);
    EXPECT_NE(result.reasoning.find("platform"), std::string::npos);
    EXPECT_NE(result.reasoning.find("ui"), std::string::npos);
}

TEST(ArchitectureDetectorTest, HexagonalArchitecture_DetectedAsLayered)
{
    // Hexagonal / clean architecture: adapters + ports + domain.
    CodeIndex idx;
    add_file(idx, "src/domain/user.rs", Language::Rust);
    add_file(idx, "src/adapters/http/handler.rs", Language::Rust);
    add_file(idx, "src/ports/user_repository.rs", Language::Rust);
    add_file(idx, "src/infrastructure/db.rs", Language::Rust);
    add_file(idx, "src/main.rs", Language::Rust);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Layered);
    EXPECT_NE(result.reasoning.find("adapters"), std::string::npos);
}

TEST(ArchitectureDetectorTest, SmallProject_DefaultsToMonolith)
{
    CodeIndex idx;
    add_file(idx, "main.cpp", Language::Cpp);
    add_file(idx, "helper.cpp", Language::Cpp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Monolith);
}

TEST(ArchitectureDetectorTest, FixtureSubtreesDoNotPolluteReasoning)
{
    // Regression guard for the Vectis-scans-itself bug: test fixtures
    // under `tests/fixtures/...` containing `models/`, `dao/`,
    // `controllers/` subdirs used to inject fake layered signals into
    // architecture reasoning. The segment walker now stops descending
    // at test / fixture / docs / vendor roots so deep noise doesn't
    // reach the classifier.
    CodeIndex idx;
    // Real project shape — plugin / modular layering (Vectis-like).
    add_file(idx, "src/core/app.cpp", Language::Cpp);
    add_file(idx, "src/core/log.cpp", Language::Cpp);
    add_file(idx, "src/platform/io.cpp", Language::Cpp);
    add_file(idx, "src/main.cpp", Language::Cpp);
    // Test fixtures that must NOT inject signals.
    add_file(idx, "tests/fixtures/code/sample-java/models/User.java", Language::Java);
    add_file(idx, "tests/fixtures/code/sample-multimodule/business/dao/Foo.java", Language::Java);
    add_file(idx, "tests/fixtures/code/sample-mvc/controllers/Home.java", Language::Java);

    const auto result = detect_architecture(idx, "/fake");

    // Should be Layered or Monolith — definitely NOT MVC (needs
    // models+views+controllers trio; all three in fixtures must not
    // bleed through).
    EXPECT_NE(result.label, ArchitectureLabel::Mvc);
    // Reasoning must not cite `models/`, `dao/`, or `controllers/` —
    // they live under a fixture subtree that was pruned.
    EXPECT_EQ(result.reasoning.find("models"), std::string::npos)
        << "reasoning leaked fixture segment `models`: " << result.reasoning;
    EXPECT_EQ(result.reasoning.find("dao"), std::string::npos)
        << "reasoning leaked fixture segment `dao`: " << result.reasoning;
    EXPECT_EQ(result.reasoning.find("controllers"), std::string::npos)
        << "reasoning leaked fixture segment `controllers`: " << result.reasoning;
}

TEST(ArchitectureDetectorTest, MonorepoReasoningCitesActualMatch)
{
    // When only `libs/` fires (not `packages/` or `apps/`), the
    // reasoning string must say so — the old template claimed
    // "`packages/` or `apps/`" even when only `libs/` matched,
    // misleading consumers reading the digest.
    CodeIndex idx;
    add_file(idx, "libs/utils/main.go", Language::Go);
    add_file(idx, "libs/api/main.go", Language::Go);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_NE(result.reasoning.find("libs"), std::string::npos);
    EXPECT_EQ(result.reasoning.find("packages"), std::string::npos);
    EXPECT_EQ(result.reasoning.find("apps"), std::string::npos);
}

TEST(ArchitectureDetectorTest, Mvvm_DetectsViewModelsPlusViews)
{
    CodeIndex idx;
    add_file(idx, "src/FlowForge.UI/ViewModels/MainWindowViewModel.cs", Language::CSharp);
    add_file(idx, "src/FlowForge.UI/ViewModels/DialogViewModel.cs", Language::CSharp);
    add_file(idx, "src/FlowForge.UI/Views/MainWindow.xaml.cs", Language::CSharp);
    add_file(idx, "src/FlowForge.UI/App.xaml.cs", Language::CSharp);

    const auto result = detect_architecture(idx, "/fake");
    // The presence of dotted project dirs (FlowForge.UI) could also
    // trigger DotNetSolution; MVVM should rank higher because it's a
    // more specific signal.
    EXPECT_EQ(result.label, ArchitectureLabel::Mvvm);
    EXPECT_GE(result.confidence, 80);
}

TEST(ArchitectureDetectorTest, CleanArchitecture_DetectsThreeLayerFolders)
{
    CodeIndex idx;
    add_file(idx, "Domain/Entities/User.cs", Language::CSharp);
    add_file(idx, "Application/UseCases/CreateUser.cs", Language::CSharp);
    add_file(idx, "Infrastructure/Persistence/Db.cs", Language::CSharp);
    add_file(idx, "Presentation/Controllers/UserCtl.cs", Language::CSharp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::CleanArchitecture);
    EXPECT_GE(result.confidence, 80);
}

TEST(ArchitectureDetectorTest, DotNetSolution_DetectsMultipleDottedProjects)
{
    CodeIndex idx;
    // Two sibling projects under src/, each with a dotted name.
    // No ViewModels/Views (so MVVM doesn't pre-empt) and no
    // Domain/Application trio (so Clean doesn't pre-empt).
    add_file(idx, "src/Example.CLI/Program.cs", Language::CSharp);
    add_file(idx, "src/Example.Core/Engine.cs", Language::CSharp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::DotNetSolution);
    EXPECT_GE(result.confidence, 70);
}

// -----------------------------------------------------------------------------
// Workspace-manifest detection (post-pivot). These tests write a real
// tempdir with the expected marker file(s) because the detector now
// actually reads manifests from disk.
// -----------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

fs::path fresh_tmp(const char* tag)
{
    static std::uint64_t counter = 0;
    ++counter;
    fs::path p = fs::temp_directory_path() /
                 ("vectis_arch_test_" + std::string(tag) + "_" + std::to_string(counter));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void write_file(const fs::path& p, std::string_view body)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

} // namespace

TEST(ArchitectureDetectorTest, RustWorkspace_ReadsCargoToml)
{
    const fs::path root = fresh_tmp("rust");
    write_file(root / "Cargo.toml", R"(
[workspace]
members = ["crates/*"]
)");
    CodeIndex idx;
    add_file(idx, "crates/foo/src/lib.rs", Language::Rust);
    add_file(idx, "crates/bar/src/lib.rs", Language::Rust);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_GE(result.confidence, 90);
    EXPECT_NE(result.reasoning.find("Cargo workspace"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, NpmWorkspaces_ReadsPackageJson)
{
    const fs::path root = fresh_tmp("npm");
    write_file(root / "package.json", R"({
  "name": "root",
  "private": true,
  "workspaces": ["packages/*"]
}
)");
    CodeIndex idx;
    add_file(idx, "packages/ui/src/index.ts", Language::TypeScript);
    add_file(idx, "packages/core/src/index.ts", Language::TypeScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_NE(result.reasoning.find("npm workspaces"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PnpmWorkspace_DetectedByYaml)
{
    const fs::path root = fresh_tmp("pnpm");
    write_file(root / "pnpm-workspace.yaml", "packages:\n  - 'packages/*'\n");
    CodeIndex idx;
    add_file(idx, "packages/a/src/index.ts", Language::TypeScript);
    add_file(idx, "packages/b/src/index.ts", Language::TypeScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_NE(result.reasoning.find("pnpm"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, LernaAndTurbo_AlsoDetected)
{
    {
        const fs::path root = fresh_tmp("lerna");
        write_file(root / "lerna.json", "{ \"version\": \"0.0.0\" }\n");
        CodeIndex idx;
        add_file(idx, "packages/x/src/a.js", Language::JavaScript);
        const auto result = detect_architecture(idx, root);
        EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
        EXPECT_NE(result.reasoning.find("Lerna"), std::string::npos);
        fs::remove_all(root);
    }
    {
        const fs::path root = fresh_tmp("turbo");
        write_file(root / "turbo.json", "{ \"pipeline\": {} }\n");
        CodeIndex idx;
        add_file(idx, "apps/web/src/a.ts", Language::TypeScript);
        const auto result = detect_architecture(idx, root);
        EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
        EXPECT_NE(result.reasoning.find("Turborepo"), std::string::npos);
        fs::remove_all(root);
    }
}

TEST(ArchitectureDetectorTest, PythonSrcLayout_DetectsMultiplePackages)
{
    const fs::path root = fresh_tmp("python");
    write_file(root / "pyproject.toml", "[project]\nname = \"sample\"\nversion = \"0.0.0\"\n");
    CodeIndex idx;
    add_file(idx, "src/pkg_a/__init__.py", Language::Python);
    add_file(idx, "src/pkg_a/module.py", Language::Python);
    add_file(idx, "src/pkg_b/__init__.py", Language::Python);
    add_file(idx, "src/pkg_b/helpers.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_NE(result.reasoning.find("pyproject.toml"), std::string::npos);
    EXPECT_NE(result.reasoning.find("2 packages"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PythonSrcLayout_SinglePackageIsNotMonorepo)
{
    // A normal library with one package doesn't warrant Monorepo. The
    // detector should fall through to the earlier heuristics.
    const fs::path root = fresh_tmp("python_single");
    write_file(root / "pyproject.toml", "[project]\nname = \"sample\"\n");
    CodeIndex idx;
    add_file(idx, "src/mylib/__init__.py", Language::Python);
    add_file(idx, "src/mylib/core.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Monorepo);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, SubtableDoesNotTriggerRustWorkspace)
{
    // Regression guard: `[workspace.metadata]` alone must NOT be
    // treated as a workspace declaration — only a bare `[workspace]`
    // table header counts.
    const fs::path root = fresh_tmp("rust_subtable_only");
    write_file(root / "Cargo.toml", R"(
[package]
name = "sample"
version = "0.1.0"

[workspace.metadata.some_tool]
key = "value"
)");
    CodeIndex idx;
    add_file(idx, "src/main.rs", Language::Rust);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Monorepo);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, LabelName_RoundTrip)
{
    using vectis::code::architecture_label_name;
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Monolith), "Monolith");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Layered), "Layered");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Mvc), "MVC");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Monorepo), "Monorepo");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::FrontendSpa), "Frontend SPA");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Unknown), "Unknown");
}

TEST(ArchitectureDetectorTest, LibraryLayout_DetectsIncludeAndSrcWithoutMain)
{
    // Canonical C++ library shape: public headers in include/, impl in
    // src/. A test driver under test/ must NOT defeat detection — pure
    // libraries routinely ship their own tests.
    CodeIndex idx;
    add_file(idx, "include/foo.h", Language::Cpp);
    add_file(idx, "src/foo.cpp", Language::Cpp);
    add_file(idx, "src/helper.cpp", Language::Cpp);
    add_file(idx, "test/main.cpp", Language::Cpp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Library);
    EXPECT_GE(result.confidence, 70);
}

TEST(ArchitectureDetectorTest, LibraryLayout_DoesNotFireWhenSrcHasMain)
{
    // include/ + src/main.cpp is a binary that happens to expose a
    // public header directory — not a pure library. Library label
    // must yield to Monolith here.
    CodeIndex idx;
    add_file(idx, "include/foo.h", Language::Cpp);
    add_file(idx, "src/main.cpp", Language::Cpp);
    add_file(idx, "src/foo.cpp", Language::Cpp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_NE(result.label, ArchitectureLabel::Library);
}

TEST(ArchitectureDetectorTest, LibraryLayout_DoesNotFireOnVendoredNestedInclude)
{
    // Vendored `deps/<lib>/include/...` should not flip Library —
    // only top-level `include/` (or `lib/`) counts.
    CodeIndex idx;
    add_file(idx, "src/server.c", Language::C);
    add_file(idx, "src/networking.c", Language::C);
    add_file(idx, "deps/alloc_lib/include/alloc.h", Language::C);
    add_file(idx, "deps/alloc_lib/src/alloc.c", Language::C);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_NE(result.label, ArchitectureLabel::Library)
        << "reasoning was: " << result.reasoning;
}

TEST(ArchitectureDetectorTest, FrontendSpa_DoesNotFireOnNestedConfig)
{
    // A nested vite.config.js (e.g. backend's embedded error-page
    // renderer) must not flip FrontendSpa — the probe is root-only
    // and filesystem-based, so an index entry alone won't trigger.
    CodeIndex idx;
    for (int i = 0; i < 20; ++i) {
        add_file(idx, "src/Backend/file_" + std::to_string(i) + ".php", Language::Php);
    }
    add_file(idx,
             "src/Backend/resources/exceptions/renderer/vite.config.js",
             Language::JavaScript);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_NE(result.label, ArchitectureLabel::FrontendSpa)
        << "reasoning was: " << result.reasoning;
}

} // namespace
