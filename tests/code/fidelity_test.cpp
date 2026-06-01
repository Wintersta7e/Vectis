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
using vectis::code::ExportOptions;
using vectis::code::FileEntry;
using vectis::code::k_py_external_dotted_confidence;
using vectis::code::k_py_external_relative_confidence;
using vectis::code::k_py_resolved_confidence;
using vectis::code::Language;
using vectis::code::python_edge_confidence;
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

// --- fidelity_metadata block -------------------------------------------------

TEST(FidelityTest, Metadata_HasExpectedShape)
{
    const nlohmann::json meta = build_fidelity_metadata_json();

    ASSERT_TRUE(meta.contains("version"));
    EXPECT_EQ(meta["method"], "per-strategy empirical precision vs manual ground truth (offline)");
    EXPECT_EQ(meta["scope"], "python-import-edges");
    EXPECT_EQ(meta["provisional"], true);

    ASSERT_TRUE(meta.contains("corpus"));
    EXPECT_EQ(meta["corpus"]["projects"], 2);
    EXPECT_EQ(meta["corpus"]["labeled_edges"], 112);

    ASSERT_TRUE(meta.contains("expected_precision"));
    const auto& exp = meta["expected_precision"];
    EXPECT_DOUBLE_EQ(exp["relative-module"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(exp["dotted-package"].get<double>(), k_py_resolved_confidence);
    EXPECT_DOUBLE_EQ(exp["external-dotted"].get<double>(), k_py_external_dotted_confidence);
    EXPECT_DOUBLE_EQ(exp["external-relative"].get<double>(), k_py_external_relative_confidence);

    ASSERT_TRUE(meta.contains("caveat"));
    EXPECT_NE(meta["caveat"].get<std::string>().find("NOT a per-repo guarantee"),
              std::string::npos);
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
        EXPECT_EQ(meta["scope"], "python-import-edges");
        EXPECT_EQ(meta["provisional"], true);
        EXPECT_TRUE(meta.contains("version"));
        EXPECT_TRUE(meta.contains("expected_precision"));
        EXPECT_TRUE(meta.contains("caveat"));
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
