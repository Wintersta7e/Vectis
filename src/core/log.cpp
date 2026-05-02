#include "core/log.h"

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace vectis::core::log {

namespace {

constexpr const char* k_logger_name = "vectis";
constexpr const char* k_log_file_name = "vectis.log";
constexpr const char* k_logs_subdir = "logs";
constexpr std::size_t k_max_file_bytes = std::size_t{5} * 1024 * 1024;
constexpr std::size_t k_max_rotated_files = 3;
constexpr const char* k_log_level_env = "VECTIS_LOG_LEVEL";
constexpr const char* k_log_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v";

/// The single shared logger, accessed through a Meyers-singleton accessor.
/// Held as a `shared_ptr` into spdlog's registry so we can null it out
/// during shutdown without racing with spdlog internals. Wrapping it in
/// a function-local static instead of a translation-unit global keeps
/// the storage non-global at the language level (cppcoreguidelines) and
/// gives well-defined initialization order.
[[nodiscard]] std::shared_ptr<spdlog::logger>& logger_slot() noexcept
{
    static std::shared_ptr<spdlog::logger> slot;
    return slot;
}

[[nodiscard]] spdlog::level::level_enum level_from_env() noexcept
{
    // std::getenv is thread-unsafe on POSIX but log::init runs during
    // single-threaded startup, so this is safe by construction.
    const char* raw = std::getenv(k_log_level_env); // NOLINT(concurrency-mt-unsafe)
    if (raw == nullptr) {
        return spdlog::level::info;
    }

    const std::string_view value{raw};
    if (value == "trace") {
        return spdlog::level::trace;
    }
    if (value == "debug") {
        return spdlog::level::debug;
    }
    if (value == "info") {
        return spdlog::level::info;
    }
    if (value == "warn") {
        return spdlog::level::warn;
    }
    if (value == "warning") {
        return spdlog::level::warn;
    }
    if (value == "error") {
        return spdlog::level::err;
    }
    if (value == "err") {
        return spdlog::level::err;
    }
    if (value == "critical") {
        return spdlog::level::critical;
    }
    if (value == "off") {
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

Result<void> init(const std::filesystem::path& data_dir)
{
    auto& slot = logger_slot();
    if (slot) {
        // Already initialized — treat as idempotent success.
        return {};
    }

    const std::filesystem::path logs_dir = data_dir / k_logs_subdir;

    std::error_code ec;
    std::filesystem::create_directories(logs_dir, ec);
    if (ec) {
        return make_error(ErrorKind::IoError, "failed to create logs directory: " + ec.message(),
                          logs_dir.string());
    }

    const std::filesystem::path log_file = logs_dir / k_log_file_name;

    try {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file.string(), k_max_file_bytes, k_max_rotated_files);
        auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks{file_sink, stderr_sink};
        auto logger = std::make_shared<spdlog::logger>(k_logger_name, sinks.begin(), sinks.end());

        logger->set_level(level_from_env());
        logger->flush_on(spdlog::level::warn);
        logger->set_pattern(k_log_pattern);

        spdlog::register_logger(logger);
        slot = std::move(logger);
    }
    catch (const std::exception& e) {
        return make_error(ErrorKind::IoError,
                          std::string{"failed to create log sinks: "} + e.what(),
                          log_file.string());
    }

    return {};
}

void shutdown() noexcept
{
    auto& slot = logger_slot();
    if (!slot) {
        return;
    }
    try {
        slot->flush();
        spdlog::drop(k_logger_name);
    }
    catch (...) { // NOLINT(bugprone-empty-catch)
        // Intentional swallow: shutdown is noexcept and must not propagate
        // exceptions during program teardown. Any spdlog flush/drop failure
        // here is unrecoverable; we still reset the slot below so subsequent
        // log calls become no-ops via `get()` returning nullptr.
    }
    slot.reset();
}

spdlog::logger* get() noexcept
{
    return logger_slot().get();
}

} // namespace vectis::core::log
