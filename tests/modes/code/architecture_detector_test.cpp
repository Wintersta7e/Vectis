#include "modes/code/architecture_detector.h"

#include <string>
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
