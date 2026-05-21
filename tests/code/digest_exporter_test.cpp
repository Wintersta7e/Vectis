#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/digest_exporter.h"
#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::build_digest_string;
using vectis::code::CodeIndex;
using vectis::code::default_output_path;
using vectis::code::Dependency;
using vectis::code::DigestFormat;
using vectis::code::export_digest;
using vectis::code::ExportOptions;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::Visibility;

std::int64_t add_cpp_file(CodeIndex& index, const std::string& path, int line_count)
{
    FileEntry f;
    f.path_relative = path;
    f.language = Language::Cpp;
    f.line_count = line_count;
    return index.add_file(std::move(f));
}

/// Populate a small synthetic index with two files and four symbols.
/// Used by every test in this file so the assertions share a fixture.
void populate_synthetic_index(CodeIndex& index)
{
    FileEntry f1;
    f1.path_relative = "src/core/app.cpp";
    f1.language = Language::Cpp;
    f1.size = 15234;
    f1.line_count = 425;
    const std::int64_t f1_id = index.add_file(std::move(f1));

    FileEntry f2;
    f2.path_relative = "src/scan/scanner.cpp";
    f2.language = Language::Cpp;
    f2.size = 9123;
    f2.line_count = 280;
    const std::int64_t f2_id = index.add_file(std::move(f2));

    // Designated init: future-proof against new Symbol fields.
    const std::array<Symbol, 6> batch = {
        Symbol{.file_id = f1_id,
               .name = "App",
               .kind = SymbolKind::Class,
               .line_start = 13,
               .line_end = 460},
        Symbol{.file_id = f1_id,
               .name = "initialize",
               .kind = SymbolKind::Method,
               .line_start = 109,
               .line_end = 256,
               .signature = "bool App::initialize()"},
        Symbol{.file_id = f1_id,
               .name = "run",
               .kind = SymbolKind::Method,
               .line_start = 258,
               .line_end = 300,
               .signature = "int App::run()"},
        Symbol{.file_id = f2_id,
               .name = "Scanner",
               .kind = SymbolKind::Class,
               .line_start = 52,
               .line_end = 280},
        Symbol{.file_id = f1_id,
               .name = "ErrorKind",
               .kind = SymbolKind::Enum,
               .line_start = 14,
               .line_end = 22,
               .members = {"IoError", "ParseError", "NetworkError"}},
        Symbol{.file_id = f2_id,
               .name = "Point",
               .kind = SymbolKind::Struct,
               .line_start = 30,
               .line_end = 34,
               .members = {"x", "y"}},
    };
    index.add_symbols(batch);
}

