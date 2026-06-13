#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

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

/// Build a slim digest from `index` and parse it. Centralises the
/// three-line make_options / build_digest_string / parse pattern used
/// by most slim tests.
nlohmann::json slim_parse(CodeIndex& index)
{
    return nlohmann::json::parse(
        build_digest_string(index, make_options(DigestFormat::SlimJson, "/fake/project")));
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
    EXPECT_FALSE(
        parsed.contains("languages"));      // top-level only in slim; full uses project.languages
    EXPECT_FALSE(parsed.contains("kinds")); // top-level only in slim
    EXPECT_FALSE(parsed.contains("refs"));  // top-level only in slim
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

TEST(DigestExporterTest, Json_ProjectLanguagesSortedByFileCount)
{
    // Three Python files, one C++ file. Count order is Python, C++ — the
    // reverse of the alphabetical order the slim table uses, so this fails
    // if `project.languages` ever regresses to an alphabetical sort.
    CodeIndex index;
    auto add = [&index](const std::string& path, Language lang) {
        FileEntry f;
        f.path_relative = path;
        f.language = lang;
        index.add_file(std::move(f));
    };
    add("a.cpp", Language::Cpp);
    add("b.py", Language::Python);
    add("c.py", Language::Python);
    add("d.py", Language::Python);

    const auto parsed = nlohmann::json::parse(
        build_digest_string(index, make_options(DigestFormat::Json, "/fake/project")));

    const auto langs = parsed["project"]["languages"].get<std::vector<std::string>>();
    ASSERT_EQ(langs.size(), 2U);
    EXPECT_EQ(langs[0], "Python"); // 3 files — dominant, leads despite > "C++"
    EXPECT_EQ(langs[1], "C++");    // 1 file
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

    auto parsed = slim_parse(index);

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

    auto parsed = slim_parse(index);

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

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed.contains("encoding"));
    const auto& enc = parsed["encoding"];
    EXPECT_EQ(enc["edge_format"], "tuple-v1");
    // files count matches synthetic index (2 files).
    // Files / languages / kinds / refs all wired; no placeholder zeros remain.
    EXPECT_EQ(enc["files"], 2);
    EXPECT_TRUE(enc["languages"].is_number_integer());
    EXPECT_TRUE(enc["kinds"].is_number_integer());
    EXPECT_TRUE(enc["refs"].is_number_integer());
}

TEST(DigestExporterTest, SlimJson_FilesUseLangIndex)
{
    CodeIndex index;
    populate_synthetic_index(index);

    auto parsed = slim_parse(index);

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

    auto parsed = slim_parse(index);

    // No known-language files -> table is empty.
    ASSERT_TRUE(parsed.contains("languages"));
    EXPECT_TRUE(parsed["languages"].empty());
    ASSERT_EQ(parsed["files"].size(), 1U);
    EXPECT_EQ(parsed["files"][0]["lang"], -1);
}

TEST(DigestExporterTest, SlimJson_HasKindsTable)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Register two deps with distinct kinds so the table isn't empty.
    Dependency a;
    a.source_file_id = 1;
    a.target_file_id = 2;
    a.kind = "include";
    Dependency b;
    b.source_file_id = 2;
    b.target_file_id = 0; // external
    b.kind = "import";
    b.import_string = "ext/lib.h";

    Dependency empty_kind;
    empty_kind.source_file_id = 1;
    empty_kind.target_file_id = 0;
    // empty kind on purpose — must NOT land in the kinds table.

    const std::array<Dependency, 3> batch = {a, b, empty_kind};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed.contains("kinds"));
    const auto kinds = parsed["kinds"].get<std::vector<std::string>>();
    ASSERT_EQ(kinds.size(), 2U);
    EXPECT_TRUE(std::is_sorted(kinds.begin(), kinds.end()));
    EXPECT_EQ(kinds[0], "import");
    EXPECT_EQ(kinds[1], "include");
    EXPECT_EQ(parsed["encoding"]["kinds"], kinds.size());
}

