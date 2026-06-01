#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/digest_exporter.h"
#include "code/fidelity.h"
#include "code/language.h"

namespace {

using vectis::code::build_digest_string;
using vectis::code::build_fidelity_metadata_json;
using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::DigestFormat;
using vectis::code::EdgeFidelity;
using vectis::code::ExportOptions;
using vectis::code::FileEntry;
using vectis::code::go_edge_confidence;
using vectis::code::k_go_external_stdlib_confidence;
using vectis::code::k_go_external_thirdparty_confidence;
using vectis::code::k_go_internal_confidence;
using vectis::code::k_py_external_dotted_confidence;
using vectis::code::k_py_external_relative_confidence;
using vectis::code::k_py_resolved_confidence;
using vectis::code::Language;
using vectis::code::python_edge_confidence;
using vectis::code::reconstruct_edge_fidelity;
using vectis::code::reconstruct_go_resolved_by;
using vectis::code::reconstruct_python_resolved_by;

// --- Strategy reconstruction -------------------------------------------------

TEST(FidelityTest, Reconstruct_RelativeModule)
{
    // from .x  ->  pkg/x.py  (resolved, module)
    EXPECT_EQ(reconstruct_python_resolved_by(".x", "pkg/x.py", /*is_external=*/false),
              "relative-module");
}

TEST(FidelityTest, Reconstruct_RelativePackage)
{
    // from .x  ->  pkg/x/__init__.py  (resolved, package)
    EXPECT_EQ(reconstruct_python_resolved_by(".x", "pkg/x/__init__.py", /*is_external=*/false),
              "relative-package");
}

TEST(FidelityTest, Reconstruct_DottedModule)
{
    // import a.b  ->  a/b.py  (resolved, module)
    EXPECT_EQ(reconstruct_python_resolved_by("a.b", "a/b.py", /*is_external=*/false),
              "dotted-module");
}

TEST(FidelityTest, Reconstruct_DottedPackage)
{
    // import a.b  ->  a/b/__init__.py  (resolved, package)
    EXPECT_EQ(reconstruct_python_resolved_by("a.b", "a/b/__init__.py", /*is_external=*/false),
              "dotted-package");
}

TEST(FidelityTest, Reconstruct_ExternalDotted)
{
    // Unresolved, no leading dot -> external-dotted. Target path is
    // ignored when external.
    EXPECT_EQ(reconstruct_python_resolved_by("numpy", "", /*is_external=*/true), "external-dotted");
    EXPECT_EQ(reconstruct_python_resolved_by("os.path", "ignored.py", /*is_external=*/true),
              "external-dotted");
}

TEST(FidelityTest, Reconstruct_ExternalRelative)
{
    // Unresolved, leading dot -> external-relative.
    EXPECT_EQ(reconstruct_python_resolved_by(".missing", "", /*is_external=*/true),
              "external-relative");
}

TEST(FidelityTest, Reconstruct_EmptyImportStringIsDotted)
{
    // No import string can't be relative (no leading dot); treat as dotted.
    EXPECT_EQ(reconstruct_python_resolved_by("", "", /*is_external=*/true), "external-dotted");
    EXPECT_EQ(reconstruct_python_resolved_by("", "a/b.py", /*is_external=*/false), "dotted-module");
}

TEST(FidelityTest, Reconstruct_GoInternal)
{
    // Resolved Go import (matched the go.mod prefix) -> go-internal.
    EXPECT_EQ(reconstruct_go_resolved_by("example.com/app/handler", /*is_external=*/false),
              "go-internal");
}

TEST(FidelityTest, Reconstruct_GoExternalStdlib)
{
    // Unresolved, first path segment has no dot -> standard library.
    EXPECT_EQ(reconstruct_go_resolved_by("fmt", /*is_external=*/true), "go-external-stdlib");
    EXPECT_EQ(reconstruct_go_resolved_by("database/sql", /*is_external=*/true),
              "go-external-stdlib");
}

TEST(FidelityTest, Reconstruct_GoExternalThirdparty)
{
    // Unresolved, first path segment is a domain -> third-party.
    EXPECT_EQ(reconstruct_go_resolved_by("github.com/gin-gonic/gin", /*is_external=*/true),
              "go-external-thirdparty");
    EXPECT_EQ(reconstruct_go_resolved_by("golang.org/x/sync/errgroup", /*is_external=*/true),
              "go-external-thirdparty");
}

// --- Confidence lookup -------------------------------------------------------

TEST(FidelityTest, Confidence_ResolvedStrategies)
{
    EXPECT_DOUBLE_EQ(python_edge_confidence("relative-module"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("relative-package"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("dotted-module"), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("dotted-package"), k_py_resolved_confidence);
}

TEST(FidelityTest, Confidence_ExternalStrategies)
{
    EXPECT_DOUBLE_EQ(python_edge_confidence("external-relative"),
                     k_py_external_relative_confidence);
    EXPECT_DOUBLE_EQ(python_edge_confidence("external-dotted"), k_py_external_dotted_confidence);
}

TEST(FidelityTest, Confidence_UnknownFailsClosed)
{
    // An unrecognised strategy must not inherit a neighbour's number.
    EXPECT_DOUBLE_EQ(python_edge_confidence("not-a-strategy"), 0.0);
    EXPECT_DOUBLE_EQ(python_edge_confidence(""), 0.0);
}

TEST(FidelityTest, Confidence_GoStrategies)
{
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-internal"), k_go_internal_confidence);
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-external-stdlib"), k_go_external_stdlib_confidence);
    EXPECT_DOUBLE_EQ(go_edge_confidence("go-external-thirdparty"),
                     k_go_external_thirdparty_confidence);
}

TEST(FidelityTest, Confidence_GoUnknownFailsClosed)
{
    // Python strata are not valid Go strategies and vice versa: each lookup
    // only knows its own taxonomy and fails closed otherwise.
    EXPECT_DOUBLE_EQ(go_edge_confidence("relative-module"), 0.0);
    EXPECT_DOUBLE_EQ(go_edge_confidence("not-a-strategy"), 0.0);
    EXPECT_DOUBLE_EQ(go_edge_confidence(""), 0.0);
}

// --- Dispatcher --------------------------------------------------------------

TEST(FidelityTest, Dispatch_PythonAndGoImportEdges)
{
    const auto py = reconstruct_edge_fidelity("pkg/a.py", "import", ".b", "pkg/b.py",
                                              /*is_external=*/false);
    ASSERT_TRUE(py.has_value());
    EXPECT_EQ(py->resolved_by, "relative-module");
    EXPECT_DOUBLE_EQ(py->confidence, k_py_resolved_confidence);

    const auto go = reconstruct_edge_fidelity("cmd/main.go", "import", "example.com/app/x",
                                              "x/x.go", /*is_external=*/false);
    ASSERT_TRUE(go.has_value());
    EXPECT_EQ(go->resolved_by, "go-internal");
    EXPECT_DOUBLE_EQ(go->confidence, k_go_internal_confidence);
}

TEST(FidelityTest, Dispatch_UncalibratedReturnsNullopt)
{
    // A language/kind with no calibration model yet must yield no enrichment.
    EXPECT_FALSE(
        reconstruct_edge_fidelity("src/x.cpp", "include", "y.h", "src/y.h", false).has_value());
    // Right extension, wrong kind for that language.
    EXPECT_FALSE(reconstruct_edge_fidelity("a.py", "call", "b", "b.py", false).has_value());
}

// --- fidelity_metadata block -------------------------------------------------

TEST(FidelityTest, Metadata_HasExpectedShape)
{
    const nlohmann::json meta = build_fidelity_metadata_json();

    // Shared top-level caveat, then a per-language `languages` map; each
    // language carries its own `provisional` flag.
    ASSERT_FALSE(meta.contains("provisional")) << "provisional moved per-language";
    ASSERT_TRUE(meta.contains("caveat"));
    EXPECT_NE(meta["caveat"].get<std::string>().find("NOT a per-repo guarantee"),
              std::string::npos);
    ASSERT_TRUE(meta.contains("languages"));

    const auto& py = meta["languages"]["python"];
    ASSERT_TRUE(py.contains("version"));
    EXPECT_EQ(py["method"], "per-strategy empirical precision vs manual ground truth (offline)");
    EXPECT_EQ(py["scope"], "python-import-edges");
    EXPECT_EQ(py["provisional"], true);
    EXPECT_EQ(py["corpus"]["projects"], 2);
    EXPECT_EQ(py["corpus"]["labeled_edges"], 112);
    const auto& py_exp = py["expected_precision"];
    EXPECT_DOUBLE_EQ(py_exp["relative-module"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(py_exp["dotted-package"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(py_exp["external-dotted"].get<double>(), k_py_external_dotted_confidence);
    EXPECT_DOUBLE_EQ(py_exp["external-relative"].get<double>(), k_py_external_relative_confidence);

    const auto& go = meta["languages"]["go"];
    ASSERT_TRUE(go.contains("version"));
    EXPECT_EQ(go["scope"], "go-import-edges");
    EXPECT_EQ(go["provisional"], false) << "Go de-provisionalized after corpus expansion";
    EXPECT_EQ(go["corpus"]["projects"], 11);
    EXPECT_EQ(go["corpus"]["labeled_edges"], 90);
    const auto& go_exp = go["expected_precision"];
    EXPECT_DOUBLE_EQ(go_exp["go-internal"].get<double>(), k_go_internal_confidence);
    EXPECT_DOUBLE_EQ(go_exp["go-external-stdlib"].get<double>(), k_go_external_stdlib_confidence);
    EXPECT_DOUBLE_EQ(go_exp["go-external-thirdparty"].get<double>(),
                     k_go_external_thirdparty_confidence);
}

// --- Digest integration ------------------------------------------------------

ExportOptions make_options(DigestFormat format)
{
    ExportOptions options;
    options.format = format;
    options.project_root = "/fake/project";
    options.project_name = "fidelity-test";
    return options;
}

/// Two Python files plus a C++ pair, so the digest carries both a
/// Python import edge (gets resolved_by/confidence) and a non-Python
/// edge (must stay untouched).
void populate_mixed_index(CodeIndex& index, std::int64_t& py_src, std::int64_t& py_dst)
{
    FileEntry a;
    a.path_relative = "pkg/a.py";
    a.language = Language::Python;
    py_src = index.add_file(std::move(a));

    FileEntry b;
    b.path_relative = "pkg/b.py";
    b.language = Language::Python;
    py_dst = index.add_file(std::move(b));

    FileEntry c;
    c.path_relative = "src/x.cpp";
    c.language = Language::Cpp;
    const std::int64_t c_id = index.add_file(std::move(c));

    FileEntry d;
    d.path_relative = "src/y.cpp";
    d.language = Language::Cpp;
    const std::int64_t d_id = index.add_file(std::move(d));

    // Resolved Python relative import: pkg/a.py  from .b  -> pkg/b.py
    Dependency py;
    py.source_file_id = py_src;
    py.target_file_id = py_dst;
    py.import_string = ".b";
    py.kind = "import";

    // C++ include — must NOT gain fidelity fields.
    Dependency cpp;
    cpp.source_file_id = c_id;
    cpp.target_file_id = d_id;
    cpp.import_string = "y.h";
    cpp.kind = "include";

    const std::array<Dependency, 2> batch = {py, cpp};
    index.add_dependencies(batch);
}

TEST(FidelityTest, FullJson_PythonEdgeCarriesResolvedByAndConfidence)
{
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_python = false;
    bool saw_cpp = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        if (e["kind"] == "import") {
            saw_python = true;
            ASSERT_TRUE(e.contains("resolved_by"));
            ASSERT_TRUE(e.contains("confidence"));
            // pkg/a.py `from .b` resolving to pkg/b.py (a module) is
            // relative-module at the resolved confidence.
            EXPECT_EQ(e["resolved_by"], "relative-module");
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_py_resolved_confidence);
        }
        if (e["kind"] == "include") {
            saw_cpp = true;
            EXPECT_FALSE(e.contains("resolved_by")) << "non-Python edges must stay untouched";
            EXPECT_FALSE(e.contains("confidence"));
        }
    }
    EXPECT_TRUE(saw_python);
    EXPECT_TRUE(saw_cpp);
}

TEST(FidelityTest, FullJson_ExternalPythonEdgeIsExternalDotted)
{
    CodeIndex index;
    FileEntry a;
    a.path_relative = "app/main.py";
    a.language = Language::Python;
    const std::int64_t a_id = index.add_file(std::move(a));

    Dependency ext;
    ext.source_file_id = a_id;
    ext.target_file_id = 0; // unresolved external
    ext.import_string = "numpy";
    ext.kind = "import";
    index.add_dependency(std::move(ext));

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_EQ(edges[0]["resolved_by"], "external-dotted");
    EXPECT_DOUBLE_EQ(edges[0]["confidence"].get<double>(), k_py_external_dotted_confidence);
}

TEST(FidelityTest, FullJson_GoEdgesCarryStrategyAndConfidence)
{
    CodeIndex index;

    FileEntry main_go;
    main_go.path_relative = "cmd/main.go";
    main_go.language = Language::Go;
    const std::int64_t main_id = index.add_file(std::move(main_go));

    FileEntry handler_go;
    handler_go.path_relative = "internal/handler/handler.go";
    handler_go.language = Language::Go;
    const std::int64_t handler_id = index.add_file(std::move(handler_go));

    // Resolved internal import (matched the go.mod prefix) -> go-internal.
    Dependency internal_dep;
    internal_dep.source_file_id = main_id;
    internal_dep.target_file_id = handler_id;
    internal_dep.import_string = "example.com/app/internal/handler";
    internal_dep.kind = "import";

    // Unresolved standard-library import -> go-external-stdlib.
    Dependency stdlib;
    stdlib.source_file_id = main_id;
    stdlib.target_file_id = 0;
    stdlib.import_string = "database/sql";
    stdlib.kind = "import";

    // Unresolved third-party import -> go-external-thirdparty.
    Dependency thirdparty;
    thirdparty.source_file_id = main_id;
    thirdparty.target_file_id = 0;
    thirdparty.import_string = "github.com/gin-gonic/gin";
    thirdparty.kind = "import";

    const std::array<Dependency, 3> batch = {internal_dep, stdlib, thirdparty};
    index.add_dependencies(batch);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::Json)));

    bool saw_internal = false;
    bool saw_stdlib = false;
    bool saw_thirdparty = false;
    for (const auto& e : parsed["dependency_graph"]["edges"]) {
        if (e["kind"] != "import") {
            continue;
        }
        ASSERT_TRUE(e.contains("resolved_by"));
        ASSERT_TRUE(e.contains("confidence"));
        if (e["resolved_by"] == "go-internal") {
            saw_internal = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_internal_confidence);
        }
        else if (e["resolved_by"] == "go-external-stdlib") {
            saw_stdlib = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_external_stdlib_confidence);
        }
        else if (e["resolved_by"] == "go-external-thirdparty") {
            saw_thirdparty = true;
            EXPECT_DOUBLE_EQ(e["confidence"].get<double>(), k_go_external_thirdparty_confidence);
        }
    }
    EXPECT_TRUE(saw_internal);
    EXPECT_TRUE(saw_stdlib);
    EXPECT_TRUE(saw_thirdparty);
}

