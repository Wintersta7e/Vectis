#include "code/scanner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "code/code_index.h"
#include "code/dependency_resolver.h"
#include "code/exclude_dirs.h"
#include "code/gitignore.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/string_util.h"
#include "platform/file_io.h"

namespace vectis::code {

namespace {

/// Skip files larger than 2 MB — they're typically minified bundles,
/// generated artifacts, or otherwise unhelpful for symbol extraction.
constexpr std::uint64_t k_max_file_size_bytes = 2ULL * 1024 * 1024;

/// Publish progress at most once every 50 files OR every 100 ms,
/// whichever fires first. Scan loops that publish per file dominate
/// their own runtime on large codebases due to bus mutex contention.
constexpr std::size_t k_progress_file_stride = 50;
constexpr std::chrono::milliseconds k_progress_time_stride{100};

/// Cheap binary-file heuristic: any NUL byte in the first 512 bytes
/// means "don't even try to parse this". Catches images, archives,
/// compiled artifacts, and most non-text formats with zero false
/// positives on real source code.
[[nodiscard]] bool looks_binary(const std::string& content) noexcept
{
    const std::size_t limit = std::min<std::size_t>(content.size(), 512);
    for (std::size_t i = 0; i < limit; ++i) {
        if (content[i] == '\0') {
            return true;
        }
    }
    return false;
}

/// Reason the current scan should stop, or an empty view to keep
/// going. Checked at every loop boundary so cancellation and epoch
/// pre-emption surface immediately.
[[nodiscard]] std::string_view
scan_preemption_reason(const vectis::core::CancellationToken& cancel_token,
                       const std::atomic<std::int64_t>& current_epoch,
                       std::int64_t expected_epoch) noexcept
{
    if (cancel_token.stop_requested()) {
        return "cancelled by token";
    }
    if (current_epoch.load(std::memory_order_acquire) != expected_epoch) {
        return "pre-empted by epoch bump";
    }
    return {};
}

} // namespace

vectis::core::Result<ScanResult>
Scanner::run_collect(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                     const ProgressCallback& on_progress,
                     const vectis::core::CancellationToken& cancel_token,
                     const std::atomic<std::int64_t>& current_epoch)
{
    using vectis::core::ErrorKind;
    using vectis::core::make_error;

    VECTIS_LOG_INFO("Scanner: starting scan of '{}'", config.root.string());

    std::size_t files_seen = 0;
    std::uint64_t files_skipped = 0;
    auto last_publish = std::chrono::steady_clock::now();

    // Per-file raw imports collected during the scan. Resolution is
    // deferred to `resolve_all` after the main loop exits so the
    // resolver sees the complete file table.
    std::vector<FileImports> per_file_imports;

    std::error_code ec;
    if (!std::filesystem::is_directory(config.root, ec) || ec) {
        VECTIS_LOG_ERROR("Scanner: root '{}' is not a directory: {}", config.root.string(),
                         ec.message());
        return make_error(ErrorKind::IoError, "scan root is not a directory: " + ec.message(),
                          config.root.string());
    }

    using Iter = std::filesystem::recursive_directory_iterator;
    const auto options = std::filesystem::directory_options::skip_permission_denied;

    Iter it{};
    try {
        it = Iter{config.root, options, ec};
    }
    catch (const std::exception& e) {
        VECTIS_LOG_ERROR("Scanner: failed to open recursive iterator: {}", e.what());
        return make_error(ErrorKind::PlatformError,
                          std::string{"recursive_directory_iterator threw: "} + e.what(),
                          config.root.string());
    }
    if (ec) {
        VECTIS_LOG_ERROR("Scanner: recursive_directory_iterator init failed: {}", ec.message());
        return make_error(ErrorKind::IoError,
                          "recursive_directory_iterator init failed: " + ec.message(),
                          config.root.string());
    }

    const Iter end_it{};
    for (; it != end_it;) {
        if (const std::string_view reason =
                scan_preemption_reason(cancel_token, current_epoch, config.epoch);
            !reason.empty()) {
            VECTIS_LOG_INFO("Scanner: scan {}", reason);
            return make_error(vectis::core::ErrorKind::Cancelled, std::string{reason},
                              config.root.string());
        }

        const std::filesystem::directory_entry& entry = *it;

        // Directory-level skip: disable recursion for excluded subtrees.
        if (entry.is_directory(ec)) {
            if (is_excluded_basename(entry.path(), config.exclude_dir_names,
                                     config.exclude_dir_globs)) {
                it.disable_recursion_pending();
            }
            try {
                it.increment(ec);
            }
            catch (const std::exception& e) {
                VECTIS_LOG_WARN("Scanner: directory increment threw: {}", e.what());
                ec.clear();
                break;
            }
            if (ec) {
                VECTIS_LOG_WARN("Scanner: directory increment error: {}", ec.message());
                ec.clear();
            }
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            try {
                it.increment(ec);
            }
            catch (const std::exception& e) {
                VECTIS_LOG_WARN("Scanner: non-file increment threw: {}", e.what());
                ec.clear();
                break;
            }
            continue;
        }

        const std::filesystem::path& path = entry.path();
        const Language language = detect_language(path);
        if (language == Language::JavaScript && looks_like_vendored_js(path.filename().string())) {
            ++files_skipped;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }
        if (language == Language::Unknown) {
            ++files_skipped;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::uint64_t size = entry.file_size(ec);
        if (ec || size > k_max_file_size_bytes) {
            if (size > k_max_file_size_bytes) {
                VECTIS_LOG_DEBUG("Scanner: skipping large file '{}' ({} bytes)", path.string(),
                                 size);
            }
            ec.clear();
            ++files_skipped;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        auto read_result = vectis::platform::read_file(path);
        if (!read_result) {
            VECTIS_LOG_DEBUG("Scanner: read_file failed for '{}': {}", path.string(),
                             read_result.error().message);
            ++files_skipped;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::string& content = *read_result;
        if (looks_binary(content)) {
            VECTIS_LOG_DEBUG("Scanner: skipping binary file '{}'", path.string());
            ++files_skipped;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const Language refined = refine_language(language, path, content);

        // Parse symbols and import/namespace tables up-front so the
        // namespaces can be persisted on the FileEntry — the warm
        // cache loader needs them so an incremental rescan of a
        // changed sibling can still see this file's namespace.
        auto parse_result = parser.parse_file(refined, content);
        auto raw_imports = parser.extract_imports(refined, content);
        auto declared_ns = parser.extract_namespaces(refined, content);

        FileEntry file_entry;
        file_entry.path_relative = std::filesystem::relative(path, config.root, ec);
        if (ec) {
            file_entry.path_relative = path;
            ec.clear();
        }
        file_entry.language = refined;
        file_entry.size = size;
        file_entry.line_count = vectis::core::count_lines(content);
        file_entry.content_hash = vectis::core::fnv1a_hex(content);
        file_entry.declared_namespaces = declared_ns;
        const auto last_write = entry.last_write_time(ec);
        if (!ec) {
            file_entry.last_modified = last_write;
        }
        ec.clear();

        // Save the relative path before moving file_entry into the
        // index — we need it for the dependency resolver later.
        std::filesystem::path relative_path = file_entry.path_relative;
        const std::int64_t file_id = index.add_file(std::move(file_entry));

        if (!parse_result.symbols.empty()) {
            for (Symbol& sym : parse_result.symbols) {
                sym.file_id = file_id;
            }
            index.add_symbols(parse_result.symbols);
        }

        // A file with no imports but a namespace declaration is still
        // pushed — the resolver builds a namespace → file-ids map
        // across the whole batch, so a C# `Models/User.cs` that only
        // declares `namespace SampleApp.Models` (no `using`) must be
        // visible to it.
        if (!raw_imports.empty() || !declared_ns.empty()) {
            FileImports fi;
            fi.file_id = file_id;
            fi.language = refined;
            fi.relative_path = std::move(relative_path);
            fi.imports = std::move(raw_imports);
            fi.declared_namespaces = std::move(declared_ns);
            per_file_imports.push_back(std::move(fi));
        }

        ++files_seen;

        // Throttled progress publication — see k_progress_* constants.
        const auto now = std::chrono::steady_clock::now();
        if (files_seen % k_progress_file_stride == 0 ||
            (now - last_publish) >= k_progress_time_stride) {
            ScanProgress progress;
            progress.files_scanned = files_seen;
            progress.files_skipped = files_skipped;
            progress.current_path = std::filesystem::relative(path, config.root, ec).string();
            if (ec) {
                progress.current_path = path.string();
                ec.clear();
            }
            if (on_progress) {
                on_progress(progress);
            }
            last_publish = now;
        }

        try {
            it.increment(ec);
        }
        catch (const std::exception& e) {
            VECTIS_LOG_WARN("Scanner: file increment threw: {}", e.what());
            ec.clear();
            break;
        }
    }

    if (const std::string_view reason =
            scan_preemption_reason(cancel_token, current_epoch, config.epoch);
        !reason.empty()) {
        VECTIS_LOG_INFO("Scanner: scan {}", reason);
        return make_error(vectis::core::ErrorKind::Cancelled, std::string{reason},
                          config.root.string());
    }

    ScanResult scan;
    scan.summary.file_count = index.file_count();
    scan.summary.symbol_count = index.symbol_count();
    scan.summary.language_count = index.language_count();
    scan.summary.files_skipped = files_skipped;
    scan.per_file_imports = std::move(per_file_imports);

    VECTIS_LOG_INFO("Scanner: collect done — {} files, {} symbols, {} languages ({} skipped)",
                    scan.summary.file_count, scan.summary.symbol_count, scan.summary.language_count,
                    files_skipped);

    return scan;
}

// ============================================================================
// Incremental scan
// ============================================================================

vectis::core::Result<ScanResult>
Scanner::run_incremental_collect(const ScanConfig& config, CodeIndex& index,
                                 TreeSitterParser& parser, const ProgressCallback& on_progress,
                                 const vectis::core::CancellationToken& cancel_token,
                                 const std::atomic<std::int64_t>& current_epoch)
{
    using vectis::core::ErrorKind;
    using vectis::core::make_error;

    VECTIS_LOG_INFO("Scanner: starting incremental scan of '{}'", config.root.string());

    // Build a map of existing files: relative_path → cached metadata.
    // declared_namespaces travels alongside so the unchanged branch can
    // restore the resolver's namespace lookup without re-parsing.
    struct ExistingFileInfo
    {
        std::int64_t id = 0;
        std::string content_hash;
        Language language = Language::Unknown;
        std::vector<std::string> declared_namespaces;
    };
    const auto existing_files = index.snapshot_files();
    std::unordered_map<std::string, ExistingFileInfo> path_to_info;
    path_to_info.reserve(existing_files.size());
    for (const auto& f : existing_files) {
        path_to_info[f.path_relative.generic_string()] =
            ExistingFileInfo{f.id, f.content_hash, f.language, f.declared_namespaces};
    }

    // Track which existing paths were visited. Lives on the result so
    // the caller can extend it during a manifest pass before calling
    // `prune_missing`.
    ScanResult scan;
    scan.visited_paths.reserve(existing_files.size());
    auto& visited_paths = scan.visited_paths;
    std::uint64_t files_skipped_local = 0;
    std::size_t files_seen = 0;
    auto last_publish = std::chrono::steady_clock::now();

    // Imports collected for the dependency resolver pass.
    std::vector<FileImports> per_file_imports;

    std::error_code ec;
    if (!std::filesystem::is_directory(config.root, ec) || ec) {
        return make_error(ErrorKind::IoError, "scan root is not a directory", config.root.string());
    }

    using Iter = std::filesystem::recursive_directory_iterator;
    const auto options = std::filesystem::directory_options::skip_permission_denied;

    Iter it{};
    try {
        it = Iter{config.root, options, ec};
    }
    catch (const std::exception& e) {
        return make_error(ErrorKind::PlatformError,
                          std::string{"recursive_directory_iterator threw: "} + e.what(),
                          config.root.string());
    }
    if (ec) {
        return make_error(ErrorKind::IoError,
                          "recursive_directory_iterator init failed: " + ec.message(),
                          config.root.string());
    }

    const Iter end_it{};
    for (; it != end_it;) {
        if (const std::string_view reason =
                scan_preemption_reason(cancel_token, current_epoch, config.epoch);
            !reason.empty()) {
            VECTIS_LOG_INFO("Scanner: incremental scan {}", reason);
            return make_error(ErrorKind::Cancelled, std::string{reason}, config.root.string());
        }

        const std::filesystem::directory_entry& entry = *it;

        if (entry.is_directory(ec)) {
            if (is_excluded_basename(entry.path(), config.exclude_dir_names,
                                     config.exclude_dir_globs)) {
                it.disable_recursion_pending();
            }
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            if (ec) {
                ec.clear();
            }
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::filesystem::path& path = entry.path();
        const Language language = detect_language(path);
        if (language == Language::JavaScript && looks_like_vendored_js(path.filename().string())) {
            ++files_skipped_local;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }
        if (language == Language::Unknown) {
            ++files_skipped_local;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::uint64_t size = entry.file_size(ec);
        if (ec || size > k_max_file_size_bytes) {
            ec.clear();
            ++files_skipped_local;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        auto read_result = vectis::platform::read_file(path);
        if (!read_result) {
            ++files_skipped_local;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::string& content = *read_result;
        if (looks_binary(content)) {
            ++files_skipped_local;
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const Language refined = refine_language(language, path, content);

        const auto rel = std::filesystem::relative(path, config.root, ec);
        if (ec) {
            ec.clear();
            try {
                it.increment(ec);
            }
            catch (...) {
                break;
            }
            continue;
        }
        const std::string rel_str = rel.generic_string();

        visited_paths.insert(rel_str);
        const auto new_hash = vectis::core::fnv1a_hex(content);

        const auto existing_it = path_to_info.find(rel_str);
        if (existing_it != path_to_info.end()) {
            if (existing_it->second.content_hash == new_hash) {
                // Unchanged — skip the parse, but still hand the
                // resolver this file's declared namespaces so a new or
                // modified file elsewhere in the tree can resolve
                // imports targeting them. Without this an incremental
                // rescan loses every namespace declared in files that
                // happen not to have changed.
                if (!existing_it->second.declared_namespaces.empty()) {
                    FileImports fi;
                    fi.file_id = existing_it->second.id;
                    fi.language = existing_it->second.language;
                    fi.relative_path = rel;
                    fi.declared_namespaces = existing_it->second.declared_namespaces;
                    per_file_imports.push_back(std::move(fi));
                }
                ++scan.files_unchanged;
            }
            else {
                // Modified — remove old, re-add.
                index.remove_file(existing_it->second.id);

                auto parse_result = parser.parse_file(refined, content);
                auto raw_imports = parser.extract_imports(refined, content);
                auto declared_ns = parser.extract_namespaces(refined, content);

                FileEntry file_entry;
                file_entry.path_relative = rel;
                file_entry.language = refined;
                file_entry.size = size;
                file_entry.line_count = vectis::core::count_lines(content);
                file_entry.content_hash = new_hash;
                file_entry.declared_namespaces = declared_ns;
                const auto last_write = entry.last_write_time(ec);
                if (!ec) {
                    file_entry.last_modified = last_write;
                }
                ec.clear();

                const std::int64_t file_id = index.add_file(std::move(file_entry));

                if (!parse_result.symbols.empty()) {
                    for (Symbol& sym : parse_result.symbols) {
                        sym.file_id = file_id;
                    }
                    index.add_symbols(parse_result.symbols);
                }

                if (!raw_imports.empty() || !declared_ns.empty()) {
                    FileImports fi;
                    fi.file_id = file_id;
                    fi.language = refined;
                    fi.relative_path = rel;
                    fi.imports = std::move(raw_imports);
                    fi.declared_namespaces = std::move(declared_ns);
                    per_file_imports.push_back(std::move(fi));
                }

                ++scan.files_updated;
            }
        }
        else {
            // New file — add.
            auto parse_result = parser.parse_file(refined, content);
            auto raw_imports = parser.extract_imports(refined, content);
            auto declared_ns = parser.extract_namespaces(refined, content);

            FileEntry file_entry;
            file_entry.path_relative = rel;
            file_entry.language = refined;
            file_entry.size = size;
            file_entry.line_count = vectis::core::count_lines(content);
            file_entry.content_hash = new_hash;
            file_entry.declared_namespaces = declared_ns;
            const auto last_write = entry.last_write_time(ec);
            if (!ec) {
                file_entry.last_modified = last_write;
            }
            ec.clear();

            const std::int64_t file_id = index.add_file(std::move(file_entry));

            if (!parse_result.symbols.empty()) {
                for (Symbol& sym : parse_result.symbols) {
                    sym.file_id = file_id;
                }
                index.add_symbols(parse_result.symbols);
            }

            if (!raw_imports.empty() || !declared_ns.empty()) {
                FileImports fi;
                fi.file_id = file_id;
                fi.language = refined;
                fi.relative_path = rel;
                fi.imports = std::move(raw_imports);
                fi.declared_namespaces = std::move(declared_ns);
                per_file_imports.push_back(std::move(fi));
            }

            ++scan.files_added;
        }

        ++files_seen;

        const auto now = std::chrono::steady_clock::now();
        if (files_seen % k_progress_file_stride == 0 ||
            (now - last_publish) >= k_progress_time_stride) {
            ScanProgress progress;
            progress.files_scanned = files_seen;
            progress.current_path = rel_str;
            if (on_progress) {
                on_progress(progress);
            }
            last_publish = now;
        }

        try {
            it.increment(ec);
        }
        catch (...) {
            ec.clear();
            break;
        }
    }

    scan.summary.file_count = index.file_count();
    scan.summary.symbol_count = index.symbol_count();
    scan.summary.language_count = index.language_count();
    scan.summary.files_skipped = files_skipped_local;
    scan.per_file_imports = std::move(per_file_imports);

    VECTIS_LOG_INFO("Scanner: incremental collect done — {} added, {} updated, {} unchanged",
                    scan.files_added, scan.files_updated, scan.files_unchanged);

    return scan;
}

// ============================================================================
// Edge resolution + prune sweep + thin run / run_incremental wrappers
// ============================================================================

void Scanner::resolve(CodeIndex& index, const std::filesystem::path& project_root,
                      const std::vector<FileImports>& per_file)
{
    if (per_file.empty()) {
        return;
    }
    resolve_all(index, project_root, per_file);
}

std::size_t Scanner::prune_missing(CodeIndex& index,
                                   const std::unordered_set<std::string>& visited_paths)
{
    std::size_t removed = 0;
    // Snapshot current file table; mutating remove_file while iterating
    // the live index would invalidate iterators / lookup maps.
    const auto files = index.snapshot_files();
    for (const auto& f : files) {
        if (visited_paths.find(f.path_relative.generic_string()) == visited_paths.end()) {
            index.remove_file(f.id);
            ++removed;
        }
    }
    return removed;
}

vectis::core::Result<ScanSummary> Scanner::run(const ScanConfig& config, CodeIndex& index,
                                               TreeSitterParser& parser,
                                               const ProgressCallback& on_progress,
                                               const CompletionCallback& on_complete,
                                               const vectis::core::CancellationToken& cancel_token,
                                               const std::atomic<std::int64_t>& current_epoch)
{
    auto scan = run_collect(config, index, parser, on_progress, cancel_token, current_epoch);
    if (!scan) {
        return tl::unexpected<vectis::core::Error>(scan.error());
    }

    resolve(index, config.root, scan->per_file_imports);

    ScanSummary summary = scan->summary;
    VECTIS_LOG_INFO(
        "Scanner: scan complete — {} files, {} symbols, {} languages, {} deps ({} skipped)",
        summary.file_count, summary.symbol_count, summary.language_count, index.dependency_count(),
        summary.files_skipped);

    if (on_complete) {
        on_complete(summary);
    }
    return summary;
}

vectis::core::Result<IncrementalScanResult>
Scanner::run_incremental(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                         const ProgressCallback& on_progress,
                         const vectis::core::CancellationToken& cancel_token,
                         const std::atomic<std::int64_t>& current_epoch)
{
    auto scan =
        run_incremental_collect(config, index, parser, on_progress, cancel_token, current_epoch);
    if (!scan) {
        return tl::unexpected<vectis::core::Error>(scan.error());
    }

    const std::size_t deleted = prune_missing(index, scan->visited_paths);
    resolve(index, config.root, scan->per_file_imports);

    // Reclaim soft-delete tombstones once per scan — long-running
    // incremental sessions would otherwise let the underlying vectors
    // grow without bound.
    if (deleted > 0 || scan->files_updated > 0) {
        index.compact();
    }

    IncrementalScanResult out;
    out.files_added = scan->files_added;
    out.files_updated = scan->files_updated;
    out.files_deleted = deleted;
    out.files_unchanged = scan->files_unchanged;
    out.files_skipped = scan->summary.files_skipped;

    VECTIS_LOG_INFO(
        "Scanner: incremental scan done — {} added, {} updated, {} deleted, {} unchanged",
        out.files_added, out.files_updated, out.files_deleted, out.files_unchanged);

    return out;
}

} // namespace vectis::code
