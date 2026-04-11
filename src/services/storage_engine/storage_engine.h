#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/result.h"

namespace vectis::services {

/// Low-level SQLite wrapper providing prepared statements, transactions,
/// WAL mode, and schema migrations.
///
/// Thread safety: WAL mode enables concurrent readers. All DML (INSERT /
/// UPDATE / DELETE) and DDL (CREATE / ALTER / DROP) are serialized by an
/// internal write mutex. Read-only SELECTs via Statement::query() do
/// **not** take the write mutex — SQLite WAL handles that natively.
class StorageEngine {
public:
    StorageEngine();
    ~StorageEngine();

    StorageEngine(const StorageEngine&)            = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;
    StorageEngine(StorageEngine&&) noexcept;
    StorageEngine& operator=(StorageEngine&&) noexcept;

    // ----- Lifecycle ---------------------------------------------------------

    /// Open (or create) a database at `db_path`. Enables WAL mode and
    /// foreign key enforcement.
    [[nodiscard]] vectis::core::Result<void> open(const std::filesystem::path& db_path);

    /// Close the database. Safe to call on an already-closed engine.
    void close();

    /// Apply pending schema migrations from `migrations.h`.
    [[nodiscard]] vectis::core::Result<void> migrate();

    /// Whether the database is currently open.
    [[nodiscard]] bool is_open() const;

    /// Path to the open database (empty if not open).
    [[nodiscard]] std::filesystem::path path() const;

    // ----- Raw SQL -----------------------------------------------------------

    /// Execute one or more SQL statements (no parameters, no result set).
    [[nodiscard]] vectis::core::Result<void> execute(std::string_view sql);

    // ----- Prepared statements -----------------------------------------------

    /// One value in a result row.
    using ColumnValue = std::variant<std::monostate, std::int64_t, double, std::string>;

    /// A single row from a SELECT result. Columns are accessed by
    /// zero-based index matching the SELECT column order.
    class Row {
    public:
        [[nodiscard]] std::int64_t  get_int(int col) const;
        [[nodiscard]] std::string   get_text(int col) const;
        [[nodiscard]] double        get_real(int col) const;
        [[nodiscard]] bool          is_null(int col) const;

    private:
        friend class StorageEngine;
        friend class Statement;
        std::vector<ColumnValue> m_columns;
    };

    /// A compiled SQL statement with typed parameter binding.
    /// Move-only; finalizes the underlying sqlite3_stmt on destruction.
    class Statement {
    public:
        ~Statement();

        Statement(const Statement&)            = delete;
        Statement& operator=(const Statement&) = delete;
        Statement(Statement&&) noexcept;
        Statement& operator=(Statement&&) noexcept;

        /// Bind a 64-bit integer to parameter `idx` (1-based).
        Statement& bind(int idx, std::int64_t value);
        /// Bind a text value to parameter `idx` (1-based).
        Statement& bind(int idx, std::string_view value);
        /// Bind a double to parameter `idx` (1-based).
        Statement& bind(int idx, double value);
        /// Bind NULL to parameter `idx` (1-based).
        Statement& bind_null(int idx);

        /// Execute a DML statement (INSERT / UPDATE / DELETE).
        [[nodiscard]] vectis::core::Result<void> execute();

        /// Run a SELECT and return all result rows. Data is copied out of
        /// the sqlite3_stmt at step time so rows outlive the statement.
        [[nodiscard]] vectis::core::Result<std::vector<Row>> query();

        /// The ROWID of the last INSERT performed by this statement.
        [[nodiscard]] std::int64_t last_insert_id() const;

        /// Reset the statement so it can be re-bound and re-executed.
        void reset();

    private:
        friend class StorageEngine;
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        explicit Statement(std::unique_ptr<Impl> impl);
    };

    /// Compile a SQL string into a prepared statement.
    [[nodiscard]] vectis::core::Result<Statement> prepare(std::string_view sql);

    // ----- Transactions ------------------------------------------------------

    [[nodiscard]] vectis::core::Result<void> begin_transaction();
    [[nodiscard]] vectis::core::Result<void> commit();
    [[nodiscard]] vectis::core::Result<void> rollback();

    /// RAII transaction guard. Rolls back on destruction unless
    /// `commit()` was called.
    class Transaction {
    public:
        explicit Transaction(StorageEngine& engine);
        ~Transaction();

        Transaction(const Transaction&)            = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) noexcept;
        Transaction& operator=(Transaction&&) noexcept;

        [[nodiscard]] vectis::core::Result<void> commit();

        /// Whether the transaction was successfully started.
        /// Callers should check this after construction.
        [[nodiscard]] bool is_active() const { return m_active; }

    private:
        StorageEngine* m_engine  = nullptr;
        bool           m_active  = false;
    };

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::services