TEST(DigestExporterTest, SlimJson_HasRefsTable)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Three deps where two share the same import_string -> the refs[]
    // table must contain exactly two unique entries.
    Dependency a;
    a.source_file_id = 1;
    a.target_file_id = 2;
    a.kind = "include";
    a.import_string = "scanner.h";
    Dependency b;
    b.source_file_id = 1;
    b.target_file_id = 0;
    b.kind = "import";
    b.import_string = "boost/asio.hpp";
    Dependency c;
    c.source_file_id = 2;
    c.target_file_id = 0;
    c.kind = "import";
    c.import_string = "boost/asio.hpp"; // duplicate
    const std::array<Dependency, 3> batch = {a, b, c};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed.contains("refs"));
    const auto refs = parsed["refs"].get<std::vector<std::string>>();
    ASSERT_EQ(refs.size(), 2U) << "duplicate import_string must dedupe";
    EXPECT_TRUE(std::is_sorted(refs.begin(), refs.end()));
    EXPECT_EQ(refs[0], "boost/asio.hpp");
    EXPECT_EQ(refs[1], "scanner.h");
    EXPECT_EQ(parsed["encoding"]["refs"], refs.size());

    // Every edge's slot[3] is either null or a valid ref_id index.
    const auto& edges = parsed["dependency_graph"]["edges"];
    for (const auto& e : edges) {
        ASSERT_TRUE(e.is_array());
        ASSERT_EQ(e.size(), 4U);
        if (!e[3].is_null()) {
            EXPECT_TRUE(e[3].is_number_integer());
            const int rid = e[3].get<int>();
            ASSERT_GE(rid, 0);
            ASSERT_LT(static_cast<std::size_t>(rid), refs.size());
        }
    }

    // Both deps that share "boost/asio.hpp" must resolve to the same
    // ref_id — that's the whole point of dedup.
    std::vector<int> import_ref_ids;
    const auto kinds = parsed["kinds"].get<std::vector<std::string>>();
    const auto import_idx =
        static_cast<int>(std::find(kinds.begin(), kinds.end(), "import") - kinds.begin());
    for (const auto& e : edges) {
        if (e[2].get<int>() == import_idx) {
            ASSERT_TRUE(e[3].is_number_integer());
            import_ref_ids.push_back(e[3].get<int>());
        }
    }
    ASSERT_EQ(import_ref_ids.size(), 2U);
    EXPECT_EQ(import_ref_ids[0], import_ref_ids[1]);
}

TEST(DigestExporterTest, SlimJson_CyclesAreObjects)
{
    // Build a 3-cycle: 1 -> 2 -> 3 -> 1. A 3-node cycle exercises the
    // closes-the-loop sentinel for non-trivial lengths (fids should
    // be [1, 2, 3, 1] in cycle-traversal order).
    CodeIndex index;
    populate_synthetic_index(index);
    const std::int64_t f3_id = add_cpp_file(index, "src/util/helper.cpp", 120);

    Dependency a;
    a.source_file_id = 1;
    a.target_file_id = 2;
    a.kind = "include";
    Dependency b;
    b.source_file_id = 2;
    b.target_file_id = f3_id;
    b.kind = "include";
    Dependency c;
    c.source_file_id = f3_id;
    c.target_file_id = 1;
    c.kind = "include";
    const std::array<Dependency, 3> batch = {a, b, c};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed["dependency_graph"].contains("cycles"));
    const auto& cycles = parsed["dependency_graph"]["cycles"];
    ASSERT_EQ(cycles.size(), 1U);
    const auto& cy = cycles[0];
    ASSERT_TRUE(cy.is_object()) << "cycles in slim v2 are objects, not arrays";
    EXPECT_FALSE(cy.contains("paths"));
    ASSERT_TRUE(cy.contains("file_ids"));
    const auto fids = cy["file_ids"].get<std::vector<std::int64_t>>();
    // Closes-the-loop: first repeated at end. With 3 distinct nodes,
    // the vector is exactly 4 long.
    ASSERT_EQ(fids.size(), 4U);
    EXPECT_EQ(fids.front(), fids.back());
    // Three distinct ids in positions [0..2]; the same set as the deps.
    const std::set<std::int64_t> unique_ids{fids.begin(), fids.end() - 1};
    EXPECT_EQ(unique_ids.size(), 3U);
}

