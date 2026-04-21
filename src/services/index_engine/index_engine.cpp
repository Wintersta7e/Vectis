#include "services/index_engine/index_engine.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "code/symbol.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::services {

// ============================================================================
// Impl
// ============================================================================

struct IndexEngine::Impl {
    StorageEngine* storage = nullptr;
};

// ============================================================================
// Lifecycle
// ============================================================================

IndexEngine::IndexEngine()  : m_impl(std::make_unique<Impl>()) {}
IndexEngine::~IndexEngine() = default;

IndexEngine::IndexEngine(IndexEngine&&) noexcept            = default;
IndexEngine& IndexEngine::operator=(IndexEngine&&) noexcept = default;

void IndexEngine::initialize(StorageEngine& storage)
{
    m_impl->storage = &storage;
    VECTIS_LOG_INFO("IndexEngine: initialized");
}

// ============================================================================
// Indexing
// ============================================================================

void IndexEngine::index_file(std::int64_t file_id,
                             std::string_view path,
                             std::string_view content)
{
    if (m_impl->storage == nullptr) return;

    auto stmt = m_impl->storage->prepare(
        "INSERT INTO fts_content (source, source_id, title, body) "
        "VALUES ('file', ?, ?, ?)");
    if (!stmt) {
        VECTIS_LOG_WARN("IndexEngine: prepare failed for index_file: {}", stmt.error().message);
        return;
    }
    stmt->bind(1, file_id);
    stmt->bind(2, path);
    stmt->bind(3, content);
    if (auto r = stmt->execute(); !r) {
        VECTIS_LOG_WARN("IndexEngine: index_file execute failed: {}", r.error().message);
    }
}

void IndexEngine::remove_file(std::int64_t file_id)
{
    if (m_impl->storage == nullptr) return;

    auto stmt = m_impl->storage->prepare(
        "DELETE FROM fts_content WHERE source = 'file' AND source_id = ?");
    if (!stmt) return;
    stmt->bind(1, file_id);
    (void)stmt->execute();

    // Also remove symbols indexed for this file.
    auto stmt2 = m_impl->storage->prepare(
        "DELETE FROM fts_content WHERE source = 'symbol' AND source_id IN "
        "(SELECT id FROM symbols WHERE file_id = ?)");
    if (!stmt2) return;
    stmt2->bind(1, file_id);
    (void)stmt2->execute();
}

void IndexEngine::index_symbols(
    std::int64_t file_id,
    const std::vector<vectis::code::Symbol>& symbols)
{
    if (m_impl->storage == nullptr) return;
    (void)file_id; // symbols carry their own IDs

    auto stmt = m_impl->storage->prepare(
        "INSERT INTO fts_content (source, source_id, title, body) "
        "VALUES ('symbol', ?, ?, ?)");
    if (!stmt) {
        VECTIS_LOG_WARN("IndexEngine: prepare failed for index_symbols");
        return;
    }

    for (const auto& sym : symbols) {
        stmt->bind(1, sym.id);
        stmt->bind(2, std::string_view{sym.name});
        // Body = signature if present, else just the name again.
        const std::string_view body =
            sym.signature.empty() ? std::string_view{sym.name}
                                  : std::string_view{sym.signature};
        stmt->bind(3, body);
        if (auto r = stmt->execute(); !r) {
            VECTIS_LOG_WARN("IndexEngine: index_symbols execute failed: {}", r.error().message);
        }
        stmt->reset();
    }
}

// ============================================================================
// Search
// ============================================================================

namespace {

/// Escape FTS5 special characters in user queries by wrapping each
/// term in double quotes.
[[nodiscard]] std::string escape_fts_query(std::string_view query)
{
    // Simple strategy: wrap the entire query in double quotes to
    // treat it as a phrase match. If user typed special syntax
    // characters this prevents them from being interpreted.
    if (query.empty()) return {};
    std::string escaped;
    escaped.reserve(query.size() + 2);
    escaped.push_back('"');
    for (const char ch : query) {
        if (ch == '"') {
            // Escape embedded double quotes
            escaped.append("\"\"");
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::vector<SearchResult> run_search(StorageEngine* storage,
                                     std::string_view query,
                                     const char* source_filter,
                                     int max_results)
{
    std::vector<SearchResult> results;
    if (storage == nullptr || query.empty()) return results;

    const auto escaped = escape_fts_query(query);

    std::string sql =
        "SELECT source, source_id, title, "
        "snippet(fts_content, 3, '<b>', '</b>', '...', 30) AS snippet, "
        "rank "
        "FROM fts_content "
        "WHERE fts_content MATCH ?";

    if (source_filter != nullptr) {
        sql += " AND source = '";
        sql += source_filter;
        sql += "'";
    }

    sql += " ORDER BY rank LIMIT ?";

    auto stmt = storage->prepare(sql);
    if (!stmt) return results;

    stmt->bind(1, std::string_view{escaped});
    stmt->bind(2, static_cast<std::int64_t>(max_results));

    auto rows = stmt->query();
    if (!rows) return results;

    results.reserve(rows->size());
    for (const auto& row : *rows) {
        SearchResult sr;
        sr.source    = row.get_text(0);
        sr.source_id = row.get_int(1);
        sr.title     = row.get_text(2);
        sr.snippet   = row.get_text(3);
        sr.score     = row.get_real(4);
        results.push_back(std::move(sr));
    }
    return results;
}

} // namespace

std::vector<SearchResult> IndexEngine::search(std::string_view query, int max_results)
{
    return run_search(m_impl->storage, query, nullptr, max_results);
}

std::vector<SearchResult> IndexEngine::search_files(std::string_view query, int max_results)
{
    return run_search(m_impl->storage, query, "file", max_results);
}

// ============================================================================
// Stats
// ============================================================================

int IndexEngine::indexed_file_count() const
{
    if (m_impl->storage == nullptr) return 0;

    auto stmt = m_impl->storage->prepare(
        "SELECT COUNT(*) FROM fts_content WHERE source = 'file'");
    if (!stmt) return 0;
    auto rows = stmt->query();
    if (!rows || rows->empty()) return 0;
    return static_cast<int>((*rows)[0].get_int(0));
}

} // namespace vectis::services