TEST(FidelityTest, BothFormats_CarryFidelityMetadata)
{
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    for (const DigestFormat fmt : {DigestFormat::Json, DigestFormat::SlimJson}) {
        const auto parsed = nlohmann::json::parse(build_digest_string(index, make_options(fmt)));
        ASSERT_TRUE(parsed.contains("fidelity_metadata")) << "format=" << static_cast<int>(fmt);
        const auto& meta = parsed["fidelity_metadata"];
        EXPECT_TRUE(meta.contains("caveat"));
        ASSERT_TRUE(meta.contains("languages"));
        for (const char* lang : {"python", "go"}) {
            const auto& block = meta["languages"][lang];
            EXPECT_TRUE(block.contains("version")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("scope")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("expected_precision")) << "lang=" << lang;
            EXPECT_TRUE(block.contains("provisional")) << "lang=" << lang;
        }
        EXPECT_EQ(meta["languages"]["go"]["scope"], "go-import-edges");
    }
}

TEST(FidelityTest, SlimJson_EdgeTuplesStayFrozen)
{
    // Slim must get fidelity_metadata but NOT per-edge confidence — its
    // edge tuples stay at 4 positional elements (schema version 2).
    CodeIndex index;
    std::int64_t py_src = 0;
    std::int64_t py_dst = 0;
    populate_mixed_index(index, py_src, py_dst);

    const auto parsed =
        nlohmann::json::parse(build_digest_string(index, make_options(DigestFormat::SlimJson)));

    EXPECT_EQ(parsed["_schema"]["version"], 2);
    const auto& edges = parsed["dependency_graph"]["edges"];
    ASSERT_FALSE(edges.empty());
    for (const auto& e : edges) {
        ASSERT_TRUE(e.is_array()) << "slim edges stay positional tuples";
        EXPECT_EQ(e.size(), 4U) << "slim tuple arity must not grow";
    }
}

} // namespace