ExportOptions make_options(DigestFormat format, const std::filesystem::path& root)
{
    ExportOptions options;
    options.format = format;
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

    // Full JSON must never carry slim-only fields.
    EXPECT_FALSE(parsed.contains("_schema"));
    EXPECT_FALSE(parsed.contains("encoding"));
    EXPECT_FALSE(parsed.contains("languages")); // top-level only in slim; full uses project.languages
    EXPECT_EQ(parsed["vectis_version"], "0.1.0");
    // Digest must be deterministic — no timestamps, no environment-derived
    // fields. Same input + same binary → byte-identical JSON.
    EXPECT_FALSE(parsed.contains("generated_at"));
    EXPECT_EQ(parsed["project"]["name"], "vectis-test");
    EXPECT_EQ(parsed["project"]["root"], "/fake/project");
    EXPECT_EQ(parsed["project"]["file_count"], 2);
    EXPECT_EQ(parsed["project"]["symbol_count"], 6);
    EXPECT_EQ(parsed["project"]["languages"].size(), 1U);
    EXPECT_EQ(parsed["project"]["languages"][0], "C++");

    ASSERT_EQ(parsed["files"].size(), 2U);
    // Files are sorted by path → src/core/app.cpp comes before src/scan/scanner.cpp
    EXPECT_EQ(parsed["files"][0]["path"], "src/core/app.cpp");
    EXPECT_EQ(parsed["files"][0]["language"], "C++");
    EXPECT_EQ(parsed["files"][0]["size"], 15234);
    EXPECT_EQ(parsed["files"][0]["lines"], 425);

    const auto& symbols = parsed["files"][0]["symbols"];
    // file_id f1 has: App, initialize, run, ErrorKind = 4 symbols
    ASSERT_EQ(symbols.size(), 4U);

    // Find the initialize method and check its signature made it through.
    bool saw_initialize_signature = false;
    bool saw_errorkind_members = false;
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

TEST(DigestExporterTest, Json_HandlesNonUtf8Bytes)
{
    // Legacy iso-8859-1 source files (German umlauts, Latin-1 strings)
    // produce stray bytes like 0xFC that are not valid UTF-8 standalone.
    // Before the error_handler_t::replace fix, nlohmann threw type_error.316
    // and SIGABRTed the whole digest. Now the byte is replaced with U+FFFD.
    CodeIndex index;
    FileEntry file;
    file.path_relative = "src/legacy.js";
    file.language = Language::JavaScript;
    file.size = 1024;
    file.line_count = 10;
    const std::int64_t file_id = index.add_file(std::move(file));

    // 0xFC = 'ü' in iso-8859-1; not a valid UTF-8 sequence by itself.
    Symbol sym{
        .file_id = file_id,
        .name = std::string{"M\xFCller"},
        .kind = SymbolKind::Function,
        .line_start = 1,
        .line_end = 2,
        .signature = std::string{"function M\xFCller()"},
    };
    index.add_symbols(std::array<Symbol, 1>{sym});

    const ExportOptions options = make_options(DigestFormat::Json, "/fake");
    std::string content;
    ASSERT_NO_THROW({ content = build_digest_string(index, options); });
    ASSERT_FALSE(content.empty());

    // Output is parseable JSON despite the original bad byte.
    auto parsed = nlohmann::json::parse(content);
    EXPECT_EQ(parsed["project"]["symbol_count"], 1);

    // U+FFFD encoded in UTF-8 is EF BF BD; appears where the invalid byte was.
    EXPECT_NE(content.find("\xEF\xBF\xBD"), std::string::npos);
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

    // Per-file entries use the slim v2 shape.
    ASSERT_EQ(parsed["files"].size(), 2U);
    const auto& first = parsed["files"][0];
    EXPECT_TRUE(first.contains("id"));
    EXPECT_TRUE(first.contains("path"));
    EXPECT_TRUE(first.contains("lang"));
    EXPECT_FALSE(first.contains("language"));
    EXPECT_FALSE(first.contains("size"));
    EXPECT_FALSE(first.contains("lines"));
    EXPECT_FALSE(first.contains("symbols"));
}

TEST(DigestExporterTest, SlimJson_HasSchemaHeader)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("_schema")) << "slim v2 must carry a _schema header";
    const auto& schema = parsed["_schema"];
    EXPECT_EQ(schema["name"], "vectis.slim");
    EXPECT_EQ(schema["version"], 2);
    ASSERT_TRUE(schema.contains("edge_tuple"));
    ASSERT_TRUE(schema["edge_tuple"].is_array());
    EXPECT_EQ(schema["edge_tuple"].size(), 4U);
    EXPECT_TRUE(schema.contains("edge_semantics"));
    EXPECT_TRUE(schema.contains("cycle_semantics"));
    EXPECT_TRUE(schema.contains("file_id_semantics"));
}

TEST(DigestExporterTest, SlimJson_HasEncodingBlock)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("encoding"));
    const auto& enc = parsed["encoding"];
    EXPECT_EQ(enc["edge_format"], "tuple-v1");
    // files count matches synthetic index (2 files). Other table counts
    // (kinds, refs) are zero until those tables exist; languages is wired.
    EXPECT_EQ(enc["files"], 2);
    EXPECT_TRUE(enc["languages"].is_number_integer());
    EXPECT_TRUE(enc["kinds"].is_number_integer());
    EXPECT_TRUE(enc["refs"].is_number_integer());
}

