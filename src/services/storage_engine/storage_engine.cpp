#include "services/storage_engine/storage_engine.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

#include "core/log.h"
#include "core/result.h"
#include "services/storage_engine/migrations.h"

namespace vectis::services {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

// ============================================================================
// StorageEngine::Impl
// ============================================================================

struct StorageEngine::Impl
{
    sqlite3* db = nullptr;
    std::filesystem::path db_path;
    std::mutex write_mutex;
};

// ============================================================================
// Statement::Impl
// ============================================================================

struct StorageEngine::Statement::Impl
{
    sqlite3_stmt* stmt = nullptr;
    sqlite3* db = nullptr; // borrowed — for last_insert_rowid
};

// ============================================================================
// StorageEngine lifecycle
// ============================================================================

StorageEngine::StorageEngine() : m_impl(std::make_unique<Impl>()) {}
StorageEngine::~StorageEngine()
{
    close();
}

StorageEngine::StorageEngine(StorageEngine&&) noexcept = default;
StorageEngine& StorageEngine::operator=(StorageEngine&&) noexcept = default;

Result<void> StorageEngine::open(const std::filesystem::path& db_path)
{
    if (m_impl->db != nullptr) {
        close();
    }

    const std::string path_str = db_path.string();
    const int rc =
        sqlite3_open_v2(path_str.c_str(), &m_impl->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);

    if (rc != SQLITE_OK) {
        std::string msg =
            m_impl->db != nullptr ? sqlite3_errmsg(m_impl->db) : "sqlite3_open_v2 failed";
        if (m_impl->db != nullptr) {
            sqlite3_close_v2(m_impl->db);
            m_impl->db = nullptr;
        }
        return make_error(ErrorKind::StorageError, std::move(msg), path_str);
    }

    m_impl->db_path = db_path;

    // Enable WAL mode for concurrent readers.
    if (auto r = execute("PRAGMA journal_mode=WAL"); !r) {
        return r;
    }
    // Enforce foreign key constraints.
    if (auto r = execute("PRAGMA foreign_keys=ON"); !r) {
        return r;
    }
    // NORMAL sync is safe under WAL and avoids the FULL-sync penalty.
    if (auto r = execute("PRAGMA synchronous=NORMAL"); !r) {
        return r;
    }

    VECTIS_LOG_INFO("StorageEngine: opened '{}'", path_str);
    return {};
}

void StorageEngine::close()
{
    if (m_impl->db != nullptr) {
        sqlite3_close_v2(m_impl->db);
        m_impl->db = nullptr;
        VECTIS_LOG_INFO("StorageEngine: closed '{}'", m_impl->db_path.string());
    }
}

Result<void> StorageEngine::migrate()
{
    if (m_impl->db == nullptr) {
        return make_error(ErrorKind::StorageError, "database not open");
    }

    // Ensure the schema_version table exists.
    if (auto r = execute("CREATE TABLE IF NOT EXISTS schema_version ("
                         "  version    INTEGER PRIMARY KEY,"
                         "  applied_at INTEGER"
                         ")");
        !r) {
        return r;
    }

    // Read the current schema version.
    auto stmt_result = prepare("SELECT COALESCE(MAX(version), 0) FROM schema_version");
    if (!stmt_result) {
        return tl::unexpected(stmt_result.error());
    }
    auto rows = stmt_result->query();
    if (!rows) {
        return tl::unexpected(rows.error());
    }
    int current_version = 0;
    if (!rows->empty()) {
        current_version = static_cast<int>((*rows)[0].get_int(0));
    }

    // Apply pending migrations.
    for (const auto& migration : k_migrations) {
        if (migration.version <= current_version) {
            continue;
        }

        VECTIS_LOG_INFO("StorageEngine: applying migration v{} '{}'", migration.version,
                        migration.name);

        // Use begin_transaction/commit which manage the write_mutex.
        if (auto r = begin_transaction(); !r) {
            return r;
        }

        if (auto r = execute(migration.sql); !r) {
            (void)rollback();
            return r;
        }

        // Record the migration.
        auto ins = prepare("INSERT INTO schema_version (version, applied_at) VALUES (?, ?)");
        if (!ins) {
            (void)rollback();
            return tl::unexpected(ins.error());
        }
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        ins->bind(1, static_cast<std::int64_t>(migration.version));
        ins->bind(2, static_cast<std::int64_t>(now));
        if (auto r = ins->execute(); !r) {
            (void)rollback();
            return r;
        }

        if (auto r = commit(); !r) {
            // commit() internally rolls back on failure.
            return r;
        }
    }

    return {};
}

bool StorageEngine::is_open() const
{
    return m_impl->db != nullptr;
}

std::filesystem::path StorageEngine::path() const
{
    return m_impl->db_path;
}

// ============================================================================
// Raw SQL
// ============================================================================

Result<void> StorageEngine::execute(std::string_view sql)
{
    if (m_impl->db == nullptr) {
        return make_error(ErrorKind::StorageError, "database not open");
    }

    char* errmsg = nullptr;
    const int rc = sqlite3_exec(m_impl->db, std::string(sql).c_str(), nullptr, nullptr, &errmsg);

    if (rc != SQLITE_OK) {
        std::string msg = errmsg != nullptr ? errmsg : "sqlite3_exec failed";
        sqlite3_free(errmsg);
        return make_error(ErrorKind::StorageError, std::move(msg), std::string(sql).substr(0, 120));
    }
    return {};
}

// ============================================================================
// Prepared statements
// ============================================================================

StorageEngine::Statement::Statement(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

StorageEngine::Statement::~Statement()
{
    if (m_impl && m_impl->stmt != nullptr) {
        sqlite3_finalize(m_impl->stmt);
    }
}

StorageEngine::Statement::Statement(Statement&&) noexcept = default;
StorageEngine::Statement& StorageEngine::Statement::operator=(Statement&&) noexcept = default;

StorageEngine::Statement& StorageEngine::Statement::bind(int idx, std::int64_t value)
{
    sqlite3_bind_int64(m_impl->stmt, idx, value);
    return *this;
}

StorageEngine::Statement& StorageEngine::Statement::bind(int idx, std::string_view value)
{
    sqlite3_bind_text(m_impl->stmt, idx, value.data(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
    return *this;
}

StorageEngine::Statement& StorageEngine::Statement::bind(int idx, double value)
{
    sqlite3_bind_double(m_impl->stmt, idx, value);
    return *this;
}

StorageEngine::Statement& StorageEngine::Statement::bind_null(int idx)
{
    sqlite3_bind_null(m_impl->stmt, idx);
    return *this;
}

Result<void> StorageEngine::Statement::execute()
{
    const int rc = sqlite3_step(m_impl->stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        std::string msg = sqlite3_errmsg(m_impl->db);
        sqlite3_reset(m_impl->stmt);
        return make_error(ErrorKind::StorageError, std::move(msg));
    }
    sqlite3_reset(m_impl->stmt);
    return {};
}

void StorageEngine::Statement::fill_row_from_step(Row& row, int col_count, void* stmt_ptr)
{
    auto* stmt = static_cast<sqlite3_stmt*>(stmt_ptr);
    row.m_columns.clear();
    row.m_columns.reserve(static_cast<std::size_t>(col_count));
    for (int i = 0; i < col_count; ++i) {
        const int type = sqlite3_column_type(stmt, i);
        switch (type) {
        case SQLITE_INTEGER:
            row.m_columns.emplace_back(sqlite3_column_int64(stmt, i));
            break;
        case SQLITE_FLOAT:
            row.m_columns.emplace_back(sqlite3_column_double(stmt, i));
            break;
        case SQLITE_TEXT: {
            // SQLite returns UTF-8 text as `unsigned char*` to keep its
            // C ABI portable; we read it back as `char*` to feed
            // std::string. The byte representation is identical, so
            // this is the canonical SQLite-binding cast.
            const unsigned char* raw = sqlite3_column_text(stmt, i);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const auto* text = reinterpret_cast<const char*>(raw);
            const int len = sqlite3_column_bytes(stmt, i);
            row.m_columns.emplace_back(std::string(text, static_cast<std::size_t>(len)));
            break;
        }
        case SQLITE_BLOB:
        case SQLITE_NULL:
        default:
            row.m_columns.emplace_back(std::monostate{});
            break;
        }
    }
}

Result<std::vector<StorageEngine::Row>> StorageEngine::Statement::query()
{
    std::vector<Row> rows;
    const int col_count = sqlite3_column_count(m_impl->stmt);

    while (true) {
        const int rc = sqlite3_step(m_impl->stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            std::string msg = sqlite3_errmsg(m_impl->db);
            sqlite3_reset(m_impl->stmt);
            return make_error(ErrorKind::StorageError, std::move(msg));
        }
        Row row;
        fill_row_from_step(row, col_count, m_impl->stmt);
        rows.push_back(std::move(row));
    }

    sqlite3_reset(m_impl->stmt);
    return rows;
}

Result<void> StorageEngine::Statement::query_each(const std::function<void(const Row&)>& callback)
{
    // Reset on every exit path — including a callback exception. Without
    // this, a throwing callback leaves the statement stepped-but-not-reset
    // and the next execute()/query() returns SQLITE_MISUSE (or replays
    // stale bindings on older builds).
    class ResetGuard
    {
    public:
        explicit ResetGuard(sqlite3_stmt* stmt) noexcept : m_stmt(stmt) {}
        ResetGuard(const ResetGuard&) = delete;
        ResetGuard& operator=(const ResetGuard&) = delete;
        ResetGuard(ResetGuard&&) = delete;
        ResetGuard& operator=(ResetGuard&&) = delete;
        ~ResetGuard() { sqlite3_reset(m_stmt); }

    private:
        sqlite3_stmt* m_stmt;
    };
    const ResetGuard guard(m_impl->stmt);

    const int col_count = sqlite3_column_count(m_impl->stmt);
    Row row;
    while (true) {
        const int rc = sqlite3_step(m_impl->stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            std::string msg = sqlite3_errmsg(m_impl->db);
            return make_error(ErrorKind::StorageError, std::move(msg));
        }
        fill_row_from_step(row, col_count, m_impl->stmt);
        callback(row);
    }
    return {};
}

std::int64_t StorageEngine::Statement::last_insert_id() const
{
    return sqlite3_last_insert_rowid(m_impl->db);
}

void StorageEngine::Statement::reset()
{
    sqlite3_reset(m_impl->stmt);
    sqlite3_clear_bindings(m_impl->stmt);
}

// ============================================================================
// Row accessors
// ============================================================================

std::int64_t StorageEngine::Row::get_int(int col) const
{
    const auto& val = m_columns.at(static_cast<std::size_t>(col));
    if (const auto* p = std::get_if<std::int64_t>(&val)) {
        return *p;
    }
    // SQLite may return a double for large INTEGER values.
    if (const auto* p = std::get_if<double>(&val)) {
        return static_cast<std::int64_t>(*p);
    }
    return 0;
}

std::string StorageEngine::Row::get_text(int col) const
{
    const auto& val = m_columns.at(static_cast<std::size_t>(col));
    if (const auto* p = std::get_if<std::string>(&val)) {
        return *p;
    }
    return {};
}

double StorageEngine::Row::get_real(int col) const
{
    const auto& val = m_columns.at(static_cast<std::size_t>(col));
    if (const auto* p = std::get_if<double>(&val)) {
        return *p;
    }
    if (const auto* p = std::get_if<std::int64_t>(&val)) {
        return static_cast<double>(*p);
    }
    return 0.0;
}

bool StorageEngine::Row::is_null(int col) const
{
    const auto& val = m_columns.at(static_cast<std::size_t>(col));
    return std::holds_alternative<std::monostate>(val);
}

// ============================================================================
// Prepare
// ============================================================================

Result<StorageEngine::Statement> StorageEngine::prepare(std::string_view sql)
{
    if (m_impl->db == nullptr) {
        return make_error(ErrorKind::StorageError, "database not open");
    }

    sqlite3_stmt* raw = nullptr;
    const int rc =
        sqlite3_prepare_v2(m_impl->db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr);

    if (rc != SQLITE_OK || raw == nullptr) {
        std::string msg = sqlite3_errmsg(m_impl->db);
        return make_error(ErrorKind::StorageError, std::move(msg), std::string(sql).substr(0, 120));
    }

    auto impl = std::make_unique<Statement::Impl>();
    impl->stmt = raw;
    impl->db = m_impl->db;
    return Statement(std::move(impl));
}

// ============================================================================
// Transactions
// ============================================================================

Result<void> StorageEngine::begin_transaction()
{
    m_impl->write_mutex.lock();
    auto r = execute("BEGIN TRANSACTION");
    if (!r) {
        m_impl->write_mutex.unlock();
    }
    return r;
}

Result<void> StorageEngine::commit()
{
    auto r = execute("COMMIT");
    if (!r) {
        // COMMIT failed — SQLite transaction is still open. Roll it back
        // so the connection is usable again, then unlock.
        (void)execute("ROLLBACK");
    }
    m_impl->write_mutex.unlock();
    return r;
}

Result<void> StorageEngine::rollback()
{
    auto r = execute("ROLLBACK");
    m_impl->write_mutex.unlock();
    return r;
}

// ============================================================================
// Transaction RAII guard
// ============================================================================

StorageEngine::Transaction::Transaction(StorageEngine& engine) : m_engine(&engine)
{
    auto r = m_engine->begin_transaction();
    if (r) {
        m_active = true;
    }
    else {
        VECTIS_LOG_ERROR("Transaction: begin failed: {}", r.error().message);
    }
}

StorageEngine::Transaction::~Transaction()
{
    if (m_active) {
        auto r = m_engine->rollback();
        if (!r) {
            VECTIS_LOG_ERROR("Transaction: rollback in dtor failed: {}", r.error().message);
        }
    }
}

StorageEngine::Transaction::Transaction(Transaction&& other) noexcept
    : m_engine(other.m_engine), m_active(other.m_active)
{
    other.m_active = false;
}

StorageEngine::Transaction& StorageEngine::Transaction::operator=(Transaction&& other) noexcept
{
    if (this != &other) {
        if (m_active) {
            (void)m_engine->rollback(); // best-effort
            m_active = false;
        }
        m_engine = other.m_engine;
        m_active = other.m_active;
        other.m_active = false;
    }
    return *this;
}

Result<void> StorageEngine::Transaction::commit()
{
    if (!m_active) {
        return make_error(ErrorKind::StorageError, "transaction already finalized");
    }
    auto r = m_engine->commit();
    // Always mark inactive — commit() internally rolls back on failure
    // and unlocks the mutex, so the transaction is finalized either way.
    m_active = false;
    return r;
}

} // namespace vectis::services
