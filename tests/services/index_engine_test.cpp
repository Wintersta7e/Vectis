#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "code/symbol.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace {

namespace fs = std::filesystem;
using vectis::services::IndexEngine;
using vectis::services::StorageEngine;

class IndexEngineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / "vectis_idx_test";
        fs::create_directories(m_tmp_dir);

        ASSERT_TRUE(m_storage.open(m_tmp_dir / "idx_test.db"));
        ASSERT_TRUE(m_storage.migrate());

        m_engine.initialize(m_storage);
    }

    void TearDown() override
    {
        m_storage.close();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    StorageEngine m_storage;
    IndexEngine m_engine;
    fs::path m_tmp_dir;
};

TEST_F(IndexEngineTest, IndexFileAndSearch)
{
    m_engine.index_file(1, "src/main.cpp",
                        "int main(int argc, char** argv) {\n"
                        "    auto config = load_config();\n"
                        "    return run(config);\n"
                        "}\n");

    auto results = m_engine.search("config");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].source, "file");
    EXPECT_EQ(results[0].source_id, 1);
}

TEST_F(IndexEngineTest, SearchFiles_FiltersToFileSource)
{
    m_engine.index_file(1, "src/main.cpp", "int main() { return 0; }");

    vectis::code::Symbol sym;
    sym.id = 100;
    sym.file_id = 1;
    sym.name = "main";
    sym.kind = vectis::code::SymbolKind::Function;
    sym.signature = "int main()";
    m_engine.index_symbols(1, {sym});

    // search_files should only return the file hit, not the symbol.
    auto results = m_engine.search_files("main");
    for (const auto& r : results) {
        EXPECT_EQ(r.source, "file");
    }

    // Generic search should return both.
    auto all_results = m_engine.search("main");
    EXPECT_GE(all_results.size(), 1U);
}

TEST_F(IndexEngineTest, RemoveFile_ExcludesFromResults)
{
    m_engine.index_file(1, "src/alpha.cpp", "void alpha() {}");
    m_engine.index_file(2, "src/beta.cpp", "void beta() {}");

    EXPECT_EQ(m_engine.indexed_file_count(), 2);

    m_engine.remove_file(1);

    auto results = m_engine.search_files("alpha");
    EXPECT_TRUE(results.empty());

    EXPECT_EQ(m_engine.indexed_file_count(), 1);
}

TEST_F(IndexEngineTest, IndexSymbols_SearchableByName)
{
    vectis::code::Symbol sym;
    sym.id = 42;
    sym.file_id = 1;
    sym.name = "calculate_total";
    sym.kind = vectis::code::SymbolKind::Function;
    sym.signature = "double calculate_total(const Order& order)";
    m_engine.index_symbols(1, {sym});

    auto results = m_engine.search("calculate");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].source, "symbol");
}

TEST_F(IndexEngineTest, PorterStemming)
{
    m_engine.index_file(1, "src/runner.cpp",
                        "void running() { /* this function handles running tasks */ }");

    // "run" should match "running" via porter stemming.
    auto results = m_engine.search_files("run");
    EXPECT_FALSE(results.empty());
}

TEST_F(IndexEngineTest, EmptyQueryReturnsEmpty)
{
    m_engine.index_file(1, "src/test.cpp", "void test() {}");
    auto results = m_engine.search("");
    EXPECT_TRUE(results.empty());
}

} // namespace