TEST(DigestExporterTest, SlimJson_FilesUseLangIndex)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    // Top-level languages table is present and sorted alphabetically.
    ASSERT_TRUE(parsed.contains("languages"));
    const auto langs = parsed["languages"].get<std::vector<std::string>>();
    ASSERT_FALSE(langs.empty());
    EXPECT_TRUE(std::is_sorted(langs.begin(), langs.end()));

    // Each file entry has integer `lang` (not string `language`) and an id.
    ASSERT_EQ(parsed["files"].size(), 2U);
    for (const auto& f : parsed["files"]) {
        EXPECT_TRUE(f.contains("id"));
        EXPECT_TRUE(f.contains("path"));
        EXPECT_TRUE(f.contains("lang"));
        EXPECT_TRUE(f["lang"].is_number_integer());
        EXPECT_FALSE(f.contains("language")) << "slim v2 dropped the string `language` key";
        const int idx = f["lang"].get<int>();
        ASSERT_GE(idx, 0);
        ASSERT_LT(static_cast<std::size_t>(idx), langs.size());
        EXPECT_EQ(langs[static_cast<std::size_t>(idx)], "C++");
    }

    // `encoding.languages` count must match the table length.
    EXPECT_EQ(parsed["encoding"]["languages"], langs.size());
}

TEST(DigestExporterTest, SlimJson_UnknownLanguageGetsSentinelLang)
{
    // distinct_language_names excludes Language::Unknown by design.
    // Slim emits -1 for any file that doesn't resolve into the table —
    // an agent consumer must handle the sentinel without crashing.
    CodeIndex index;
    FileEntry f;
    f.path_relative = "build/generated.bin";
    f.language = Language::Unknown;
    f.line_count = 0;
    index.add_file(std::move(f));

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    // No known-language files -> table is empty.
    ASSERT_TRUE(parsed.contains("languages"));
    EXPECT_TRUE(parsed["languages"].empty());
    ASSERT_EQ(parsed["files"].size(), 1U);
    EXPECT_EQ(parsed["files"][0]["lang"], -1);
}

TEST(DigestExporterTest, SlimJson_IsSmallerThanFullJson)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const std::string full =
        build_digest_string(index, make_options(DigestFormat::Json, "/fake/project"));
    const std::string slim =
        build_digest_string(index, make_options(DigestFormat::SlimJson, "/fake/project"));

    // The tiny 4-symbol fixture doesn't give slim format much room to
    // shine, but it should still be at least 30% smaller than full.
    EXPECT_LT(slim.size(), static_cast<std::size_t>(static_cast<double>(full.size()) * 0.7))
        << "slim=" << slim.size() << " full=" << full.size();
}

TEST(DigestExporterTest, Export_WritesFileAtDefaultLocation)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Use a real temp directory as the project root so the write path
    // is actually exercised.
    const auto root = std::filesystem::temp_directory_path() / "vectis_digest_export_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const ExportOptions options = make_options(DigestFormat::Json, root);
    const auto result = export_digest(index, options);
    ASSERT_TRUE(result.has_value());

    const auto& written_path = *result;
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

    // Path values inside the JSON document must use forward slashes on
    // every platform so an agent comparing across runs / OS doesn't
    // see Windows-style separators.
    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);
    for (const auto& f : parsed["files"]) {
        const std::string p = f["path"].get<std::string>();
        EXPECT_EQ(p.find('\\'), std::string::npos) << "backslash in file path: " << p;
    }
}

TEST(DigestExporterTest, DefaultOutputPath_ForEachFormat)
{
    const std::filesystem::path root = "/tmp/vectis-project-x";
    EXPECT_EQ(default_output_path(root, DigestFormat::Json), root / "vectis-digest.json");
    EXPECT_EQ(default_output_path(root, DigestFormat::SlimJson), root / "vectis-digest-slim.json");
}