TEST(DigestExporterTest, SlimJson_StatsHaveByKind)
{
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency inc1;
    inc1.source_file_id = 1;
    inc1.target_file_id = 2;
    inc1.kind = "include";
    Dependency inc2;
    inc2.source_file_id = 2;
    inc2.target_file_id = 1;
    inc2.kind = "include";
    Dependency imp;
    imp.source_file_id = 1;
    imp.target_file_id = 0;
    imp.kind = "import";
    imp.import_string = "third.h";
    const std::array<Dependency, 3> batch = {inc1, inc2, imp};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    const auto& stats = parsed["dependency_graph"]["stats"];
    ASSERT_TRUE(stats.contains("by_kind"));
    EXPECT_EQ(stats["by_kind"]["include"], 2);
    EXPECT_EQ(stats["by_kind"]["import"], 1);
    EXPECT_EQ(stats["total_edges"], 3);
}

TEST(DigestExporterTest, SlimJson_HotspotsHaveFileIdAndPath)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Force at least one hotspot to fire by adding a high-complexity symbol.
    const std::array<Symbol, 1> hot = {
        Symbol{.file_id = 1,
               .name = "huge_func",
               .kind = SymbolKind::Function,
               .line_start = 500,
               .line_end = 800,
               .complexity = 60},
    };
    index.add_symbols(hot);

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed.contains("hotspots"));
    ASSERT_FALSE(parsed["hotspots"].empty());
    for (const auto& h : parsed["hotspots"]) {
        EXPECT_TRUE(h.contains("file_id"));
        EXPECT_TRUE(h["file_id"].is_number_integer());
        EXPECT_TRUE(h.contains("file"));
    }
}

TEST(DigestExporterTest, Json_StatsHaveByKind)
{
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency inc1;
    inc1.source_file_id = 1;
    inc1.target_file_id = 2;
    inc1.kind = "include";
    Dependency inc2;
    inc2.source_file_id = 2;
    inc2.target_file_id = 1;
    inc2.kind = "include";
    Dependency imp;
    imp.source_file_id = 1;
    imp.target_file_id = 0;
    imp.kind = "import";
    imp.import_string = "third.h";
    const std::array<Dependency, 3> batch = {inc1, inc2, imp};
    index.add_dependencies(batch);

    const ExportOptions options = make_options(DigestFormat::Json, "/fake/project");
    const std::string content = build_digest_string(index, options);
    auto parsed = nlohmann::json::parse(content);

    const auto& stats = parsed["dependency_graph"]["stats"];
    ASSERT_TRUE(stats.contains("by_kind"));
    EXPECT_EQ(stats["by_kind"]["include"], 2);
    EXPECT_EQ(stats["by_kind"]["import"], 1);
    EXPECT_EQ(stats["total_edges"], 3);
}

TEST(DigestExporterTest, SlimJson_IsSmallerThanFullJson)
{
    CodeIndex index;
    populate_synthetic_index(index);

    // Add edges so the tuple encoding can do real work. Five deps with
    // duplicate import_strings exercise both the refs[] dedup table
    // and the kinds[] indirection.
    Dependency a;
    a.source_file_id = 1;
    a.target_file_id = 2;
    a.kind = "include";
    a.import_string = "scanner.h";
    Dependency b;
    b.source_file_id = 2;
    b.target_file_id = 1;
    b.kind = "include";
    b.import_string = "app.h";
    Dependency c;
    c.source_file_id = 1;
    c.target_file_id = 0;
    c.kind = "import";
    c.import_string = "boost/asio.hpp";
    Dependency d;
    d.source_file_id = 2;
    d.target_file_id = 0;
    d.kind = "import";
    d.import_string = "boost/asio.hpp"; // duplicate ref
    Dependency e;
    e.source_file_id = 1;
    e.target_file_id = 0;
    e.kind = "import";
    e.import_string = "fmt/format.h";
    const std::array<Dependency, 5> batch = {a, b, c, d, e};
    index.add_dependencies(batch);

    // Add many resolved internal edges so the per-edge encoding (verbose
    // objects in full vs positional tuples in slim) — slim's actual win —
    // dominates the comparison. The `fidelity_metadata` block is a fixed
    // additive cost on both sides and grows with each calibrated language;
    // a toy fixture would let that constant swamp the per-edge savings, so
    // the test measures the property that genuinely scales.
    std::vector<std::int64_t> ids;
    for (int i = 0; i < 60; ++i) {
        FileEntry f;
        f.path_relative = "src/mod" + std::to_string(i) + ".py";
        f.language = Language::Python;
        ids.push_back(index.add_file(std::move(f)));
    }
    std::vector<Dependency> many;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        Dependency dep;
        dep.source_file_id = ids[i];
        dep.target_file_id = ids[i - 1];
        dep.kind = "import";
        dep.import_string = "mod" + std::to_string(i - 1);
        many.push_back(dep);
    }
    index.add_dependencies(many);

    const std::string full =
        build_digest_string(index, make_options(DigestFormat::Json, "/fake/project"));
    const std::string slim =
        build_digest_string(index, make_options(DigestFormat::SlimJson, "/fake/project"));

    // Slim's positional tuples are far more compact than full's object edges;
    // with a representative edge count the win is large and stable regardless
    // of how many languages the fixed metadata block carries.
    EXPECT_LT(slim.size(), static_cast<std::size_t>(static_cast<double>(full.size()) * 0.55))
        << "slim=" << slim.size() << " full=" << full.size();
}

