#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace vectis::services {

/// One numbered schema migration applied during StorageEngine::migrate().
struct Migration
{
    int version;
    std::string_view name;
    std::string_view sql;
};

// clang-format off
/// All schema migrations in version order.
///
/// Step 5 defines v1 (files, symbols, dependencies, kv_store, FTS5).
/// Future steps append entries here — never modify existing migrations.
inline constexpr std::array k_migrations = {
    Migration{1, "initial_schema", R"(
        CREATE TABLE IF NOT EXISTS files (
            id             INTEGER PRIMARY KEY,
            path           TEXT    NOT NULL UNIQUE,
            language       TEXT,
            size           INTEGER,
            line_count     INTEGER,
            last_modified  INTEGER,
            last_indexed   INTEGER,
            content_hash   TEXT
        );

        CREATE TABLE IF NOT EXISTS symbols (
            id          INTEGER PRIMARY KEY,
            file_id     INTEGER REFERENCES files(id) ON DELETE CASCADE,
            name        TEXT    NOT NULL,
            kind        TEXT    NOT NULL,
            signature   TEXT,
            line_start  INTEGER,
            line_end    INTEGER,
            parent_id   INTEGER,
            complexity  INTEGER,
            members     TEXT
        );

        CREATE TABLE IF NOT EXISTS dependencies (
            source_file_id INTEGER REFERENCES files(id) ON DELETE CASCADE,
            target_file_id INTEGER REFERENCES files(id) ON DELETE CASCADE,
            kind           TEXT,
            import_string  TEXT,
            PRIMARY KEY (source_file_id, target_file_id, kind)
        );

        CREATE TABLE IF NOT EXISTS kv_store (
            key   TEXT PRIMARY KEY,
            value TEXT
        );

        CREATE VIRTUAL TABLE IF NOT EXISTS fts_content USING fts5(
            source,
            source_id UNINDEXED,
            title,
            body,
            tokenize='porter unicode61'
        );

        CREATE INDEX IF NOT EXISTS idx_symbols_file_id ON symbols(file_id);
        CREATE INDEX IF NOT EXISTS idx_symbols_name    ON symbols(name);
        CREATE INDEX IF NOT EXISTS idx_deps_source     ON dependencies(source_file_id);
        CREATE INDEX IF NOT EXISTS idx_deps_target     ON dependencies(target_file_id);
    )"},
    Migration{2, "symbol_visibility", R"(
        ALTER TABLE symbols ADD COLUMN visibility TEXT DEFAULT '';
    )"},
    Migration{3, "symbol_decorators", R"(
        ALTER TABLE symbols ADD COLUMN decorators TEXT DEFAULT '';
    )"},
    Migration{4, "drop_target_file_id_fk", R"(
        -- v1's `dependencies` table declared
        -- `target_file_id INTEGER REFERENCES files(id) ON DELETE CASCADE`,
        -- but the dependency model uses `target_file_id = 0` for
        -- external / unresolved imports (`<vector>`, `java.util.List`,
        -- etc.). With `PRAGMA foreign_keys=ON` every external import
        -- failed the FK check on save, which rolled the whole
        -- transaction back and left the cache empty. Source-side FK
        -- stays — every dep does originate in a real scanned file.
        CREATE TABLE dependencies_new (
            source_file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
            target_file_id INTEGER,
            kind           TEXT,
            import_string  TEXT,
            PRIMARY KEY (source_file_id, target_file_id, kind, import_string)
        );
        INSERT OR IGNORE INTO dependencies_new
            SELECT source_file_id, target_file_id, kind, import_string
            FROM dependencies;
        DROP TABLE dependencies;
        ALTER TABLE dependencies_new RENAME TO dependencies;
        CREATE INDEX IF NOT EXISTS idx_deps_source ON dependencies(source_file_id);
        CREATE INDEX IF NOT EXISTS idx_deps_target ON dependencies(target_file_id);
    )"},
};
// clang-format on

inline constexpr std::size_t k_migration_count = k_migrations.size();

} // namespace vectis::services
