#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "code/code_index.h"
#include "code/code_index_store.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/symbol.h"
#include "services/storage_engine/storage_engine.h"

namespace {

namespace fs = std::filesystem;
using namespace vectis::code;
using vectis::services::StorageEngine;

class CodeIndexStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / ("vectis_store_test_" + std::to_string(::getpid()));
        fs::create_directories(m_tmp_dir);
        m_db_path = m_tmp_dir / "store_test.db";

        ASSERT_TRUE(m_storage.open(m_db_path));
        ASSERT_TRUE(m_storage.migrate());
    }

    void TearDown() override
    {
        m_storage.close();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    /// Populate the index with test data.
    void populate_index()
    {
        FileEntry f1;
        f1.path_relative = "src/main.cpp";
        f1.language = Language::Cpp;
        f1.size = 1024;
        f1.line_count = 42;
        f1.content_hash = "abcdef0123456789";
        m_file1_id = m_index.add_file(std::move(f1));

        FileEntry f2;
        f2.path_relative = "src/lib.rs";
        f2.language = Language::Rust;
        f2.size = 512;
        f2.line_count = 20;
        f2.content_hash = "1234567890abcdef";
        m_file2_id = m_index.add_file(std::move(f2));

        // Add symbols to file1.
        Symbol s1;
        s1.file_id = m_file1_id;
        s1.name = "main";
        s1.kind = SymbolKind::Function;
        s1.line_start = 10;
        s1.line_end = 40;
        s1.signature = "int main(int argc, char** argv)";
        s1.complexity = 5;

        Symbol s2;
        s2.file_id = m_file1_id;
        s2.name = "Config";
        s2.kind = SymbolKind::Struct;
        s2.line_start = 1;
        s2.line_end = 8;
        s2.members = {"name", "value", "count"};

        m_index.add_symbols(std::vector<Symbol>{s1, s2});

        // Add a symbol to file2.
        Symbol s3;
        s3.file_id = m_file2_id;
        s3.name = "run";
        s3.kind = SymbolKind::Function;
        s3.line_start = 5;
        s3.line_end = 18;
        s3.parent_id = 1; // some parent
        m_index.add_symbols(std::vector<Symbol>{s3});

        // Add a dependency.
        Dependency dep;
        dep.source_file_id = m_file1_id;
        dep.target_file_id = m_file2_id;
        dep.kind = "include";
        dep.import_string = "lib.rs";
        m_index.add_dependency(std::move(dep));
    }

    StorageEngine m_storage;
    CodeIndex m_index;
    fs::path m_tmp_dir;
    fs::path m_db_path;
    std::int64_t m_file1_id = 0;
    std::int64_t m_file2_id = 0;
};

TEST_F(CodeIndexStoreTest, SaveAndLoad_RoundTrip)
{
    populate_index();

    CacheMetadata meta;
    meta.project_root = "/some/project";
    meta.scan_timestamp = "2026-04-10T12:00:00Z";

    auto save_result = save_index(m_storage, m_index, meta);
    ASSERT_TRUE(save_result) << save_result.error().message;

    // Load into a fresh index.
    CodeIndex loaded;
    auto load_result = load_index(m_storage, loaded);
    ASSERT_TRUE(load_result) << load_result.error().message;

    // Verify metadata.
    EXPECT_EQ(load_result->project_root, "/some/project");
    EXPECT_EQ(load_result->scan_timestamp, "2026-04-10T12:00:00Z");

    // Verify files.
    EXPECT_EQ(loaded.file_count(), 2U);
    const auto files = loaded.snapshot_files();
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].path_relative, "src/lib.rs"); // sorted by path
    EXPECT_EQ(files[1].path_relative, "src/main.cpp");

    // Verify symbols.
    EXPECT_EQ(loaded.symbol_count(), 3U);
    const auto syms1 = loaded.symbols_in_file(m_file1_id);
    EXPECT_EQ(syms1.size(), 2U);

    // Verify dependency.
    EXPECT_EQ(loaded.dependency_count(), 1U);
    const auto deps = loaded.all_dependencies();
    ASSERT_EQ(deps.size(), 1U);
    EXPECT_EQ(deps[0].import_string, "lib.rs");
}

