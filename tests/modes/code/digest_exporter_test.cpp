#include "modes/code/digest_exporter.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "modes/code/code_index.h"
#include "modes/code/dependency.h"
#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::build_digest_string;
using vectis::modes::code::CodeIndex;
using vectis::modes::code::default_output_path;
using vectis::modes::code::Dependency;
using vectis::modes::code::DigestFormat;
using vectis::modes::code::ExportOptions;
using vectis::modes::code::export_digest;
using vectis::modes::code::FileEntry;
using vectis::modes::code::Language;
using vectis::modes::code::Symbol;
using vectis::modes::code::SymbolKind;

/// Populate a small synthetic index with two files and four symbols.
/// Used by every test in this file so the assertions share a fixture.
void populate_synthetic_index(CodeIndex& index)
{
    FileEntry f1;
    f1.path_relative = "src/core/app.cpp";
    f1.language      = Language::Cpp;
    f1.size          = 15234;
    f1.line_count    = 425;
    const std::int64_t f1_id = index.add_file(std::move(f1));

    FileEntry f2;
    f2.path_relative = "src/modes/code/scanner.cpp";
    f2.language      = Language::Cpp;
    f2.size          = 9123;
    f2.line_count    = 280;
    const std::int64_t f2_id = index.add_file(std::move(f2));

    const std::array<Symbol, 6> batch = {
        Symbol{0, f1_id, "App",        SymbolKind::Class,  13,  460, 0, "",                       {}},
        Symbol{0, f1_id, "initialize", SymbolKind::Method, 109, 256, 0, "bool App::initialize()", {}},
        Symbol{0, f1_id, "run",        SymbolKind::Method, 258, 300, 0, "int App::run()",         {}},
        Symbol{0, f2_id, "Scanner",    SymbolKind::Class,  52,  280, 0, "",                       {}},
        Symbol{0, f1_id, "ErrorKind",  SymbolKind::Enum,   14,  22,  0, "",                       {"IoError", "ParseError", "NetworkError"}},
        Symbol{0, f2_id, "Point",      SymbolKind::Struct, 30,  34,  0, "",                       {"x", "y"}},
    };
    index.add_symbols(batch);
}

ExportOptions make_options(DigestFormat format, const std::filesystem::path& root)
{
    ExportOptions options;
    options.format       = format;
    options.project_root = root;
    options.project_name = "vectis-test";
    return options;
}

TEST(DigestExporterTest, Json_WellFormed)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);

    // Must be valid JSON.
    auto parsed = nlohmann::json::parse(content);

    EXPECT_EQ(parsed["vectis_version"], "0.1.0");
    EXPECT_TRUE(parsed.contains("generated_at"));
    EXPECT_EQ(parsed["project"]["name"], "vectis-test");
    EXPECT_EQ(parsed["project"]["root"], "/fake/project");
    EXPECT_EQ(parsed["project"]["file_count"], 2);
    EXPECT_EQ(parsed["project"]["symbol_count"], 6);
    EXPECT_EQ(parsed["project"]["languages"].size(), 1U);
    EXPECT_EQ(parsed["project"]["languages"][0], "C++");

    ASSERT_EQ(parsed["files"].size(), 2U);
    // Files are sorted by path → src/core/app.cpp comes before src/modes/code/scanner.cpp
    EXPECT_EQ(parsed["files"][0]["path"], "src/core/app.cpp");
    EXPECT_EQ(parsed["files"][0]["language"], "C++");
    EXPECT_EQ(parsed["files"][0]["size"], 15234);
    EXPECT_EQ(parsed["files"][0]["lines"], 425);

    const auto& symbols = parsed["files"][0]["symbols"];
    // file_id f1 has: App, initialize, run, ErrorKind = 4 symbols
    ASSERT_EQ(symbols.size(), 4U);

    // Find the initialize method and check its signature made it through.
    bool saw_initialize_signature = false;
    bool saw_errorkind_members    = false;
    for (const auto& sym : symbols) {
        if (sym["name"] == "initialize") {
            ASSERT_TRUE(sym.contains("signature"));
            EXPECT_EQ(sym["signature"], "bool App::initialize()");
            saw_initialize_signature = true;
        }
        if (sym["name"] == "ErrorKind") {
            ASSERT_TRUE(sym.contains("members"));
            EXPECT_EQ(sym["members"].size(), 3U);
            EXPECT_EQ(sym["members"][0], "IoError");
            saw_errorkind_members = true;
        }
    }
    EXPECT_TRUE(saw_initialize_signature);
    EXPECT_TRUE(saw_errorkind_members);
}

