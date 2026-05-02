#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vectis::code { struct Symbol; }

namespace vectis::services {

class StorageEngine;

/// One search result from the FTS5 index.
struct SearchResult {
    std::string  source;          ///< "file" or "symbol"
    std::int64_t source_id = 0;   ///< ID in the source table
    std::string  title;           ///< filename or symbol name
    std::string  snippet;         ///< matching text excerpt
    double       score     = 0.0; ///< relevance (lower = better in FTS5 rank)
};

/// Full-text search engine backed by SQLite FTS5.
///
/// Provides indexing and search over file contents and symbol names.
/// Requires a StorageEngine that has been opened and migrated
/// (fts_content virtual table must exist).
class IndexEngine {
public:
    IndexEngine();
    ~IndexEngine();

    IndexEngine(const IndexEngine&)            = delete;
    IndexEngine& operator=(const IndexEngine&) = delete;
    IndexEngine(IndexEngine&&) noexcept;
    IndexEngine& operator=(IndexEngine&&) noexcept;

    /// Bind to a StorageEngine. Must be called before any other method.
    void initialize(StorageEngine& storage);

    // ----- Indexing ----------------------------------------------------------

    /// Index file content for full-text search.
    void index_file(std::int64_t file_id, std::string_view path, std::string_view content);

    /// Remove a file from the FTS index.
    void remove_file(std::int64_t file_id);

    /// Index symbols (name + signature) for full-text search.
    void index_symbols(std::int64_t file_id,
                       const std::vector<vectis::code::Symbol>& symbols);

    // ----- Search ------------------------------------------------------------

    /// Search across all sources (files + symbols).
    [[nodiscard]] std::vector<SearchResult>
    search(std::string_view query, int max_results = 20);

    /// Search file content only.
    [[nodiscard]] std::vector<SearchResult>
    search_files(std::string_view query, int max_results = 20);

    // ----- Stats -------------------------------------------------------------

    [[nodiscard]] int indexed_file_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::services
