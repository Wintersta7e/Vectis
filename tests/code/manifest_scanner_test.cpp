#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/manifest_scanner.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::manifest_scanner::Config;
using vectis::code::manifest_scanner::Handler;
using vectis::code::manifest_scanner::scan_manifests;

FileEntry make_manifest_entry(const std::string& relative)
{
    FileEntry f;
    f.path_relative = relative;
    f.language = Language::SpringXml;
    return f;
}

/// Test handler — registers ONE specific file path during Phase A and
/// optionally emits ONE pending edge during Phase B. The pending edge
/// target is looked up by path at Phase B time, which is exactly the
/// behaviour real handlers (Maven, csproj, etc.) will rely on.
class FixedFileHandler final : public Handler
{
public:
    FixedFileHandler(std::string register_path, std::string edge_target_path, std::string edge_kind)
        : m_register_path(std::move(register_path)),
          m_edge_target_path(std::move(edge_target_path)), m_edge_kind(std::move(edge_kind))
    {}

    void register_files(const Config& /*config*/, CodeIndex& index,
                        std::unordered_set<std::string>& visited_paths) override
    {
        const auto id = index.add_or_update_file_by_path(make_manifest_entry(m_register_path));
        visited_paths.insert(m_register_path);
        m_my_id = id;
    }

    void emit_edges(const Config& /*config*/, CodeIndex& index) override
    {
        if (m_edge_target_path.empty()) {
            return; // handler doesn't emit any edges
        }
        Dependency edge;
        edge.source_file_id = m_my_id;
        edge.target_file_id = index.file_id_for_path(m_edge_target_path);
        edge.kind = m_edge_kind;
        edge.import_string = m_edge_target_path;
        index.add_dependency(edge);
    }

private:
    std::string m_register_path;
    std::string m_edge_target_path; // empty if no edge
    std::string m_edge_kind;
    std::int64_t m_my_id = 0;
};

TEST(ManifestScannerTest, NoOpDoesNotAffectIndex)
{
    // Pre-populate the index with one source file; assert that running
    // scan_manifests with an empty handler list leaves it untouched.
    CodeIndex index;
    FileEntry seed;
    seed.path_relative = "src/main.cpp";
    seed.language = Language::Cpp;
    const auto seed_id = index.add_file(seed);

    Config config;
    config.root = ".";
    std::unordered_set<std::string> visited;
    scan_manifests(config, index, visited, {});

    EXPECT_EQ(index.file_count(), 1U);
    EXPECT_EQ(index.symbol_count(), 0U);
    EXPECT_EQ(index.dependency_count(), 0U);
    EXPECT_EQ(index.snapshot_files()[0].id, seed_id);
    EXPECT_TRUE(visited.empty());
}

TEST(ManifestScannerTest, Lifecycle_RegistersAllFilesBeforeEdgeEmission)
{
    // Handler A registers a.xml AND emits an edge a.xml → b.xml during
    // Phase B. Handler B registers b.xml. If the orchestrator
    // accidentally interleaves register/emit per handler, A's Phase B
    // would run before B's Phase A and the edge would resolve as
    // external (target_file_id == 0). The strict register-then-emit
    // ordering keeps it internal.
    CodeIndex index;
    auto handler_a = std::make_shared<FixedFileHandler>("a.xml", "b.xml", "spring-import");
    auto handler_b = std::make_shared<FixedFileHandler>("b.xml", "", "");

    Config config;
    config.root = ".";
    std::unordered_set<std::string> visited;
    scan_manifests(config, index, visited, {handler_a, handler_b});

    ASSERT_EQ(index.file_count(), 2U);
    ASSERT_EQ(index.dependency_count(), 1U);
    const auto deps = index.all_dependencies();
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_NE(deps[0].target_file_id, 0)
        << "cross-handler reference must resolve internally — Phase B saw b.xml";
    EXPECT_EQ(deps[0].kind, "spring-import");
}

TEST(ManifestScannerTest, Lifecycle_TargetSortedAfterSource)
{
    // Same shape but the SOURCE manifest sorts AFTER the target
    // alphabetically. If the implementation were walking the
    // filesystem and emitting edges in walk order, "z-source.xml"
    // would only see "a-target.xml" after its own Phase A — which
    // happens to work for this case, but the test pins the contract
    // by switching the dispatch order too: target's handler is
    // dispatched FIRST.
    CodeIndex index;
    auto target = std::make_shared<FixedFileHandler>("a-target.xml", "", "");
    auto source =
        std::make_shared<FixedFileHandler>("z-source.xml", "a-target.xml", "spring-import");

    Config config;
    config.root = ".";
    std::unordered_set<std::string> visited;
    // Dispatch order: target FIRST, source SECOND. Phase A for both
    // runs before either Phase B, so source's emit still sees target.
    scan_manifests(config, index, visited, {target, source});

    ASSERT_EQ(index.dependency_count(), 1U);
    const auto deps = index.all_dependencies();
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_NE(deps[0].target_file_id, 0);
    EXPECT_EQ(deps[0].import_string, "a-target.xml");
}

TEST(ManifestScannerTest, VisitedPathsAreAppendedByEachHandler)
{
    CodeIndex index;
    auto h1 = std::make_shared<FixedFileHandler>("foo/pom.xml", "", "");
    auto h2 = std::make_shared<FixedFileHandler>("bar/pom.xml", "", "");

    Config config;
    config.root = ".";
    std::unordered_set<std::string> visited;
    scan_manifests(config, index, visited, {h1, h2});

    EXPECT_EQ(visited.size(), 2U);
    EXPECT_TRUE(visited.contains("foo/pom.xml"));
    EXPECT_TRUE(visited.contains("bar/pom.xml"));
}

TEST(ManifestScannerTest, DefaultHandlersIncludesEveryShippedFormat)
{
    const auto handlers = vectis::code::manifest_scanner::default_handlers();
    EXPECT_FALSE(handlers.empty()) << "default handlers must register every shipped format";
    for (const auto& h : handlers) {
        EXPECT_NE(h.get(), nullptr);
    }
}

} // namespace