TEST(DigestExporterTest, Json_TopLevelSymbolsArrayMatchesCount)
{
    // Regression guard for the "project.symbol_count claimed N but
    // digest['symbols'] returned []" discrepancy: the top-level
    // `symbols` array must exist in full JSON and carry an entry for
    // every symbol in the index.
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);
    const auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("symbols"));
    const auto& top_symbols = parsed["symbols"];
    ASSERT_TRUE(top_symbols.is_array());
    EXPECT_EQ(top_symbols.size(), parsed["project"]["symbol_count"].get<std::size_t>());

    // Each entry carries at least {name, kind, path, line}.
    for (const auto& s : top_symbols) {
        EXPECT_TRUE(s.contains("name"));
        EXPECT_TRUE(s.contains("kind"));
        EXPECT_TRUE(s.contains("path"));
        EXPECT_TRUE(s.contains("line"));
    }
}

TEST(DigestExporterTest, SlimJson_OmitsSizeLinesAndSymbols)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);

    auto parsed = nlohmann::json::parse(content);

    // Top-level project block is still there.
    EXPECT_EQ(parsed["project"]["file_count"], 2);
    EXPECT_EQ(parsed["project"]["symbol_count"], 6);

    // Per-file entries only have path + language.
    ASSERT_EQ(parsed["files"].size(), 2U);
    const auto& first = parsed["files"][0];
    EXPECT_TRUE(first.contains("path"));
    EXPECT_TRUE(first.contains("language"));
    EXPECT_FALSE(first.contains("size"));
    EXPECT_FALSE(first.contains("lines"));
    EXPECT_FALSE(first.contains("symbols"));
}

TEST(DigestExporterTest, SlimJson_IsSmallerThanFullJson)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const std::string full = build_digest_string(
        index, make_options(DigestFormat::Json,     "/fake/project"));
    const std::string slim = build_digest_string(
        index, make_options(DigestFormat::SlimJson, "/fake/project"));

    // The tiny 4-symbol fixture doesn't give slim format much room to
    // shine, but it should still be at least 30% smaller than full.
    EXPECT_LT(slim.size(), static_cast<std::size_t>(static_cast<double>(full.size()) * 0.7))
        << "slim=" << slim.size() << " full=" << full.size();
}

TEST(DigestExporterTest, Markdown_ContainsHeadingsAndSymbolLines)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::Markdown, "/fake/project");
    const std::string content = build_digest_string(index, options);

    EXPECT_NE(content.find("# vectis-test"),                          std::string::npos);
    EXPECT_NE(content.find("## Overview"),                            std::string::npos);
    EXPECT_NE(content.find("## Files"),                               std::string::npos);
    EXPECT_NE(content.find("- Files: 2"),                             std::string::npos);
    EXPECT_NE(content.find("- Symbols: 6"),                           std::string::npos);
    EXPECT_NE(content.find("### src/core/app.cpp"),                   std::string::npos);
    EXPECT_NE(content.find("### src/modes/code/scanner.cpp"),         std::string::npos);
    EXPECT_NE(content.find("`App`"),                                  std::string::npos);
    EXPECT_NE(content.find("`Scanner`"),                              std::string::npos);
    EXPECT_NE(content.find("line 13"),                                std::string::npos);
}

TEST(DigestExporterTest, Markdown_RendersSignaturesAndMembers)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::Markdown, "/fake/project");
    const std::string content = build_digest_string(index, options);

    // Methods with signatures should be rendered with the full
    // signature as the bullet's code span instead of just the name.
    EXPECT_NE(content.find("`bool App::initialize()` (method)"), std::string::npos);
    EXPECT_NE(content.find("`int App::run()` (method)"),         std::string::npos);

    // Classes without signatures fall back to the bare name.
    EXPECT_NE(content.find("`App` (class)"),     std::string::npos);
    EXPECT_NE(content.find("`Scanner` (class)"), std::string::npos);

    // Enum values rendered as an indented comma-separated sub-bullet.
    EXPECT_NE(content.find("`ErrorKind` (enum)"),                            std::string::npos);
    EXPECT_NE(content.find("`IoError`, `ParseError`, `NetworkError`"),       std::string::npos);

    // Struct fields rendered similarly.
    EXPECT_NE(content.find("`Point` (struct)"), std::string::npos);
    EXPECT_NE(content.find("`x`, `y`"),         std::string::npos);
}