TEST(DigestExporterTest, SlimJson_IsCompact)
{
    CodeIndex index;
    populate_synthetic_index(index);

    const ExportOptions options = make_options(DigestFormat::SlimJson, "/fake/project");
    const std::string content = build_digest_string(index, options);

    // Compact dump: no indentation, no newlines between JSON tokens.
    // We use a coarse heuristic: parse content and ensure nlohmann's
    // own re-dump at indent -1 round-trips byte-identical.
    auto parsed = nlohmann::json::parse(content);
    EXPECT_EQ(content, parsed.dump());
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

    auto parsed = slim_parse(index);

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

    // Slim emits cycles as {"file_ids": [...]} objects (not full path arrays).
    // stats.cycles is always present in both formats.
    ASSERT_TRUE(parsed.contains("dependency_graph"));
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

    auto parsed = slim_parse(index);

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
    // Slim emits external edges as `[source, null, kind_id, ref_id]` so
    // agents see the unresolved-import landscape without parsing two
    // different edge shapes.
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency d;
    d.source_file_id = 1;
    d.target_file_id = 0;
    d.kind = "import";
    d.import_string = "third_party/lib.h";
    const std::array<Dependency, 1> batch = {d};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    const auto refs = parsed["refs"].get<std::vector<std::string>>();
    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 1U) << "slim must emit external edges";
    const auto& edge = edges[0];
    ASSERT_TRUE(edge.is_array());
    ASSERT_EQ(edge.size(), 4U);
    EXPECT_EQ(edge[0].get<std::int64_t>(), 1);
    EXPECT_TRUE(edge[1].is_null());
    // edge[2] is kind_id; edge[3] is the ref_id index into refs[].
    ASSERT_TRUE(edge[3].is_number_integer());
    const auto rid = static_cast<std::size_t>(edge[3].get<int>());
    ASSERT_LT(rid, refs.size());
    EXPECT_EQ(refs[rid], "third_party/lib.h");
}