TEST(DigestExporterTest, Json_ContainsDependencyGraphAndHotspots)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Register a dependency so the graph isn't empty.
    Dependency dep;
    dep.source_file_id = 1; // app.cpp
    dep.target_file_id = 2; // scanner.cpp
    dep.import_string = "scanner.h";
    dep.kind = "include";
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

    // architecture block exists with structured (machine-readable)
    // fields only — `reasoning` (prose) is human-only and lives on the
    // struct, not in the JSON.
    ASSERT_TRUE(parsed.contains("architecture"));
    EXPECT_TRUE(parsed["architecture"].contains("label"));
    EXPECT_TRUE(parsed["architecture"].contains("confidence"));
    EXPECT_TRUE(parsed["architecture"].contains("signals"));
    EXPECT_TRUE(parsed["architecture"]["signals"].is_array());
    EXPECT_FALSE(parsed["architecture"].contains("reasoning"));

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
    EXPECT_TRUE(parsed["architecture"].contains("signals"));
    EXPECT_FALSE(parsed["architecture"].contains("reasoning"));

    ASSERT_TRUE(parsed.contains("hotspots"));
    EXPECT_TRUE(parsed["hotspots"].is_array());
    EXPECT_LE(parsed["hotspots"].size(), 10U); // capped
    for (const auto& h : parsed["hotspots"]) {
        EXPECT_FALSE(h.contains("excerpt")); // excerpts are full-format-only
    }

    // Symbols stay full-format-only to keep slim token-cheap.
    EXPECT_FALSE(parsed.contains("symbols"));

    // The full `cycles` array stays full-format-only (rarely useful
    // for first-pass orientation), but a `stats.cycles` count is
    // emitted in both formats so slim consumers can flag tangled
    // graphs without parsing the array. External edges also show up
    // in slim — agents need the third-party dep landscape.
    ASSERT_TRUE(parsed.contains("dependency_graph"));
    EXPECT_FALSE(parsed["dependency_graph"].contains("cycles"));
    ASSERT_TRUE(parsed["dependency_graph"]["stats"].contains("cycles"));
    EXPECT_TRUE(parsed["dependency_graph"]["stats"]["cycles"].is_number_unsigned());
}

TEST(DigestExporterTest, Hotspots_EmitStructuredDrivers)
{
    // Agents consume hotspot numbers as structured fields, not by
    // regex-parsing `reason`. Symbol-level entries carry name/line/
    // kind/complexity; file-level entries carry the trigger(s) that
    // actually fired.
    CodeIndex index;
    FileEntry f;
    f.path_relative = "src/gnarly.cpp";
    f.language = Language::Cpp;
    f.size = 12000;
    f.line_count = 800; // > default 500 → large-file hotspot too
    const std::int64_t fid = index.add_file(std::move(f));

    Symbol gnarly{.file_id = fid,
                  .name = "gnarly",
                  .kind = SymbolKind::Method,
                  .line_start = 42,
                  .line_end = 200,
                  .complexity = 50};
    const std::array<Symbol, 1> batch = {gnarly};
    index.add_symbols(batch);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("hotspots"));
    ASSERT_FALSE(parsed["hotspots"].empty());

    // Find the complexity-driven entry — sort is by severity, then
    // alphabetical reason, so the symbol hotspot may not be first.
    bool saw_complexity = false;
    bool saw_large_file = false;
    for (const auto& h : parsed["hotspots"]) {
        if (h.contains("complexity")) {
            saw_complexity = true;
            EXPECT_EQ(h["complexity"], 50);
            EXPECT_EQ(h["name"], "gnarly");
            EXPECT_EQ(h["line"], 42);
            EXPECT_EQ(h["kind"], "method");
            EXPECT_TRUE(h.contains("symbol_id"));
        }
        if (h.contains("line_count")) {
            saw_large_file = true;
            EXPECT_EQ(h["line_count"], 800);
            // File-level hotspots don't carry a symbol locator.
            EXPECT_FALSE(h.contains("name"));
            EXPECT_FALSE(h.contains("symbol_id"));
        }
    }
    EXPECT_TRUE(saw_complexity);
    EXPECT_TRUE(saw_large_file);
}

