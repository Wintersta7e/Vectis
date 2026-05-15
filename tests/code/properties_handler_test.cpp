#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/manifest_scanner.h"
#include "code/properties_handler.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::manifest_scanner::Config;
using vectis::code::manifest_scanner::scan_manifests;
using vectis::code::properties::make_properties_handler;

class PropertiesHandlerFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_root =
            std::filesystem::temp_directory_path() / (std::string{"vectis_props_"} + test_name);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    void TearDown() override { std::filesystem::remove_all(m_root); }

    void write_file(const std::filesystem::path& relative, std::string_view body) const
    {
        const auto full = m_root / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream stream(full);
        stream << body;
    }

    void run_handler(CodeIndex& index) const
    {
        Config config;
        config.root = m_root;
        std::unordered_set<std::string> visited;
        scan_manifests(config, index, visited, {make_properties_handler()});
    }

    [[nodiscard]] std::vector<Dependency> deps_of(const CodeIndex& index,
                                                  const std::string& path) const
    {
        const auto id = index.file_id_for_path(path);
        return id == 0 ? std::vector<Dependency>{} : index.dependencies_of(id);
    }

    std::filesystem::path m_root;
};

TEST_F(PropertiesHandlerFixture, RegistersFileWithPropertiesLanguageAndManifestSymbol)
{
    write_file("application.properties", "spring.datasource.url=jdbc:postgresql://localhost/db\n"
                                         "spring.datasource.user=admin\n"
                                         "logging.level.root=INFO\n"
                                         "app.timeout=30\n");

    CodeIndex index;
    run_handler(index);

    const auto id = index.file_id_for_path("application.properties");
    ASSERT_NE(id, 0);

    const auto files = index.snapshot_files();
    bool tagged = false;
    for (const auto& f : files) {
        if (f.id == id) {
            tagged = (f.language == Language::Properties);
        }
    }
    EXPECT_TRUE(tagged) << "registered .properties must carry Language::Properties";

    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_EQ(symbols[0].kind, SymbolKind::Manifest);
    EXPECT_EQ(symbols[0].name, "application");

    const auto& members = symbols[0].members;
    ASSERT_EQ(members.size(), 3U) << "three distinct top-level prefixes: app, logging, spring";
    EXPECT_EQ(members[0], "app") << "members must be sorted";
    EXPECT_EQ(members[1], "logging");
    EXPECT_EQ(members[2], "spring");
}

TEST_F(PropertiesHandlerFixture, TopLevelPrefixesAreCappedAtTwentyAfterSort)
{
    // Generate 25 distinct top-level prefixes a..y (one per key). The
    // cap is 20 and must apply AFTER alphabetical sort — so the kept
    // 20 are exactly a..t (not "the first 20 in insertion order").
    std::string body;
    for (char c = 'a'; c <= 'y'; ++c) {
        body += c;
        body += ".key=v\n";
    }
    write_file("big.properties", body);

    CodeIndex index;
    run_handler(index);
    const auto id = index.file_id_for_path("big.properties");
    ASSERT_NE(id, 0);
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    ASSERT_EQ(symbols[0].members.size(), 20U) << "exactly 20 after the cap";
    EXPECT_EQ(symbols[0].members.front(), "a") << "alphabetical sort must come before the cap";
    EXPECT_EQ(symbols[0].members.back(), "t") << "kept set is the alphabetical first 20 (a..t)";
}

TEST_F(PropertiesHandlerFixture, KeyWithoutDotBecomesItsOwnPrefix)
{
    write_file("flat.properties", "timeout=30\nverbose=true\n");

    CodeIndex index;
    run_handler(index);
    const auto id = index.file_id_for_path("flat.properties");
    ASSERT_NE(id, 0);
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    ASSERT_EQ(symbols[0].members.size(), 2U);
    EXPECT_EQ(symbols[0].members[0], "timeout");
    EXPECT_EQ(symbols[0].members[1], "verbose");
}

TEST_F(PropertiesHandlerFixture, NonPropertiesFilesAreIgnored)
{
    write_file("application.properties", "k=v\n");
    write_file("README.md", "# hi\n");
    write_file("config.ini", "[s]\nk=v\n");

    CodeIndex index;
    run_handler(index);

    EXPECT_NE(index.file_id_for_path("application.properties"), 0);
    EXPECT_EQ(index.file_id_for_path("README.md"), 0);
    EXPECT_EQ(index.file_id_for_path("config.ini"), 0);
}

TEST_F(PropertiesHandlerFixture, EmptyOrCommentOnlyFileStillRegisters)
{
    write_file("Messages_en.properties", "# only comments here\n! and another\n");

    CodeIndex index;
    run_handler(index);

    const auto id = index.file_id_for_path("Messages_en.properties");
    ASSERT_NE(id, 0) << "spec: even no-edges files appear in files[]";
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_TRUE(symbols[0].members.empty());
}

