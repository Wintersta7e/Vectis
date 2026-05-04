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
    EXPECT_NE(result.label, ArchitectureLabel::Library) << "reasoning was: " << result.reasoning;
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
    add_file(idx, "src/Backend/resources/exceptions/renderer/vite.config.js", Language::JavaScript);

    const auto result = detect_architecture(idx, "/fake");
    EXPECT_NE(result.label, ArchitectureLabel::FrontendSpa)
        << "reasoning was: " << result.reasoning;
}

TEST(ArchitectureDetectorTest, Mvc_DetectsTrioWhenViewsAreNonSourceTemplates)
{
    // Rails-shape: views are template files vectis doesn't index, so
    // `views` never enters segments from the index walk. The disk-walk
    // augmentation must still pick up the `app/views/` directory.
    const fs::path root = fresh_tmp("mvc_disk_views");
    fs::create_directories(root / "app" / "views");
    write_file(root / "app" / "controllers" / "users_controller.rb",
               "class UsersController; end\n");
    write_file(root / "app" / "models" / "user.rb", "class User; end\n");

    CodeIndex idx;
    add_file(idx, "app/controllers/users_controller.rb", Language::Ruby);
    add_file(idx, "app/models/user.rb", Language::Ruby);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Mvc);
    EXPECT_GE(result.confidence, 80);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, ApiBackend_DetectsRoutersDirectory)
{
    // Beego/Go convention: a `routers/` directory at the project root
    // plus no frontend config should label ApiBackend.
    const fs::path root = fresh_tmp("api_routers");
    write_file(root / "main.go", "package main\nfunc main() {}\n");
    write_file(root / "routers" / "router.go", "package routers\n");
    write_file(root / "controllers" / "user.go", "package controllers\n");

    CodeIndex idx;
    add_file(idx, "main.go", Language::Go);
    add_file(idx, "routers/router.go", Language::Go);
    add_file(idx, "controllers/user.go", Language::Go);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::ApiBackend);
    EXPECT_NE(result.reasoning.find("routers"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, DiskWalk_SkipsHeavyVendorDirs)
{
    // node_modules, build, .git etc. must not inject directory names
    // into signals — otherwise a JS project's vendored package with
    // its own MVC layout would falsely flip the label.
    const fs::path root = fresh_tmp("disk_walk_skip");
    fs::create_directories(root / "node_modules" / "fake-pkg" / "controllers");
    fs::create_directories(root / "node_modules" / "fake-pkg" / "models");
    fs::create_directories(root / "node_modules" / "fake-pkg" / "views");
    fs::create_directories(root / "build" / "intermediate");
    write_file(root / "main.js", "console.log('ok');\n");

    CodeIndex idx;
    add_file(idx, "main.js", Language::JavaScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Mvc);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, Monolith_BumpsConfidenceWithRootManifest)
{
    // A root manifest (go.mod, composer.json, package.json, ...) is
    // concrete evidence even when the directory shape is ambiguous —
    // confidence and reasoning should reflect the runtime, not just
    // "no distinctive layout".
    const fs::path root = fresh_tmp("monolith_manifest");
    write_file(root / "go.mod", "module example\n");
    write_file(root / "main.go", "package main\nfunc main() {}\n");

    CodeIndex idx;
    add_file(idx, "main.go", Language::Go);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Monolith);
    EXPECT_GE(result.confidence, 55);
    EXPECT_NE(result.reasoning.find("Go"), std::string::npos);
    EXPECT_NE(result.reasoning.find("go.mod"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, DiskWalk_SkipsCanonicalListMembers)
{
    // htmlcov/ is in the canonical scanner exclude list (added when the
    // disk-walk skip-list was unified) but was missing from the old
    // detector-local list. Verify it now gets skipped.
    const fs::path root = fresh_tmp("disk_walk_canonical");
    fs::create_directories(root / "htmlcov" / "controllers");
    fs::create_directories(root / "htmlcov" / "models");
    fs::create_directories(root / "htmlcov" / "views");
    write_file(root / "main.py", "print('ok')\n");

    CodeIndex idx;
    add_file(idx, "main.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Mvc) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, DiskWalk_RespectsCallerSuppliedExcludeSet)
{
    // Production callers (CLI -> ExportOptions) thread the runtime
    // ScanConfig::exclude_dir_names through, so .gitignore-derived
    // names extend the disk-walk filter. Simulate that by passing a
    // custom set with an unusual project-specific name.
    const fs::path root = fresh_tmp("disk_walk_custom_excludes");
    fs::create_directories(root / "scratchpad" / "controllers");
    fs::create_directories(root / "scratchpad" / "models");
    fs::create_directories(root / "scratchpad" / "views");
    write_file(root / "main.go", "package main\nfunc main() {}\n");

    CodeIndex idx;
    add_file(idx, "main.go", Language::Go);

    auto excludes = vectis::code::default_scanner_exclude_dir_names();
    excludes.insert("scratchpad");

    const auto result = detect_architecture(idx, root, excludes);
    EXPECT_NE(result.label, ArchitectureLabel::Mvc) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, NodeLibrary_PackageJsonWithMain)
{
    const fs::path root = fresh_tmp("node_lib_main");
    write_file(root / "package.json", R"({"name":"foo","main":"lib/foo.js"})");
    write_file(root / "lib" / "foo.js", "module.exports = {};\n");

    CodeIndex idx;
    add_file(idx, "lib/foo.js", Language::JavaScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;
    EXPECT_GE(result.confidence, 70);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, NodeLibrary_RootIndexJsWithoutMainField)
{
    // Express 5.x ships no `main` field — entry defaults to ./index.js.
    // Without this fallback the framework would still read Monolith.
    const fs::path root = fresh_tmp("node_lib_root_index");
    write_file(root / "package.json", R"({"name":"foo","version":"1.0.0"})");
    write_file(root / "index.js", "module.exports = require('./lib/foo');\n");
    write_file(root / "lib" / "foo.js", "module.exports = {};\n");

    CodeIndex idx;
    add_file(idx, "index.js", Language::JavaScript);
    add_file(idx, "lib/foo.js", Language::JavaScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, NodeLibrary_PrivatePackageIsNotLibrary)
{
    // Internal apps mark `private: true` so npm refuses to publish.
    // That signal alone keeps them out of the Library bucket.
    const fs::path root = fresh_tmp("node_private");
    write_file(root / "package.json", R"({"name":"foo","main":"index.js","private": true})");
    write_file(root / "index.js", "console.log('hi');\n");

    CodeIndex idx;
    add_file(idx, "index.js", Language::JavaScript);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PhpLibrary_TypeLibraryInComposer)
{
    const fs::path root = fresh_tmp("php_lib_type");
    write_file(root / "composer.json", R"({"name":"vendor/foo","type": "library"})");
    write_file(root / "src" / "Foo.php", "<?php\nclass Foo {}\n");

    CodeIndex idx;
    add_file(idx, "src/Foo.php", Language::Php);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;
    EXPECT_GE(result.confidence, 75);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PhpLibrary_AutoloadWithoutIndexEntry)
{
    const fs::path root = fresh_tmp("php_lib_autoload");
    write_file(root / "composer.json",
               R"({"name":"vendor/foo","autoload":{"psr-4":{"Foo\\":"src/"}}})");
    write_file(root / "src" / "Foo.php", "<?php\nclass Foo {}\n");

    CodeIndex idx;
    add_file(idx, "src/Foo.php", Language::Php);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, RubyLibrary_GemspecAtRoot)
{
    const fs::path root = fresh_tmp("ruby_gem");
    write_file(root / "Gemfile", "source 'https://rubygems.org'\n");
    write_file(root / "foo.gemspec", "Gem::Specification.new do |s| s.name='foo' end\n");
    write_file(root / "lib" / "foo.rb", "module Foo end\n");

    CodeIndex idx;
    add_file(idx, "lib/foo.rb", Language::Ruby);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;
    EXPECT_NE(result.reasoning.find("foo.gemspec"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PythonLibrary_PyprojectWithSrcLayout)
{
    const fs::path root = fresh_tmp("py_lib_src");
    write_file(root / "pyproject.toml", "[project]\nname = \"foo\"\n");
    write_file(root / "src" / "foo" / "__init__.py", "\n");
    write_file(root / "src" / "foo" / "core.py", "def hello(): pass\n");

    CodeIndex idx;
    add_file(idx, "src/foo/__init__.py", Language::Python);
    add_file(idx, "src/foo/core.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;
    EXPECT_NE(result.reasoning.find("foo"), std::string::npos);

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PythonLibrary_FlatPackageWithSetupPy)
{
    const fs::path root = fresh_tmp("py_lib_flat");
    write_file(root / "setup.py", "from setuptools import setup\n");
    write_file(root / "foo" / "__init__.py", "\n");
    write_file(root / "foo" / "core.py", "def hello(): pass\n");

    CodeIndex idx;
    add_file(idx, "foo/__init__.py", Language::Python);
    add_file(idx, "foo/core.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_EQ(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

TEST(ArchitectureDetectorTest, PythonLibrary_DjangoAppEntryDoesNotMatch)
{
    // pyproject.toml + manage.py at root means a Django app, not the
    // framework itself — must NOT label as Library.
    const fs::path root = fresh_tmp("py_django_app");
    write_file(root / "pyproject.toml", "[project]\nname = \"myapp\"\n");
    write_file(root / "manage.py", "import django\n");
    write_file(root / "myapp" / "__init__.py", "\n");

    CodeIndex idx;
    add_file(idx, "manage.py", Language::Python);
    add_file(idx, "myapp/__init__.py", Language::Python);

    const auto result = detect_architecture(idx, root);
    EXPECT_NE(result.label, ArchitectureLabel::Library) << "reasoning: " << result.reasoning;

    fs::remove_all(root);
}

} // namespace