TEST_F(CodeIndexStoreTest, PreservesContentHash)
{
    populate_index();

    CacheMetadata meta;
    meta.project_root = "/test";
    ASSERT_TRUE(save_index(m_storage, m_index, meta));

    CodeIndex loaded;
    ASSERT_TRUE(load_index(m_storage, loaded));

    const auto files = loaded.snapshot_files();
    // Find the C++ file.
    bool found = false;
    for (const auto& f : files) {
        if (f.path_relative == "src/main.cpp") {
            EXPECT_EQ(f.content_hash, "abcdef0123456789");
            EXPECT_EQ(f.language, Language::Cpp);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(CodeIndexStoreTest, PreservesSymbolDetails)
{
    populate_index();

    CacheMetadata meta;
    meta.project_root = "/test";
    ASSERT_TRUE(save_index(m_storage, m_index, meta));

    CodeIndex loaded;
    ASSERT_TRUE(load_index(m_storage, loaded));

    // Check the struct with members.
    const auto syms = loaded.symbols_in_file(m_file1_id);
    bool found_struct = false;
    for (const auto& s : syms) {
        if (s.name == "Config") {
            EXPECT_EQ(s.kind, SymbolKind::Struct);
            ASSERT_EQ(s.members.size(), 3U);
            EXPECT_EQ(s.members[0], "name");
            EXPECT_EQ(s.members[1], "value");
            EXPECT_EQ(s.members[2], "count");
            found_struct = true;
        }
        if (s.name == "main") {
            EXPECT_EQ(s.signature, "int main(int argc, char** argv)");
            EXPECT_EQ(s.complexity, 5);
        }
    }
    EXPECT_TRUE(found_struct);

    // Check parent_id survived.
    const auto syms2 = loaded.symbols_in_file(m_file2_id);
    ASSERT_FALSE(syms2.empty());
    // parent_id is not preserved exactly (it gets reassigned), but the
    // non-zero value should still be non-zero.
    EXPECT_NE(syms2[0].parent_id, 0);
}

TEST_F(CodeIndexStoreTest, HasCacheFor_ReturnsTrueAfterSave)
{
    populate_index();

    CacheMetadata meta;
    meta.project_root = "/my/project";
    ASSERT_TRUE(save_index(m_storage, m_index, meta));

    EXPECT_TRUE(has_cache_for(m_storage, "/my/project"));
    EXPECT_FALSE(has_cache_for(m_storage, "/other/project"));
}

TEST_F(CodeIndexStoreTest, ClearCache_RemovesAllData)
{
    populate_index();

    CacheMetadata meta;
    meta.project_root = "/proj";
    ASSERT_TRUE(save_index(m_storage, m_index, meta));
    ASSERT_TRUE(has_cache_for(m_storage, "/proj"));

    auto r = clear_cache(m_storage);
    ASSERT_TRUE(r) << r.error().message;

    EXPECT_FALSE(has_cache_for(m_storage, "/proj"));

    CodeIndex loaded;
    auto load_r = load_index(m_storage, loaded);
    EXPECT_FALSE(load_r); // should fail — no cache
}

TEST_F(CodeIndexStoreTest, RoundTripsExternalDependencies)
{
    // Regression for the FK violation that aborted save() whenever a
    // file imported anything outside the project tree (target_file_id
    // == 0 — `<vector>`, `java.util.List`, …). The whole transaction
    // rolled back, leaving the cache empty and every subsequent run
    // doing a fresh scan with no speedup.
    populate_index();

    Dependency external;
    external.source_file_id = m_file1_id;
    external.target_file_id = 0;
    external.kind = "include";
    external.import_string = "vector";
    m_index.add_dependency(std::move(external));

    Dependency external2;
    external2.source_file_id = m_file1_id;
    external2.target_file_id = 0;
    external2.kind = "include";
    external2.import_string = "string";
    m_index.add_dependency(std::move(external2));

    CacheMetadata meta;
    meta.project_root = "/some/project";
    meta.scan_timestamp = "2026-05-06T12:00:00Z";

    auto save_result = save_index(m_storage, m_index, meta);
    ASSERT_TRUE(save_result) << save_result.error().message;

    CodeIndex loaded;
    auto load_result = load_index(m_storage, loaded);
    ASSERT_TRUE(load_result) << load_result.error().message;

    // Internal + 2 externals all survived the round-trip.
    EXPECT_EQ(loaded.dependency_count(), 3U);
    const auto deps = loaded.all_dependencies();
    ASSERT_EQ(deps.size(), 3U);

    std::size_t external_count = 0;
    for (const auto& d : deps) {
        if (d.target_file_id == 0) {
            ++external_count;
        }
    }
    EXPECT_EQ(external_count, 2U);
}

TEST_F(CodeIndexStoreTest, ColdAndWarmDependencyCountsMatch)
{
    // Reproduces FEEDBACK-2026-05-06 §1: explain reported 84019 deps
    // while a cached digest reported 84017. The 2-edge delta came
    // from duplicate edges that the in-memory index kept but the
    // cache PK silently dropped via INSERT OR IGNORE. After dedup
    // at add_dependency, the in-memory count must equal the cache
    // round-trip count.
    populate_index();

    // Inject duplicate edges that an over-eager resolver might emit.
    Dependency dup;
    dup.source_file_id = m_file1_id;
    dup.target_file_id = m_file2_id;
    dup.kind = "include";
    dup.import_string = "lib.rs";
    m_index.add_dependency(dup);
    m_index.add_dependency(dup);

    const std::size_t cold_count = m_index.dependency_count();

    CacheMetadata meta;
    meta.project_root = "/some/project";
    meta.scan_timestamp = "2026-05-06T12:00:00Z";
    ASSERT_TRUE(save_index(m_storage, m_index, meta));

    CodeIndex warm;
    ASSERT_TRUE(load_index(m_storage, warm));

    EXPECT_EQ(warm.dependency_count(), cold_count);
}

TEST_F(CodeIndexStoreTest, LoadFromEmptyDB_ReturnsError)
{
    CodeIndex loaded;
    auto r = load_index(m_storage, loaded);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::StorageError);
}

} // namespace
