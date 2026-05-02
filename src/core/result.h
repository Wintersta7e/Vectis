#pragma once

#include <cstdint>
#include <source_location>
#include <string>
#include <utility>

#include <tl/expected.hpp>

namespace vectis::core {

/// Categories of errors that can flow through the Result<T> pipeline.
/// Each value corresponds to a failure domain; the `Error::message` field
/// carries the human-readable detail.
enum class ErrorKind : std::uint8_t {
    IoError,
    ParseError,
    NetworkError,
    StorageError,
    AIError,
    ConfigError,
    PlatformError,
    /// Operation was cancelled cooperatively (e.g. a scan interrupted
    /// by the user). Not a failure in the "something went wrong"
    /// sense — the caller asked for it to stop. Still modeled as an
    /// Error so it flows through the Result<T> pipeline naturally.
    Cancelled,
};

/// Rich error value propagated via `Result<T>`.
/// Includes a source-location for diagnostics; prefer constructing via
/// `make_error()` so the location is captured at the call site.
struct Error {
    ErrorKind            kind{};
    std::string          message;
    std::string          context;
    std::source_location location;
};

/// Vectis project-wide Result type.
///
/// Built on `tl::expected` for compatibility with C++20 (the standard
/// `std::expected` requires C++23). The alias intentionally matches the
/// `std::expected<T, E>` API surface so a future migration is a header swap.
template <typename T>
using Result = tl::expected<T, Error>;

/// Construct an unexpected `Error` suitable for returning from a
/// `Result<T>`-returning function. Source location is captured at the
/// call site by default.
///
/// Example:
///     return make_error(ErrorKind::ConfigError, "parse failed", path.string());
[[nodiscard]] inline tl::unexpected<Error> make_error(
    ErrorKind            kind,
    std::string          message,
    std::string          context  = {},
    std::source_location location = std::source_location::current())
{
    return tl::unexpected<Error>(Error{kind, std::move(message), std::move(context), location});
}

/// Convert an `ErrorKind` to a short, stable string suitable for logs.
[[nodiscard]] constexpr std::string_view error_kind_to_string(ErrorKind kind) noexcept
{
    switch (kind) {
        case ErrorKind::IoError:       return "IoError";
        case ErrorKind::ParseError:    return "ParseError";
        case ErrorKind::NetworkError:  return "NetworkError";
        case ErrorKind::StorageError:  return "StorageError";
        case ErrorKind::AIError:       return "AIError";
        case ErrorKind::ConfigError:   return "ConfigError";
        case ErrorKind::PlatformError: return "PlatformError";
        case ErrorKind::Cancelled:     return "Cancelled";
    }
    return "UnknownError";
}

} // namespace vectis::core
