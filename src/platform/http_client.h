#pragma once

#include <cstdint>
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

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Percent-encode a string for use in URLs.
[[nodiscard]] std::string url_encode(std::string_view input);

} // namespace vectis::platform