TEST(DigestExporterTest, SlimJson_EdgesAreTuples)
{
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency internal;
    internal.source_file_id = 1;
    internal.target_file_id = 2;
    internal.kind = "include";
    internal.import_string = "scanner.h";

    Dependency external;
    external.source_file_id = 1;
    external.target_file_id = 0;
    external.kind = "import";
    external.import_string = "boost/asio.hpp";

    const std::array<Dependency, 2> batch = {internal, external};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 2U);

    const auto kinds = parsed["kinds"].get<std::vector<std::string>>();
    const auto refs = parsed["refs"].get<std::vector<std::string>>();
    const auto include_idx =
        static_cast<int>(std::find(kinds.begin(), kinds.end(), "include") - kinds.begin());
    const auto import_idx =
        static_cast<int>(std::find(kinds.begin(), kinds.end(), "import") - kinds.begin());

    for (const auto& e : edges) {
        ASSERT_TRUE(e.is_array());
        ASSERT_EQ(e.size(), 4U);
    }

    bool saw_internal = false;
    bool saw_external = false;
    for (const auto& e : edges) {
        const auto source_id = e[0].get<std::int64_t>();
        if (source_id != 1) {
            continue;
        }
        if (e[1].is_null()) {
            EXPECT_EQ(e[2].get<int>(), import_idx);
            ASSERT_TRUE(e[3].is_number_integer());
            const auto rid = static_cast<std::size_t>(e[3].get<int>());
            ASSERT_LT(rid, refs.size());
            EXPECT_EQ(refs[rid], "boost/asio.hpp");
            saw_external = true;
        }
        else {
            EXPECT_EQ(e[1].get<std::int64_t>(), 2);
            EXPECT_EQ(e[2].get<int>(), include_idx);
            ASSERT_TRUE(e[3].is_number_integer());
            const auto rid = static_cast<std::size_t>(e[3].get<int>());
            ASSERT_LT(rid, refs.size());
            EXPECT_EQ(refs[rid], "scanner.h");
            saw_internal = true;
        }
    }
    EXPECT_TRUE(saw_internal);
    EXPECT_TRUE(saw_external);

    const auto& stats = parsed["dependency_graph"]["stats"];
    EXPECT_EQ(stats["total_edges"], 2);
    EXPECT_EQ(stats["internal_edges"], 1);
    EXPECT_EQ(stats["external_edges"], 1);
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

    auto parsed = slim_parse(index);

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

TEST(DigestExporterTest, SlimJson_CentralFilesUseFileKey)
{
    CodeIndex index;
    populate_synthetic_index(index);

    Dependency d;
    d.source_file_id = 1;
    d.target_file_id = 2;
    d.kind = "include";
    const std::array<Dependency, 1> batch = {d};
    index.add_dependencies(batch);

    auto parsed = slim_parse(index);

    ASSERT_TRUE(parsed.contains("central_files"));
    ASSERT_FALSE(parsed["central_files"].empty());
    for (const auto& cf : parsed["central_files"]) {
        EXPECT_TRUE(cf.contains("file_id"));
        EXPECT_TRUE(cf.contains("file")) << "central_files entries use `file`, not `path`";
        EXPECT_FALSE(cf.contains("path"));
        EXPECT_TRUE(cf.contains("score"));
    }
}

TEST(DigestExporterTest, SlimJson_EmptyProjectHasAllTables)
{
    // No files, no symbols, no deps. All v2 table fields must still
    // appear as empty arrays for parsing uniformity — agents should
    // not need to branch on absence.
    CodeIndex index;

    auto parsed = slim_parse(index);

    EXPECT_EQ(parsed["_schema"]["version"], 2);
    EXPECT_EQ(parsed["encoding"]["edge_format"], "tuple-v1");
    EXPECT_EQ(parsed["encoding"]["files"], 0);
    EXPECT_EQ(parsed["encoding"]["languages"], 0);
    EXPECT_EQ(parsed["encoding"]["kinds"], 0);
    EXPECT_EQ(parsed["encoding"]["refs"], 0);

    ASSERT_TRUE(parsed.contains("languages"));
    ASSERT_TRUE(parsed.contains("kinds"));
    ASSERT_TRUE(parsed.contains("refs"));
    ASSERT_TRUE(parsed.contains("files"));
    EXPECT_TRUE(parsed["languages"].is_array());
    EXPECT_TRUE(parsed["kinds"].is_array());
    EXPECT_TRUE(parsed["refs"].is_array());
    EXPECT_TRUE(parsed["files"].is_array());
    EXPECT_TRUE(parsed["languages"].empty());
    EXPECT_TRUE(parsed["kinds"].empty());
    EXPECT_TRUE(parsed["refs"].empty());
    EXPECT_TRUE(parsed["files"].empty());

    // Edges + cycles are also empty arrays, not omitted.
    ASSERT_TRUE(parsed["dependency_graph"].contains("edges"));
    ASSERT_TRUE(parsed["dependency_graph"].contains("cycles"));
    EXPECT_TRUE(parsed["dependency_graph"]["edges"].empty());
    EXPECT_TRUE(parsed["dependency_graph"]["cycles"].empty());
}

} // namespace
