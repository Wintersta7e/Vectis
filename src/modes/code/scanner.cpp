#include "modes/code/scanner.h"

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

#include "core/log.h"
#include "modes/code/code_index.h"
#include "modes/code/dependency_resolver.h"
#include "modes/code/language.h"
#include "modes/code/parser.h"
#include "modes/code/symbol.h"
#include "platform/file_io.h"

namespace vectis::modes::code {

namespace {

/// Skip files larger than 2 MB — they're typically minified bundles,
/// generated artifacts, or otherwise unhelpful for symbol extraction.
constexpr std::uint64_t k_max_file_size_bytes = 2ULL * 1024 * 1024;

/// Publish progress at most once every 50 files OR every 100 ms,
/// whichever fires first. Scan loops that publish per file dominate
/// their own runtime on large codebases due to bus mutex contention.
constexpr std::size_t               k_progress_file_stride = 50;
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

bool Scanner::run(const ScanConfig&                           config,
                  CodeIndex&                                  index,
                  TreeSitterParser&                           parser,
                  const ProgressCallback&                     on_progress,
                  const CompletionCallback&                   on_complete,
                  const vectis::core::CancellationToken&      cancel_token,
                  const std::atomic<std::int64_t>&            current_epoch)
{
    VECTIS_LOG_INFO("Scanner: starting scan of '{}'", config.root.string());

    std::size_t       files_seen     = 0;
    std::uint64_t     files_skipped  = 0;
    auto              last_publish   = std::chrono::steady_clock::now();

    // Per-file raw imports collected during the scan. Resolution is
    // deferred to `resolve_all` after the main loop exits so the
    // resolver sees the complete file table.
    std::vector<FileImports> per_file_imports;

    const auto bail_if_preempted = [&]() -> bool {
        if (cancel_token.stop_requested()) {
            VECTIS_LOG_INFO("Scanner: scan cancelled by token");
            return true;
        }
        if (current_epoch.load(std::memory_order_acquire) != config.epoch) {
            VECTIS_LOG_INFO("Scanner: scan pre-empted by epoch bump");
            return true;
        }
        return false;
    };

    // Directory-name-only exclude check. Matches the design doc: the
    // user writes `code.exclude = ["node_modules", ".git"]` as names,
    // not path prefixes.
    const auto is_excluded_dir_name = [&](const std::filesystem::path& dir) {
        return config.exclude_dir_names.find(dir.filename().string()) !=
               config.exclude_dir_names.end();
    };

    std::error_code ec;
    if (!std::filesystem::is_directory(config.root, ec) || ec) {
        VECTIS_LOG_ERROR(
            "Scanner: root '{}' is not a directory: {}",
            config.root.string(),
            ec.message());
        return false;
    }

    using Iter = std::filesystem::recursive_directory_iterator;
    const auto options = std::filesystem::directory_options::skip_permission_denied;

    Iter it{};
    try {
        it = Iter{config.root, options, ec};
    } catch (const std::exception& e) {
        VECTIS_LOG_ERROR("Scanner: failed to open recursive iterator: {}", e.what());
        return false;
    }
    if (ec) {
        VECTIS_LOG_ERROR("Scanner: recursive_directory_iterator init failed: {}", ec.message());
        return false;
    }

    const Iter end_it{};
    for (; it != end_it; ) {
        if (bail_if_preempted()) {
            return false;
        }

        const std::filesystem::directory_entry& entry = *it;

        // Directory-level skip: disable recursion for excluded subtrees.
        if (entry.is_directory(ec)) {
            if (is_excluded_dir_name(entry.path())) {
                it.disable_recursion_pending();
            }
            try {
                it.increment(ec);
            } catch (const std::exception& e) {
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
            } catch (const std::exception& e) {
                VECTIS_LOG_WARN("Scanner: non-file increment threw: {}", e.what());
                ec.clear();
                break;
            }
            continue;
        }

        const std::filesystem::path& path     = entry.path();
        const Language               language = detect_language(path);
        if (language == Language::Unknown) {
            ++files_skipped;
            try {
                it.increment(ec);
            } catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        const std::uint64_t size = entry.file_size(ec);
        if (ec || size > k_max_file_size_bytes) {
            if (size > k_max_file_size_bytes) {
                VECTIS_LOG_DEBUG(
                    "Scanner: skipping large file '{}' ({} bytes)",
                    path.string(), size);
            }
            ec.clear();
            ++files_skipped;
            try {
                it.increment(ec);
            } catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        auto read_result = vectis::platform::read_file(path);
        if (!read_result) {
            VECTIS_LOG_DEBUG(
                "Scanner: read_file failed for '{}': {}",
                path.string(),
                read_result.error().message);
            ++files_skipped;
            try {
                it.increment(ec);
            } catch (...) {
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
            } catch (...) {
                ec.clear();
                break;
            }
            continue;
        }

        FileEntry file_entry;
        file_entry.path_relative = std::filesystem::relative(path, config.root, ec);
        if (ec) {
            file_entry.path_relative = path;
            ec.clear();
        }
        file_entry.language   = language;
        file_entry.size       = size;
        file_entry.line_count = count_lines(content);
        const auto last_write = entry.last_write_time(ec);
        if (!ec) {
            file_entry.last_modified = last_write;
        }
        ec.clear();

        // Save the relative path before moving file_entry into the
        // index — we need it for the dependency resolver later.
        std::filesystem::path relative_path = file_entry.path_relative;
        const std::int64_t file_id = index.add_file(std::move(file_entry));

        auto parse_result = parser.parse_file(language, content);
        if (!parse_result.symbols.empty()) {
            for (Symbol& sym : parse_result.symbols) {
                sym.file_id = file_id;
            }
            index.add_symbols(parse_result.symbols);
        }

        // Collect raw imports for later resolution — only if the
        // language has an import query wired up. Skipped languages
        // return an empty vector so no cost to call unconditionally.
        auto raw_imports = parser.extract_imports(language, content);
        if (!raw_imports.empty()) {
            FileImports fi;
            fi.file_id       = file_id;
            fi.language      = language;
            fi.relative_path = std::move(relative_path);
            fi.imports       = std::move(raw_imports);
            per_file_imports.push_back(std::move(fi));
        }

        ++files_seen;

        // Throttled progress publication — see k_progress_* constants.
        const auto now = std::chrono::steady_clock::now();
        if (files_seen % k_progress_file_stride == 0 ||
            (now - last_publish) >= k_progress_time_stride)
        {
            ScanProgress progress;
            progress.files_scanned = files_seen;
            progress.files_skipped = files_skipped;
            progress.current_path  = std::filesystem::relative(path, config.root, ec).string();
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
        } catch (const std::exception& e) {
            VECTIS_LOG_WARN("Scanner: file increment threw: {}", e.what());
            ec.clear();
            break;
        }
    }

    if (bail_if_preempted()) {
        return false;
    }

    // Second pass — resolve raw imports into Dependency edges now
    // that the file table is stable. Only runs once per scan, so any
    // O(N*M) worst case inside resolve_all is bounded.
    if (!per_file_imports.empty()) {
        resolve_all(index, config.root, per_file_imports);
    }

    ScanSummary summary;
    summary.file_count     = index.file_count();
    summary.symbol_count   = index.symbol_count();
    summary.language_count = index.language_count();

    VECTIS_LOG_INFO(
        "Scanner: scan complete — {} files, {} symbols, {} languages, {} deps ({} skipped)",
        summary.file_count, summary.symbol_count, summary.language_count,
        index.dependency_count(), files_skipped);

    if (on_complete) {
        on_complete(summary);
    }
    return true;
}

} // namespace vectis::modes::code