TEST(DigestExporterTest, BothFormats_StatsCarryCycleCount)
{
    // Two files with mutual deps form a single 2-cycle. Both slim and
    // full digests must surface `stats.cycles == 1`.
    CodeIndex index;
    FileEntry a;
    a.path_relative = "a.cpp";
    a.language = Language::Cpp;
    const std::int64_t a_id = index.add_file(std::move(a));
    FileEntry b;
    b.path_relative = "b.cpp";
    b.language = Language::Cpp;
    const std::int64_t b_id = index.add_file(std::move(b));

    Dependency a_to_b;
    a_to_b.source_file_id = a_id;
    a_to_b.target_file_id = b_id;
    a_to_b.import_string = "b.h";
    a_to_b.kind = "include";
    index.add_dependency(std::move(a_to_b));
    Dependency b_to_a;
    b_to_a.source_file_id = b_id;
    b_to_a.target_file_id = a_id;
    b_to_a.import_string = "a.h";
    b_to_a.kind = "include";
    index.add_dependency(std::move(b_to_a));

    for (const DigestFormat fmt : {DigestFormat::SlimJson, DigestFormat::Json}) {
        const ExportOptions options = make_options(fmt, "/fake/project");
        const auto parsed = nlohmann::json::parse(build_digest_string(index, options));
        const auto& stats = parsed["dependency_graph"]["stats"];
        ASSERT_TRUE(stats.contains("cycles")) << "format=" << static_cast<int>(fmt);
        EXPECT_EQ(stats["cycles"], 1U) << "format=" << static_cast<int>(fmt);
    }
}

TEST(DigestExporterTest, SlimJson_CarriesExternalEdges)
{
    // Earlier slim filtered externals out entirely. That left agents
    // with `stats.external_edges = N` but `edges[]` shorter than N —
    // an inconsistent schema. Slim now emits external edges with the
    // same shape full does (target=null, target_external=<raw>).
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency internal_dep;
    internal_dep.source_file_id = 1;
    internal_dep.target_file_id = 2;
    internal_dep.import_string = "scanner.h";
    internal_dep.kind = "include";
    index.add_dependency(std::move(internal_dep));

    Dependency external_dep;
    external_dep.source_file_id = 1;
    external_dep.target_file_id = 0; // unresolved → external
    external_dep.import_string = "<vector>";
    external_dep.kind = "include";
    index.add_dependency(std::move(external_dep));

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 2U) << "slim must emit both internal and external edges";

    // Find the external edge — target=null, target_external=raw string.
    bool saw_external = false;
    bool saw_internal = false;
    for (const auto& e : edges) {
        if (e["target"].is_null()) {
            saw_external = true;
            EXPECT_EQ(e["target_external"], "<vector>");
            EXPECT_EQ(e["kind"], "include");
            EXPECT_FALSE(e.contains("import_ref"))
                << "external edges already carry the raw import in target_external; "
                << "import_ref would be redundant";
        }
        else {
            saw_internal = true;
            EXPECT_FALSE(e.contains("target_external"));
            ASSERT_TRUE(e.contains("import_ref"))
                << "internal edge with non-empty import_string must carry import_ref";
            EXPECT_EQ(e["import_ref"], "scanner.h");
        }
    }
    EXPECT_TRUE(saw_external);
    EXPECT_TRUE(saw_internal);

    EXPECT_EQ(parsed["dependency_graph"]["stats"]["total_edges"], 2);
    EXPECT_EQ(parsed["dependency_graph"]["stats"]["internal_edges"], 1);
    EXPECT_EQ(parsed["dependency_graph"]["stats"]["external_edges"], 1);
}

