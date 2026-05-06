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

/// Count newlines once — cheaper than computing it again in CodeIndex.
[[nodiscard]] int count_lines(std::string_view content) noexcept
{
    int count = content.empty() ? 0 : 1;
    for (const char ch : content) {
        if (ch == '\n') {
            ++count;
        }
    }
    return count;
}

} // namespace

vectis::core::Result<ScanSummary> Scanner::run(const ScanConfig& config, CodeIndex& index,
                                               TreeSitterParser& parser,
                                               const ProgressCallback& on_progress,
                                               const CompletionCallback& on_complete,
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

    // Check for cancellation or a superseding epoch. Returns the
    // reason the scan should stop, or an empty string to continue.
    const auto preemption_reason = [&]() -> std::string_view {
        if (cancel_token.stop_requested()) {
            return "cancelled by token";
        }
        if (current_epoch.load(std::memory_order_acquire) != config.epoch) {
            return "pre-empted by epoch bump";
        }
        return {};
    };

    // Directory-name-only exclude check. Matches the design doc: the
    // user writes `code.exclude = ["node_modules", ".git"]` as names,
    // not path prefixes.
    const auto is_excluded_dir_name = [&](const std::filesystem::path& dir) {
        const std::string name = dir.filename().string();
        if (config.exclude_dir_names.find(name) != config.exclude_dir_names.end()) {
            return true;
        }
        return std::ranges::any_of(config.exclude_dir_globs, [&](const std::string& glob) {
            return wildcard_match(glob, name);
        });
    };

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
        if (const std::string_view reason = preemption_reason(); !reason.empty()) {
            VECTIS_LOG_INFO("Scanner: scan {}", reason);
            return make_error(vectis::core::ErrorKind::Cancelled, std::string{reason},
                              config.root.string());
        }

        const std::filesystem::directory_entry& entry = *it;

        // Directory-level skip: disable recursion for excluded subtrees.
        if (entry.is_directory(ec)) {
            if (is_excluded_dir_name(entry.path())) {
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
            // Vendored bundles (jquery-1.6.1.js, prototype.js, *.min.js)
            // dominate symbol counts and skew architecture detection
            // toward "JavaScript-heavy SPA" on backends that just ship
            // a help-system overlay. Counted as skipped, not Unknown,
            // so progress reporting still mentions them.
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

        // Refine the path-only language guess against the actual file
        // contents — handles `.h` files that turn out to be JS aliases
        // and similar legacy ambiguity.
        std::string ext = path.extension().string();
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const Language refined = refine_language(language, ext, content);

        FileEntry file_entry;
        file_entry.path_relative = std::filesystem::relative(path, config.root, ec);
        if (ec) {
            file_entry.path_relative = path;
            ec.clear();
        }
        file_entry.language = refined;
        file_entry.size = size;
        file_entry.line_count = count_lines(content);
        file_entry.content_hash = vectis::core::fnv1a_hex(content);
        const auto last_write = entry.last_write_time(ec);
        if (!ec) {
            file_entry.last_modified = last_write;
        }
        ec.clear();

        // Save the relative path before moving file_entry into the
        // index — we need it for the dependency resolver later.
        std::filesystem::path relative_path = file_entry.path_relative;
        const std::int64_t file_id = index.add_file(std::move(file_entry));

        auto parse_result = parser.parse_file(refined, content);
        if (!parse_result.symbols.empty()) {
            for (Symbol& sym : parse_result.symbols) {
                sym.file_id = file_id;
            }
            index.add_symbols(parse_result.symbols);
        }

        // Collect raw imports and namespace declarations for later
        // resolution. Both are no-ops for languages without the
        // corresponding query. A file with no imports but a namespace
        // declaration is still pushed — the resolver builds a
        // namespace → file-ids map across the whole batch, so a C#
        // `Models/User.cs` file that only declares `namespace
        // SampleApp.Models` (no `using`) must be visible to it.
        auto raw_imports = parser.extract_imports(refined, content);
        auto declared_ns = parser.extract_namespaces(refined, content);
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

    if (const std::string_view reason = preemption_reason(); !reason.empty()) {
        VECTIS_LOG_INFO("Scanner: scan {}", reason);
        return make_error(vectis::core::ErrorKind::Cancelled, std::string{reason},
                          config.root.string());
    }

    // Second pass — resolve raw imports into Dependency edges now
    // that the file table is stable. Only runs once per scan, so any
    // O(N*M) worst case inside resolve_all is bounded.
    if (!per_file_imports.empty()) {
        resolve_all(index, config.root, per_file_imports);
    }

    ScanSummary summary;
    summary.file_count = index.file_count();
    summary.symbol_count = index.symbol_count();
    summary.language_count = index.language_count();

    VECTIS_LOG_INFO(
        "Scanner: scan complete — {} files, {} symbols, {} languages, {} deps ({} skipped)",
        summary.file_count, summary.symbol_count, summary.language_count, index.dependency_count(),
        files_skipped);

    if (on_complete) {
        on_complete(summary);
    }
    return summary;
}

// ============================================================================
// Incremental scan
// ============================================================================

vectis::core::Result<IncrementalScanResult>
Scanner::run_incremental(const ScanConfig& config, CodeIndex& index, TreeSitterParser& parser,
                         const ProgressCallback& on_progress,
                         const vectis::core::CancellationToken& cancel_token,
                         const std::atomic<std::int64_t>& current_epoch)
{
    using vectis::core::ErrorKind;
    using vectis::core::make_error;

    VECTIS_LOG_INFO("Scanner: starting incremental scan of '{}'", config.root.string());

    // Build a map of existing files: relative_path → (file_id, content_hash).
    const auto existing_files = index.snapshot_files();
    std::unordered_map<std::string, std::pair<std::int64_t, std::string>> path_to_info;
    path_to_info.reserve(existing_files.size());
    for (const auto& f : existing_files) {
        path_to_info[f.path_relative.string()] = {f.id, f.content_hash};
    }

    // Track which existing paths were visited.
    std::unordered_set<std::string> visited_paths;
    visited_paths.reserve(existing_files.size());

    IncrementalScanResult result;
    std::size_t files_seen = 0;
    auto last_publish = std::chrono::steady_clock::now();

    // Imports collected for the dependency resolver pass.
    std::vector<FileImports> per_file_imports;

    const auto preemption_reason = [&]() -> std::string_view {
        if (cancel_token.stop_requested()) {
            return "cancelled by token";
        }
        if (current_epoch.load(std::memory_order_acquire) != config.epoch) {
            return "pre-empted by epoch bump";
        }
        return {};
    };

    const auto is_excluded_dir_name = [&](const std::filesystem::path& dir) {
        const std::string name = dir.filename().string();
        if (config.exclude_dir_names.find(name) != config.exclude_dir_names.end()) {
            return true;
        }
        return std::ranges::any_of(config.exclude_dir_globs, [&](const std::string& glob) {
            return wildcard_match(glob, name);
        });
    };

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
        if (const std::string_view reason = preemption_reason(); !reason.empty()) {
            VECTIS_LOG_INFO("Scanner: incremental scan {}", reason);
            return make_error(ErrorKind::Cancelled, std::string{reason}, config.root.string());
        }

        const std::filesystem::directory_entry& entry = *it;

        if (entry.is_directory(ec)) {
            if (is_excluded_dir_name(entry.path())) {
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
            // Skip vendored JS bundles in incremental scans for the
            // same reason as the full-scan path. The incremental
            // counters don't track "skipped" separately, so the file
            // simply does not get added or updated.
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
            try {
                it.increment(ec);
            }
            catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        // Same content-aware refinement as the full-scan path.
        std::string ext = path.extension().string();
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const Language refined = refine_language(language, ext, content);

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
        const std::string rel_str = rel.string();

        visited_paths.insert(rel_str);
        const auto new_hash = vectis::core::fnv1a_hex(content);

        const auto existing_it = path_to_info.find(rel_str);
        if (existing_it != path_to_info.end()) {
            if (existing_it->second.second == new_hash) {
                // Unchanged — skip. Existing dependency edges in the
                // index are already correct; no need to re-extract
                // imports or re-resolve.
                ++result.files_unchanged;
            }
            else {
                // Modified — remove old, re-add.
                index.remove_file(existing_it->second.first);

                FileEntry file_entry;
                file_entry.path_relative = rel;
                file_entry.language = refined;
                file_entry.size = size;
                file_entry.line_count = count_lines(content);
                file_entry.content_hash = new_hash;
                const auto last_write = entry.last_write_time(ec);
                if (!ec) {
                    file_entry.last_modified = last_write;
                }
                ec.clear();

                const std::int64_t file_id = index.add_file(std::move(file_entry));

                auto parse_result = parser.parse_file(refined, content);
                if (!parse_result.symbols.empty()) {
                    for (Symbol& sym : parse_result.symbols) {
                        sym.file_id = file_id;
                    }
                    index.add_symbols(parse_result.symbols);
                }

                auto raw_imports = parser.extract_imports(refined, content);
                auto declared_ns = parser.extract_namespaces(refined, content);
                if (!raw_imports.empty() || !declared_ns.empty()) {
                    FileImports fi;
                    fi.file_id = file_id;
                    fi.language = refined;
                    fi.relative_path = rel;
                    fi.imports = std::move(raw_imports);
                    fi.declared_namespaces = std::move(declared_ns);
                    per_file_imports.push_back(std::move(fi));
                }

                ++result.files_updated;
            }
        }
        else {
            // New file — add.
            FileEntry file_entry;
            file_entry.path_relative = rel;
            file_entry.language = refined;
            file_entry.size = size;
            file_entry.line_count = count_lines(content);
            file_entry.content_hash = new_hash;
            const auto last_write = entry.last_write_time(ec);
            if (!ec) {
                file_entry.last_modified = last_write;
            }
            ec.clear();

            const std::int64_t file_id = index.add_file(std::move(file_entry));

            auto parse_result = parser.parse_file(refined, content);
            if (!parse_result.symbols.empty()) {
                for (Symbol& sym : parse_result.symbols) {
                    sym.file_id = file_id;
                }
                index.add_symbols(parse_result.symbols);
            }

            auto raw_imports = parser.extract_imports(refined, content);
            auto declared_ns = parser.extract_namespaces(refined, content);
            if (!raw_imports.empty() || !declared_ns.empty()) {
                FileImports fi;
                fi.file_id = file_id;
                fi.language = refined;
                fi.relative_path = rel;
                fi.imports = std::move(raw_imports);
                fi.declared_namespaces = std::move(declared_ns);
                per_file_imports.push_back(std::move(fi));
            }

            ++result.files_added;
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

    // Detect deleted files.
    for (const auto& [path_str, info] : path_to_info) {
        if (visited_paths.find(path_str) == visited_paths.end()) {
            index.remove_file(info.first);
            ++result.files_deleted;
        }
    }

    // Re-resolve dependencies with the updated file table.
    if (!per_file_imports.empty()) {
        resolve_all(index, config.root, per_file_imports);
    }

    // Reclaim soft-delete tombstones once per scan — long-running
    // incremental sessions would otherwise let the underlying
    // vectors grow without bound.
    if (result.files_deleted > 0 || result.files_updated > 0) {
        index.compact();
    }

    VECTIS_LOG_INFO(
        "Scanner: incremental scan done — {} added, {} updated, {} deleted, {} unchanged",
        result.files_added, result.files_updated, result.files_deleted, result.files_unchanged);

    return result;
}

} // namespace vectis::code