TEST(DigestExporterTest, Export_WritesFileAtDefaultLocation)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Use a real temp directory as the project root so the write path
    // is actually exercised.
    const auto root = std::filesystem::temp_directory_path() /
                      "vectis_digest_export_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const ExportOptions options = make_options(DigestFormat::Json, root);
    const auto result = export_digest(index, options);
    ASSERT_TRUE(result.has_value());

    const auto written_path = *result;
    EXPECT_EQ(written_path, root / "vectis-digest.json");
    ASSERT_TRUE(std::filesystem::exists(written_path));

    // Read it back and confirm it parses as JSON.
    std::ifstream stream(written_path);
    const std::string content((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    auto parsed = nlohmann::json::parse(content);
    EXPECT_EQ(parsed["project"]["file_count"], 2);

    std::filesystem::remove_all(root);
}

TEST(DigestExporterTest, Export_ForwardSlashesInPaths)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Markdown renders the relative paths inline — easiest place to
    // verify no backslashes creep in on any platform.
    const ExportOptions options = make_options(DigestFormat::Markdown, "/fake/project");
    const std::string content = build_digest_string(index, options);
    EXPECT_EQ(content.find('\\'), std::string::npos)
        << "backslashes leaked into markdown output";
}

TEST(DigestExporterTest, DefaultOutputPath_ForEachFormat)
{
    const std::filesystem::path root = "/tmp/vectis-project-x";
    EXPECT_EQ(default_output_path(root, DigestFormat::Json),
              root / "vectis-digest.json");
    EXPECT_EQ(default_output_path(root, DigestFormat::Markdown),
              root / "vectis-digest.md");
    EXPECT_EQ(default_output_path(root, DigestFormat::SlimJson),
              root / "vectis-digest-slim.json");
}

TEST(DigestExporterTest, Json_ContainsDependencyGraphAndHotspots)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Register a dependency so the graph isn't empty.
    Dependency dep;
    dep.source_file_id = 1;  // app.cpp
    dep.target_file_id = 2;  // scanner.cpp
    dep.import_string  = "scanner.h";
    dep.kind           = "include";
    index.add_dependency(std::move(dep));

    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    // dependency_graph block exists with the expected shape.
    ASSERT_TRUE(parsed.contains("dependency_graph"));
    const auto& graph = parsed["dependency_graph"];
    ASSERT_TRUE(graph.contains("edges"));
    ASSERT_TRUE(graph.contains("stats"));
    ASSERT_TRUE(graph.contains("cycles"));
    EXPECT_EQ(graph["stats"]["total_edges"], 1);
    EXPECT_EQ(graph["stats"]["internal_edges"], 1);

    // hotspots array exists (empty for this synthetic index).
    ASSERT_TRUE(parsed.contains("hotspots"));
    EXPECT_TRUE(parsed["hotspots"].is_array());

    // architecture block exists.
    ASSERT_TRUE(parsed.contains("architecture"));
    EXPECT_TRUE(parsed["architecture"].contains("label"));
    EXPECT_TRUE(parsed["architecture"].contains("confidence"));

    // project block grew a dependency_count.
    EXPECT_EQ(parsed["project"]["dependency_count"], 1);
}

TEST(DigestExporterTest, SlimJson_IncludesArchitectureAndCompactHotspots)
{
    // Slim previously dropped architecture + hotspots entirely, which
    // left consumers only with file paths + deps — not enough for an
    // agent's orientation pass. Slim now keeps both, with hotspots
    // capped at top 10 and stripped of excerpts so they stay cheap.
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("architecture"));
    EXPECT_TRUE(parsed["architecture"].contains("label"));
    EXPECT_TRUE(parsed["architecture"].contains("confidence"));

    ASSERT_TRUE(parsed.contains("hotspots"));
    EXPECT_TRUE(parsed["hotspots"].is_array());
    EXPECT_LE(parsed["hotspots"].size(), 10U); // capped
    for (const auto& h : parsed["hotspots"]) {
        EXPECT_FALSE(h.contains("excerpt"));    // excerpts are full-format-only
    }

    // Symbols stay full-format-only to keep slim token-cheap.
    EXPECT_FALSE(parsed.contains("symbols"));

    EXPECT_TRUE(parsed.contains("dependency_graph"));
}

TEST(DigestExporterTest, Markdown_ContainsDependencyGraphAndHotspotSections)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::Markdown, "/fake/project");
    const std::string content = build_digest_string(index, options);

    EXPECT_NE(content.find("## Architecture"),   std::string::npos);
    EXPECT_NE(content.find("## Hotspots"),       std::string::npos);
    EXPECT_NE(content.find("## Dependency Graph"), std::string::npos);
}

} // namespace