TEST(DigestExporterTest, Json_OmitsImportRefWhenImportStringEmpty)
{
    // Some resolved edges might be registered with an empty
    // import_string (synthetic / introspective). The exporter must
    // skip import_ref rather than emit an empty value.
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency dep;
    dep.source_file_id = 1;
    dep.target_file_id = 2;
    dep.import_string = ""; // intentionally empty
    dep.kind = "include";
    index.add_dependency(std::move(dep));

    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_FALSE(edges[0].contains("import_ref"))
        << "empty import_string must not produce an empty import_ref field";
}

TEST(DigestExporterTest, SlimJson_DiversifiesHotspotBuckets)
{
    // Big-codebase regression: with many fan-in hubs, the slim top-10
    // used to be entirely "high fan-in" entries because they all tie at
    // severity 3 and lex-sort first within the file bucket. The slim
    // picker now reserves a quota per dimension so complexity, fan-in,
    // fan-out, and size each get representation when present.
    CodeIndex index;

    std::vector<std::int64_t> hub_ids;
    hub_ids.reserve(20);
    for (int i = 0; i < 20; ++i) {
        hub_ids.push_back(add_cpp_file(index, "src/hub_" + std::to_string(i) + ".h", 50));
    }
    std::vector<std::int64_t> dep_ids;
    dep_ids.reserve(35);
    for (int i = 0; i < 35; ++i) {
        dep_ids.push_back(add_cpp_file(index, "src/dep_" + std::to_string(i) + ".cpp", 100));
    }
    std::vector<Dependency> deps;
    deps.reserve(hub_ids.size() * dep_ids.size() + 16);
    for (std::int64_t hub : hub_ids) {
        for (std::int64_t dep : dep_ids) {
            deps.push_back(Dependency{.source_file_id = dep,
                                      .target_file_id = hub,
                                      .import_string = "",
                                      .kind = "include"});
        }
    }
    const std::int64_t fan_out_id = add_cpp_file(index, "src/wide.cpp", 100);
    for (int i = 0; i < 16; ++i) {
        deps.push_back(Dependency{.source_file_id = fan_out_id,
                                  .target_file_id = hub_ids[i],
                                  .import_string = "",
                                  .kind = "include"});
    }
    index.add_dependencies(deps);

    (void)add_cpp_file(index, "src/big.cpp", 1500);

    const std::int64_t host_id = add_cpp_file(index, "src/host.cpp", 100);
    const std::array<Symbol, 1> batch = {Symbol{.file_id = host_id,
                                                .name = "gnarly",
                                                .kind = SymbolKind::Method,
                                                .line_start = 10,
                                                .line_end = 90,
                                                .complexity = 60}};
    index.add_symbols(batch);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    ASSERT_TRUE(parsed.contains("hotspots"));
    const auto& hs = parsed["hotspots"];
    ASSERT_LE(hs.size(), 10U);

    bool has_complexity = false;
    bool has_fan_in = false;
    bool has_fan_out = false;
    bool has_size = false;
    for (const auto& h : hs) {
        if (h.contains("complexity") && h.contains("symbol_id")) {
            has_complexity = true;
        }
        if (h.contains("fan_in")) {
            has_fan_in = true;
        }
        if (h.contains("fan_out")) {
            has_fan_out = true;
        }
        if (h.contains("line_count")) {
            has_size = true;
        }
    }
    EXPECT_TRUE(has_complexity) << "complexity dimension missing from slim top-N";
    EXPECT_TRUE(has_fan_in) << "fan-in dimension missing from slim top-N";
    EXPECT_TRUE(has_fan_out) << "fan-out dimension missing from slim top-N";
    EXPECT_TRUE(has_size) << "size dimension missing from slim top-N";
}

} // namespace
