#include "code/code_index_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/result.h"
#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/symbol.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::code {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;
using vectis::services::StorageEngine;

namespace {

/// Serialize Symbol::members as a newline-joined string.
[[nodiscard]] std::string join_members(const std::vector<std::string>& members)
{
    std::string out;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out += members[i];
    }
    return out;
}

/// Deserialize a newline-joined string back to members.
[[nodiscard]] std::vector<std::string> split_members(const std::string& joined)
{
    std::vector<std::string> result;
    if (joined.empty()) {
        return result;
    }
    std::string::size_type start = 0;
    while (true) {
        const auto pos = joined.find('\n', start);
        if (pos == std::string::npos) {
            result.push_back(joined.substr(start));
            break;
        }
        result.push_back(joined.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

/// Convert file_time_type to seconds since epoch.
///
/// MSVC's `std::chrono::file_clock` (= `std::filesystem::_File_time_clock`)
/// does not expose `to_sys`/`from_sys` as static members; libstdc++ does.
/// `std::chrono::clock_cast` is the C++20-standard portable bridge on both.
[[nodiscard]] std::int64_t to_epoch_seconds(std::filesystem::file_time_type ft)
{
    const auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::seconds>(
        sys_time.time_since_epoch()).count();
}

/// Convert seconds since epoch to file_time_type.
[[nodiscard]] std::filesystem::file_time_type from_epoch_seconds(std::int64_t epoch)
{
    const auto sys_time = std::chrono::sys_seconds{std::chrono::seconds{epoch}};
    return std::chrono::clock_cast<std::chrono::file_clock>(sys_time);
}

} // namespace

// ============================================================================
// save_index
// ============================================================================

Result<void> save_index(StorageEngine&       storage,
                        const CodeIndex&     index,
                        const CacheMetadata& metadata)
{
    StorageEngine::Transaction txn(storage);
    if (!txn.is_active()) {
        return make_error(ErrorKind::StorageError, "failed to begin transaction for save_index");
    }

    // Clear existing data.
    if (auto r = storage.execute("DELETE FROM dependencies"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM symbols"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM files"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM fts_content"); !r) { return r;
}
    if (auto r = storage.execute(
            "DELETE FROM kv_store WHERE key LIKE 'cache.%'"); !r) {
        return r;
}

    // Insert files.
    auto ins_file = storage.prepare(
        "INSERT INTO files (id, path, language, size, line_count, "
        "last_modified, last_indexed, content_hash) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    if (!ins_file) { return tl::unexpected(ins_file.error());
}

    const auto files = index.snapshot_files();
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& f : files) {
        ins_file->bind(1, f.id);
        ins_file->bind(2, std::string_view{f.path_relative.string()});
        ins_file->bind(3, language_name(f.language));
        ins_file->bind(4, static_cast<std::int64_t>(f.size));
        ins_file->bind(5, static_cast<std::int64_t>(f.line_count));
        ins_file->bind(6, to_epoch_seconds(f.last_modified));
        ins_file->bind(7, static_cast<std::int64_t>(now_epoch));
        ins_file->bind(8, std::string_view{f.content_hash});
        if (auto r = ins_file->execute(); !r) { return r;
}
        ins_file->reset();
    }

    // Insert symbols.
    auto ins_sym = storage.prepare(
        "INSERT INTO symbols (id, file_id, name, kind, signature, "
        "line_start, line_end, parent_id, complexity, members) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!ins_sym) { return tl::unexpected(ins_sym.error());
}

    for (const auto& f : files) {
        const auto syms = index.symbols_in_file(f.id);
        for (const auto& s : syms) {
            ins_sym->bind(1, s.id);
            ins_sym->bind(2, s.file_id);
            ins_sym->bind(3, std::string_view{s.name});
            ins_sym->bind(4, symbol_kind_name(s.kind));
            ins_sym->bind(5, std::string_view{s.signature});
            ins_sym->bind(6, static_cast<std::int64_t>(s.line_start));
            ins_sym->bind(7, static_cast<std::int64_t>(s.line_end));
            ins_sym->bind(8, s.parent_id);
            ins_sym->bind(9, static_cast<std::int64_t>(s.complexity));
            const auto members_str = join_members(s.members);
            ins_sym->bind(10, std::string_view{members_str});
            if (auto r = ins_sym->execute(); !r) { return r;
}
            ins_sym->reset();
        }
    }

    // Insert dependencies.
    auto ins_dep = storage.prepare(
        "INSERT OR IGNORE INTO dependencies "
        "(source_file_id, target_file_id, kind, import_string) "
        "VALUES (?, ?, ?, ?)");
    if (!ins_dep) { return tl::unexpected(ins_dep.error());
}

    const auto deps = index.all_dependencies();
    for (const auto& d : deps) {
        ins_dep->bind(1, d.source_file_id);
        ins_dep->bind(2, d.target_file_id);
        ins_dep->bind(3, std::string_view{d.kind});
        ins_dep->bind(4, std::string_view{d.import_string});
        if (auto r = ins_dep->execute(); !r) { return r;
}
        ins_dep->reset();
    }

    // Write cache metadata.
    auto ins_kv = storage.prepare(
        "INSERT OR REPLACE INTO kv_store (key, value) VALUES (?, ?)");
    if (!ins_kv) { return tl::unexpected(ins_kv.error());
}

    ins_kv->bind(1, std::string_view{"cache.project_root"});
    ins_kv->bind(2, std::string_view{metadata.project_root.string()});
    if (auto r = ins_kv->execute(); !r) { return r;
}
    ins_kv->reset();

    ins_kv->bind(1, std::string_view{"cache.scan_timestamp"});
    ins_kv->bind(2, std::string_view{metadata.scan_timestamp});
    if (auto r = ins_kv->execute(); !r) { return r;
}
    ins_kv->reset();

    VECTIS_LOG_INFO(
        "CodeIndexStore: saved {} files, {} deps to cache",
        files.size(), deps.size());

    return txn.commit();
}

// ============================================================================
// load_index
// ============================================================================

Result<CacheMetadata> load_index(StorageEngine& storage, CodeIndex& index)
{
    // Read cache metadata first.
    auto kv = storage.prepare("SELECT value FROM kv_store WHERE key = ?");
    if (!kv) { return tl::unexpected(kv.error());
}

    kv->bind(1, std::string_view{"cache.project_root"});
    auto kv_rows = kv->query();
    if (!kv_rows) { return tl::unexpected(kv_rows.error());
}

    if (kv_rows->empty()) {
        return make_error(ErrorKind::StorageError, "no cache found (missing project_root key)");
    }

    CacheMetadata metadata;
    metadata.project_root = (*kv_rows)[0].get_text(0);

    kv->reset();
    kv->bind(1, std::string_view{"cache.scan_timestamp"});
    kv_rows = kv->query();
    if (kv_rows && !kv_rows->empty()) {
        metadata.scan_timestamp = (*kv_rows)[0].get_text(0);
    }

    // Load files.
    auto sel_files = storage.prepare(
        "SELECT id, path, language, size, line_count, last_modified, "
        "content_hash FROM files ORDER BY id ASC");
    if (!sel_files) { return tl::unexpected(sel_files.error());
}

    auto file_rows = sel_files->query();
    if (!file_rows) { return tl::unexpected(file_rows.error());
}

    if (file_rows->empty()) {
        return make_error(ErrorKind::StorageError, "cache is empty (no files)");
    }

    for (const auto& row : *file_rows) {
        FileEntry f;
        // id will be reassigned by add_file; we rely on ORDER BY id ASC
        // so the assigned IDs match the original ones.
        f.path_relative  = row.get_text(1);
        f.language       = language_from_name(row.get_text(2));
        f.size           = static_cast<std::uint64_t>(row.get_int(3));
        f.line_count     = static_cast<int>(row.get_int(4));
        f.last_modified  = from_epoch_seconds(row.get_int(5));
        f.content_hash   = row.get_text(6);
        index.add_file(std::move(f));
    }

    // Load symbols — batch by file_id for add_symbols.
    auto sel_syms = storage.prepare(
        "SELECT id, file_id, name, kind, signature, line_start, line_end, "
        "parent_id, complexity, members FROM symbols ORDER BY file_id, id");
    if (!sel_syms) { return tl::unexpected(sel_syms.error());
}

    auto sym_rows = sel_syms->query();
    if (!sym_rows) { return tl::unexpected(sym_rows.error());
}

    std::int64_t        current_file = -1;
    std::vector<Symbol> batch;

    const auto flush_batch = [&]() {
        if (!batch.empty()) {
            index.add_symbols(batch);
            batch.clear();
        }
    };

    for (const auto& row : *sym_rows) {
        const std::int64_t file_id = row.get_int(1);
        if (file_id != current_file) {
            flush_batch();
            current_file = file_id;
        }

        Symbol s;
        // s.id will be reassigned by add_symbols
        s.file_id    = file_id;
        s.name       = row.get_text(2);
        s.kind       = symbol_kind_from_name(row.get_text(3));
        s.signature  = row.get_text(4);
        s.line_start = static_cast<int>(row.get_int(5));
        s.line_end   = static_cast<int>(row.get_int(6));
        s.parent_id  = row.get_int(7);
        s.complexity = static_cast<int>(row.get_int(8));
        s.members    = split_members(row.get_text(9));
        batch.push_back(std::move(s));
    }
    flush_batch();

    // Load dependencies.
    auto sel_deps = storage.prepare(
        "SELECT source_file_id, target_file_id, kind, import_string "
        "FROM dependencies");
    if (!sel_deps) { return tl::unexpected(sel_deps.error());
}

    auto dep_rows = sel_deps->query();
    if (!dep_rows) { return tl::unexpected(dep_rows.error());
}

    for (const auto& row : *dep_rows) {
        Dependency d;
        d.source_file_id = row.get_int(0);
        d.target_file_id = row.get_int(1);
        d.kind           = row.get_text(2);
        d.import_string  = row.get_text(3);
        index.add_dependency(std::move(d));
    }

    VECTIS_LOG_INFO(
        "CodeIndexStore: loaded {} files, {} symbols, {} deps from cache",
        index.file_count(), index.symbol_count(), index.dependency_count());

    return metadata;
}

// ============================================================================
// has_cache_for
// ============================================================================

bool has_cache_for(StorageEngine&               storage,
                   const std::filesystem::path& project_root)
{
    auto kv = storage.prepare("SELECT value FROM kv_store WHERE key = ?");
    if (!kv) { return false;
}
    kv->bind(1, std::string_view{"cache.project_root"});
    auto rows = kv->query();
    if (!rows || rows->empty()) { return false;
}
    return (*rows)[0].get_text(0) == project_root.string();
}

// ============================================================================
// clear_cache
// ============================================================================

Result<void> clear_cache(StorageEngine& storage)
{
    StorageEngine::Transaction txn(storage);
    if (!txn.is_active()) {
        return make_error(ErrorKind::StorageError, "failed to begin transaction for clear_cache");
    }
    if (auto r = storage.execute("DELETE FROM dependencies"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM symbols"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM files"); !r) { return r;
}
    if (auto r = storage.execute("DELETE FROM fts_content"); !r) { return r;
}
    if (auto r = storage.execute(
            "DELETE FROM kv_store WHERE key LIKE 'cache.%'"); !r) { return r;
}
    return txn.commit();
}

} // namespace vectis::code