TEST_F(PropertiesHandlerFixture, MultiDotStemBecomesSymbolName)
{
    // `path::stem()` strips only the FINAL extension component, so a
    // file like `weird.name.properties` yields stem `weird.name`. Pin
    // this so a future switch to a different stem policy is caught.
    write_file("weird.name.properties", "k=v\n");

    CodeIndex index;
    run_handler(index);
    const auto id = index.file_id_for_path("weird.name.properties");
    ASSERT_NE(id, 0);
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_EQ(symbols[0].name, "weird.name");
}

TEST_F(PropertiesHandlerFixture, LeadingDotKeyIsSkippedFromPrefixes)
{
    // A key starting with `.` has an empty first segment. The prefix
    // builder filters empty prefixes so the manifest symbol's members
    // stay free of `""` noise.
    write_file("oddball.properties", ".foo=bar\n.bar=baz\nreal=value\n");

    CodeIndex index;
    run_handler(index);
    const auto id = index.file_id_for_path("oddball.properties");
    ASSERT_NE(id, 0);
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    ASSERT_EQ(symbols[0].members.size(), 1U)
        << "leading-dot keys contribute no prefix; only `real` survives";
    EXPECT_EQ(symbols[0].members[0], "real");
}

/// Count the `properties-include` edges in `deps`, separating those
/// that resolve to `expected_target` (with `expected_value` in
/// `import_string`) from any others. Tightens every positive test to
/// catch "spurious extra edge from the same source" regressions.
struct IncludeCount
{
    std::size_t total = 0;
    std::size_t matching = 0;
};

[[nodiscard]] static IncludeCount count_include_edges(const std::vector<Dependency>& deps,
                                                      std::int64_t expected_target,
                                                      std::string_view expected_value)
{
    IncludeCount c;
    for (const auto& d : deps) {
        if (d.kind != "properties-include") {
            continue;
        }
        ++c.total;
        if (d.target_file_id == expected_target && d.import_string == expected_value) {
            ++c.matching;
        }
    }
    return c;
}

TEST_F(PropertiesHandlerFixture, SpringConfigImport_ResolvesToSiblingProperties)
{
    write_file("application.properties", "spring.config.import=secrets.properties\n");
    write_file("secrets.properties", "db.password=hunter2\n");

    CodeIndex index;
    run_handler(index);

    const auto src = index.file_id_for_path("application.properties");
    const auto tgt = index.file_id_for_path("secrets.properties");
    ASSERT_NE(src, 0);
    ASSERT_NE(tgt, 0);

    const auto c =
        count_include_edges(deps_of(index, "application.properties"), tgt, "secrets.properties");
    EXPECT_EQ(c.total, 1U) << "exactly one properties-include edge from this source";
    EXPECT_EQ(c.matching, 1U)
        << "the one edge must point at secrets.properties (raw value preserved)";
}

TEST_F(PropertiesHandlerFixture, IncludeKey_ResolvesToSiblingProperties)
{
    write_file("audit.properties", "include=audit-extra.properties\n");
    write_file("audit-extra.properties", "logger.x.level=INFO\n");

    CodeIndex index;
    run_handler(index);

    const auto tgt = index.file_id_for_path("audit-extra.properties");
    ASSERT_NE(tgt, 0);
    const auto c =
        count_include_edges(deps_of(index, "audit.properties"), tgt, "audit-extra.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U);
}

TEST_F(PropertiesHandlerFixture, FilterParametersIncludeIsNotAnEdge_Regression)
{
    // Regression guard: a key whose suffix is `.include` (e.g.
    // `filterParameters.include`) must NOT emit a properties-include
    // edge — exact-key match only. Substring drift is the primary
    // failure mode this whole rule defends against.
    write_file("service.properties", "filterParameters.include=test1,test2\n"
                                     "dataset.container.version.include=4.4.4\n");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "service.properties");
    for (const auto& d : deps) {
        EXPECT_NE(d.kind, "properties-include")
            << "*.include keys must not match the exact-key rule";
    }
}

