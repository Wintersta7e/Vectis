#include "modes/code/architecture_detector.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "modes/code/code_index.h"
#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::ArchitectureDescription;
using vectis::modes::code::ArchitectureLabel;
using vectis::modes::code::CodeIndex;
using vectis::modes::code::detect_architecture;
using vectis::modes::code::FileEntry;
using vectis::modes::code::Language;

void add_file(CodeIndex& idx, const std::string& path,
              Language lang = Language::Cpp)
{
    FileEntry f;
    f.path_relative = path;
    f.language      = lang;
    f.line_count    = 10;
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
    add_file(idx, "models/user.py",       Language::Python);
    add_file(idx, "views/user_view.py",   Language::Python);
    add_file(idx, "controllers/user_ctrl.py", Language::Python);
    add_file(idx, "main.py",              Language::Python);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Mvc);
    EXPECT_GE(result.confidence, 80);
}

TEST(ArchitectureDetectorTest, Layered_DetectsLayered)
{
    CodeIndex idx;
    add_file(idx, "src/controllers/user.cpp", Language::Cpp);
    add_file(idx, "src/services/user.cpp",    Language::Cpp);
    add_file(idx, "src/repositories/user.cpp", Language::Cpp);
    add_file(idx, "main.cpp",                 Language::Cpp);

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
    add_file(idx, "src/components/Card.tsx",   Language::TypeScript);
    add_file(idx, "src/pages/index.tsx",       Language::TypeScript);
    add_file(idx, "src/pages/about.tsx",       Language::TypeScript);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::FrontendSpa);
}

TEST(ArchitectureDetectorTest, FrontendSpa_DetectsSpaByConfigFile)
{
    CodeIndex idx;
    add_file(idx, "src/App.tsx",               Language::TypeScript);
    add_file(idx, "vite.config.ts",            Language::TypeScript);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::FrontendSpa);
    EXPECT_NE(result.reasoning.find("vite"), std::string::npos);
}

TEST(ArchitectureDetectorTest, Monorepo_DetectsPackagesPlusMultipleMains)
{
    CodeIndex idx;
    add_file(idx, "packages/api/main.go",      Language::Go);
    add_file(idx, "packages/worker/main.go",   Language::Go);
    add_file(idx, "packages/shared/util.go",   Language::Go);

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
    add_file(idx, "modes/code/code_mode.cpp");
    add_file(idx, "modes/code/scanner.cpp");
    add_file(idx, "platform/file_io.cpp");
    add_file(idx, "ui/theme.cpp");
    add_file(idx, "main.cpp");

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Layered);
    EXPECT_GE(result.confidence, 70);
    EXPECT_NE(result.reasoning.find("core"),     std::string::npos);
    EXPECT_NE(result.reasoning.find("modes"),    std::string::npos);
    EXPECT_NE(result.reasoning.find("platform"), std::string::npos);
    EXPECT_NE(result.reasoning.find("ui"),       std::string::npos);
}

TEST(ArchitectureDetectorTest, HexagonalArchitecture_DetectedAsLayered)
{
    // Hexagonal / clean architecture: adapters + ports + domain.
    CodeIndex idx;
    add_file(idx, "src/domain/user.rs",              Language::Rust);
    add_file(idx, "src/adapters/http/handler.rs",    Language::Rust);
    add_file(idx, "src/ports/user_repository.rs",    Language::Rust);
    add_file(idx, "src/infrastructure/db.rs",        Language::Rust);
    add_file(idx, "src/main.rs",                     Language::Rust);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Layered);
    EXPECT_NE(result.reasoning.find("adapters"), std::string::npos);
}

TEST(ArchitectureDetectorTest, SmallProject_DefaultsToMonolith)
{
    CodeIndex idx;
    add_file(idx, "main.cpp",  Language::Cpp);
    add_file(idx, "helper.cpp", Language::Cpp);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_EQ(result.label, ArchitectureLabel::Monolith);
}

TEST(ArchitectureDetectorTest, Mvvm_DetectsViewModelsPlusViews)
{
    CodeIndex idx;
    add_file(idx, "src/FlowForge.UI/ViewModels/MainWindowViewModel.cs",
             Language::CSharp);
    add_file(idx, "src/FlowForge.UI/ViewModels/DialogViewModel.cs",
             Language::CSharp);
    add_file(idx, "src/FlowForge.UI/Views/MainWindow.xaml.cs",
             Language::CSharp);
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
    add_file(idx, "Domain/Entities/User.cs",               Language::CSharp);
    add_file(idx, "Application/UseCases/CreateUser.cs",    Language::CSharp);
    add_file(idx, "Infrastructure/Persistence/Db.cs",      Language::CSharp);
    add_file(idx, "Presentation/Controllers/UserCtl.cs",   Language::CSharp);

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
    add_file(idx, "src/Example.CLI/Program.cs",   Language::CSharp);
    add_file(idx, "src/Example.Core/Engine.cs",   Language::CSharp);

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
    add_file(idx, "packages/ui/src/index.ts",    Language::TypeScript);
    add_file(idx, "packages/core/src/index.ts",  Language::TypeScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monorepo);
    EXPECT_NE(result.reasoning.find("npm workspaces"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PnpmWorkspace_DetectedByYaml)
{
    const fs::path root = fresh_tmp("pnpm");
    write_file(root / "pnpm-workspace.yaml",
               "packages:\n  - 'packages/*'\n");
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
    write_file(root / "pyproject.toml",
               "[project]\nname = \"sample\"\nversion = \"0.0.0\"\n");
    CodeIndex idx;
    add_file(idx, "src/pkg_a/__init__.py",     Language::Python);
    add_file(idx, "src/pkg_a/module.py",       Language::Python);
    add_file(idx, "src/pkg_b/__init__.py",     Language::Python);
    add_file(idx, "src/pkg_b/helpers.py",      Language::Python);

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
    write_file(root / "pyproject.toml",
               "[project]\nname = \"sample\"\n");
    CodeIndex idx;
    add_file(idx, "src/mylib/__init__.py", Language::Python);
    add_file(idx, "src/mylib/core.py",     Language::Python);

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
    using vectis::modes::code::architecture_label_name;
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Monolith), "Monolith");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Layered), "Layered");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Mvc), "MVC");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Monorepo), "Monorepo");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::FrontendSpa), "Frontend SPA");
    EXPECT_EQ(architecture_label_name(ArchitectureLabel::Unknown), "Unknown");
}

} // namespace
