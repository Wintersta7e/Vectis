#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "core/result.h"

namespace vectis::platform {

/// HTTP request descriptor.
struct HttpRequest {
    std::string                        method = "GET";
    std::string                        url;
    std::map<std::string, std::string> headers;
    std::string                        body;
    int                                timeout_ms = 30000;
    std::size_t                        max_body_bytes = 32ULL * 1024 * 1024; // 32 MB
};

/// HTTP response returned from HttpClient::send().
struct HttpResponse {
    int                                status_code = 0;
    std::map<std::string, std::string> headers;
    std::string                        body;
    double                             total_time_ms = 0.0;
};

/// Per-chunk callback invoked by `HttpClient::send_streaming()`. Return
/// `false` to abort the transfer (libcurl will report an abort error on
/// return). Called synchronously on the thread that invoked send_streaming.
using HttpChunkCallback = std::function<bool(std::string_view chunk)>;

/// Streaming request descriptor. Inherits the base `HttpRequest` fields
/// and adds a chunk callback plus optional cancellation flag.
struct HttpStreamRequest : HttpRequest {
    HttpChunkCallback  on_chunk;
    std::atomic<bool>* cancel_flag = nullptr; ///< optional; polled per chunk
};

/// Synchronous HTTP client backed by libcurl.
///
/// Each instance owns a `CURL*` handle. Instances are not thread-safe
/// but multiple instances can be used concurrently from different threads.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    /// Send an HTTP request and return the response.
    [[nodiscard]] vectis::core::Result<HttpResponse> send(const HttpRequest& req);

    /// Send a streaming HTTP request. Each received byte range is handed
    /// to `req.on_chunk` as it arrives. The returned `HttpResponse` has
    /// an empty `body` (already delivered to the callback); only
    /// `status_code`, `headers`, and `total_time_ms` are populated.
    [[nodiscard]] vectis::core::Result<HttpResponse>
    send_streaming(const HttpStreamRequest& req);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Percent-encode a string for use in URLs.
[[nodiscard]] std::string url_encode(std::string_view input);

} // namespace vectis::platform