TEST_F(PropertiesHandlerFixture, PlaceholderImportTarget_EmitsExternal)
{
    write_file("application.properties", "include=${env.dir}/foo.properties\n");

    CodeIndex index;
    run_handler(index);

    const auto c = count_include_edges(deps_of(index, "application.properties"), 0,
                                       "${env.dir}/foo.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U) << "external edge: target 0, raw placeholder preserved";
}

TEST_F(PropertiesHandlerFixture, UnresolvedRelativePath_EmitsExternal)
{
    // No `missing.properties` exists in the fixture root.
    write_file("application.properties", "spring.config.import=missing.properties\n");

    CodeIndex index;
    run_handler(index);

    const auto c =
        count_include_edges(deps_of(index, "application.properties"), 0, "missing.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U);
}

TEST_F(PropertiesHandlerFixture, NestedImporterRelativePath_Resolves)
{
    // Importer at `cfg/application.properties` imports `inner.properties`,
    // expecting `cfg/inner.properties` to be the resolved target.
    write_file("cfg/application.properties", "spring.config.import=inner.properties\n");
    write_file("cfg/inner.properties", "k=v\n");

    CodeIndex index;
    run_handler(index);

    const auto tgt = index.file_id_for_path("cfg/inner.properties");
    ASSERT_NE(tgt, 0);
    const auto c =
        count_include_edges(deps_of(index, "cfg/application.properties"), tgt, "inner.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U);
}

TEST_F(PropertiesHandlerFixture, LeadingSlashResolvesFromProjectRoot)
{
    // Mirrors Phase 3b's spring `<import>` semantics: a leading `/` is
    // project-root-relative, NOT filesystem-absolute. Importer at
    // `nested/application.properties` imports `/shared/foo.properties`,
    // which must resolve to the root-level `shared/foo.properties`.
    write_file("nested/application.properties", "spring.config.import=/shared/foo.properties\n");
    write_file("shared/foo.properties", "k=v\n");

    CodeIndex index;
    run_handler(index);

    const auto tgt = index.file_id_for_path("shared/foo.properties");
    ASSERT_NE(tgt, 0);
    const auto c = count_include_edges(deps_of(index, "nested/application.properties"), tgt,
                                       "/shared/foo.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U) << "leading / is root-relative; raw value preserved";
}

TEST_F(PropertiesHandlerFixture, EmptyIncludeValueEmitsNoEdge)
{
    // `include=` (empty value) and `spring.config.import =` have no
    // target to express; the handler skips them rather than emitting
    // an external edge with a blank `import_string`.
    write_file("app.properties", "include=\n"
                                 "spring.config.import =\n");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "app.properties");
    for (const auto& d : deps) {
        EXPECT_NE(d.kind, "properties-include")
            << "empty include/spring.config.import value must not emit an edge";
    }
}

TEST_F(PropertiesHandlerFixture, MessagesBundleNoEdges)
{
    // i18n bundle with only translatable strings — no edges expected.
    write_file("Messages_en.properties", "greeting.hello=Hello\n"
                                         "greeting.bye=Goodbye\n");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "Messages_en.properties");
    for (const auto& d : deps) {
        EXPECT_NE(d.kind, "properties-include") << "i18n bundles must not emit include edges";
    }
}

TEST_F(PropertiesHandlerFixture, DualSpringConfigImportAndIncludeKeysEmitTwoEdges)
{
    // Both spring.config.import and include exist in one file; each
    // must emit its own edge. Catches a hypothetical "first-key-wins"
    // regression where the handler short-circuits after the first match.
    write_file("app.properties", "spring.config.import=secrets.properties\n"
                                 "include=overrides.properties\n");
    write_file("secrets.properties", "k=v\n");
    write_file("overrides.properties", "k=v\n");

    CodeIndex index;
    run_handler(index);

    const auto sec_id = index.file_id_for_path("secrets.properties");
    const auto ov_id = index.file_id_for_path("overrides.properties");
    ASSERT_NE(sec_id, 0);
    ASSERT_NE(ov_id, 0);
    std::size_t total = 0;
    bool to_secrets = false;
    bool to_overrides = false;
    for (const auto& d : deps_of(index, "app.properties")) {
        if (d.kind == "properties-include") {
            ++total;
            if (d.target_file_id == sec_id) {
                to_secrets = true;
            }
            if (d.target_file_id == ov_id) {
                to_overrides = true;
            }
        }
    }
    EXPECT_EQ(total, 2U) << "spring.config.import and include both emit edges";
    EXPECT_TRUE(to_secrets);
    EXPECT_TRUE(to_overrides);
}

TEST_F(PropertiesHandlerFixture, SpringPrefixFormStaysExternal)
{
    // Per settled decision #3 the whole raw value is treated as a path
    // (no Spring `optional:` / `file:` / `classpath:` prefix stripping).
    // The resulting "path" does not resolve, so the edge stays external
    // and the raw value is preserved verbatim so agents see the Spring
    // form intact.
    write_file("application.properties",
               "spring.config.import=optional:file:./secrets.properties\n");

    CodeIndex index;
    run_handler(index);

    const auto c = count_include_edges(deps_of(index, "application.properties"), 0,
                                       "optional:file:./secrets.properties");
    EXPECT_EQ(c.total, 1U);
    EXPECT_EQ(c.matching, 1U) << "Spring prefix forms stay external; raw value preserved verbatim";
}

TEST_F(PropertiesHandlerFixture, DuplicateIncludeKeyEmitsTwoEdges)
{
    // parse_properties preserves source order without dedup; the handler
    // emits one edge per occurrence. Agents see the full history rather
    // than a silently-dropped last-write-wins value.
    write_file("app.properties", "include=first.properties\n"
                                 "include=second.properties\n");
    write_file("first.properties", "k=v\n");
    write_file("second.properties", "k=v\n");

    CodeIndex index;
    run_handler(index);

    std::size_t total = 0;
    for (const auto& d : deps_of(index, "app.properties")) {
        if (d.kind == "properties-include") {
            ++total;
        }
    }
    EXPECT_EQ(total, 2U) << "duplicate keys emit duplicate edges (no last-write-wins)";
}

} // namespace
