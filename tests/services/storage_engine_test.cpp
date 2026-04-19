#include "services/storage_engine/storage_engine.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using vectis::services::StorageEngine;

/// Each test gets its own temp directory that is removed on tear-down.
class StorageEngineTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / "vectis_storage_test";
        fs::create_directories(m_tmp_dir);
        m_db_path = m_tmp_dir / "test.db";
    }

    void TearDown() override
    {
        m_engine.close();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    StorageEngine     m_engine;
    fs::path          m_tmp_dir;
    fs::path          m_db_path;
};

// ---- Lifecycle tests -------------------------------------------------------

TEST_F(StorageEngineTest, OpenCreatesDatabase)
{
    auto r = m_engine.open(m_db_path);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(fs::exists(m_db_path));
    EXPECT_TRUE(m_engine.is_open());
    EXPECT_EQ(m_engine.path(), m_db_path);
}

TEST_F(StorageEngineTest, CloseIsIdempotent)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    m_engine.close();
    EXPECT_FALSE(m_engine.is_open());
    m_engine.close(); // second close is a no-op
    EXPECT_FALSE(m_engine.is_open());
}

// ---- WAL mode test ---------------------------------------------------------

TEST_F(StorageEngineTest, WALModeEnabled)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    auto stmt = m_engine.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(stmt) << stmt.error().message;
    auto rows = stmt->query();
    ASSERT_TRUE(rows) << rows.error().message;
    ASSERT_FALSE(rows->empty());
    EXPECT_EQ((*rows)[0].get_text(0), "wal");
}

// ---- Migration tests -------------------------------------------------------

TEST_F(StorageEngineTest, MigrateCreatesSchema)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    auto r = m_engine.migrate();
    ASSERT_TRUE(r) << r.error().message;

    // Verify that the expected tables exist.
    auto stmt = m_engine.prepare(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    ASSERT_TRUE(stmt);
    auto rows = stmt->query();
    ASSERT_TRUE(rows);

    std::vector<std::string> tables;
    for (const auto& row : *rows) {
        tables.push_back(row.get_text(0));
    }

    EXPECT_NE(std::find(tables.begin(), tables.end(), "files"),          tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "symbols"),        tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "dependencies"),   tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "kv_store"),       tables.end());
    EXPECT_NE(std::find(tables.begin(), tables.end(), "schema_version"), tables.end());
}

TEST_F(StorageEngineTest, MigrateIsIdempotent)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.migrate());
    // Calling migrate again should succeed with no changes.
    auto r = m_engine.migrate();
    ASSERT_TRUE(r) << r.error().message;

    // One migration version should be recorded (v1 initial_schema).
    auto stmt = m_engine.prepare("SELECT COUNT(*) FROM schema_version");
    ASSERT_TRUE(stmt);
    auto rows = stmt->query();
    ASSERT_TRUE(rows);
    EXPECT_EQ((*rows)[0].get_int(0), 1);
}

// ---- Raw execute + query ---------------------------------------------------

TEST_F(StorageEngineTest, ExecuteAndQuery)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)"));
    ASSERT_TRUE(m_engine.execute("INSERT INTO test VALUES (1, 'hello')"));
    ASSERT_TRUE(m_engine.execute("INSERT INTO test VALUES (2, 'world')"));

    auto stmt = m_engine.prepare("SELECT id, name FROM test ORDER BY id");
    ASSERT_TRUE(stmt);
    auto rows = stmt->query();
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 2U);
    EXPECT_EQ((*rows)[0].get_int(0), 1);
    EXPECT_EQ((*rows)[0].get_text(1), "hello");
    EXPECT_EQ((*rows)[1].get_int(0), 2);
    EXPECT_EQ((*rows)[1].get_text(1), "world");
}

// ---- Prepared statement bind + query ---------------------------------------

TEST_F(StorageEngineTest, PreparedStatementBindAndQuery)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.execute(
        "CREATE TABLE kv (key TEXT PRIMARY KEY, int_val INTEGER, real_val REAL)"));

    auto ins = m_engine.prepare("INSERT INTO kv VALUES (?, ?, ?)");
    ASSERT_TRUE(ins);
    ins->bind(1, std::string_view{"alpha"});
    ins->bind(2, std::int64_t{42});
    ins->bind(3, 3.14);
    ASSERT_TRUE(ins->execute());

    ins->reset();
    ins->bind(1, std::string_view{"beta"});
    ins->bind_null(2);
    ins->bind(3, 2.71);
    ASSERT_TRUE(ins->execute());

    auto sel = m_engine.prepare("SELECT key, int_val, real_val FROM kv ORDER BY key");
    ASSERT_TRUE(sel);
    auto rows = sel->query();
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 2U);

    // alpha
    EXPECT_EQ((*rows)[0].get_text(0), "alpha");
    EXPECT_EQ((*rows)[0].get_int(1), 42);
    EXPECT_NEAR((*rows)[0].get_real(2), 3.14, 0.001);
    EXPECT_FALSE((*rows)[0].is_null(1));

    // beta — int_val is NULL
    EXPECT_EQ((*rows)[1].get_text(0), "beta");
    EXPECT_TRUE((*rows)[1].is_null(1));
    EXPECT_NEAR((*rows)[1].get_real(2), 2.71, 0.001);
}

TEST_F(StorageEngineTest, LastInsertId)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.execute("CREATE TABLE seq (id INTEGER PRIMARY KEY, val TEXT)"));

    auto ins = m_engine.prepare("INSERT INTO seq (val) VALUES (?)");
    ASSERT_TRUE(ins);
    ins->bind(1, std::string_view{"first"});
    ASSERT_TRUE(ins->execute());
    EXPECT_EQ(ins->last_insert_id(), 1);

    ins->reset();
    ins->bind(1, std::string_view{"second"});
    ASSERT_TRUE(ins->execute());
    EXPECT_EQ(ins->last_insert_id(), 2);
}

// ---- Transaction tests -----------------------------------------------------

TEST_F(StorageEngineTest, TransactionCommit)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.execute("CREATE TABLE t (v INTEGER)"));

    {
        StorageEngine::Transaction txn(m_engine);
        ASSERT_TRUE(m_engine.execute("INSERT INTO t VALUES (100)"));
        ASSERT_TRUE(txn.commit());
    }

    auto sel = m_engine.prepare("SELECT v FROM t");
    ASSERT_TRUE(sel);
    auto rows = sel->query();
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ((*rows)[0].get_int(0), 100);
}

TEST_F(StorageEngineTest, TransactionRollback)
{
    ASSERT_TRUE(m_engine.open(m_db_path));
    ASSERT_TRUE(m_engine.execute("CREATE TABLE t (v INTEGER)"));

    {
        StorageEngine::Transaction txn(m_engine);
        ASSERT_TRUE(m_engine.execute("INSERT INTO t VALUES (999)"));
        // txn destructor rolls back
    }

    auto sel = m_engine.prepare("SELECT COUNT(*) FROM t");
    ASSERT_TRUE(sel);
    auto rows = sel->query();
    ASSERT_TRUE(rows);
    EXPECT_EQ((*rows)[0].get_int(0), 0);
}

} // namespace
