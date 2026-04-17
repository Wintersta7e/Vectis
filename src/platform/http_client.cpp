#include "platform/http_client.h"

#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>

#include "core/log.h"
#include "core/result.h"

namespace vectis::platform {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

// ============================================================================
// Global curl init (once)
// ============================================================================

namespace {

std::once_flag g_curl_init_flag;

void ensure_curl_init()
{
    std::call_once(g_curl_init_flag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

/// State passed to the write callback for body size enforcement.
struct WriteState {
    std::string* body       = nullptr;
    std::size_t  max_bytes  = 0;
};

// libcurl write callback — appends received data to a std::string.
// Returns 0 (abort) if the accumulated body exceeds the configured cap.
std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb,
                           void* userdata)
{
    const std::size_t total = size * nmemb;
    auto* state = static_cast<WriteState*>(userdata);
    if (state->max_bytes > 0 &&
        state->body->size() + total > state->max_bytes)
    {
        return 0; // signals abort to libcurl
    }
    state->body->append(ptr, total);
    return total;
}

/// State passed to the streaming write callback. Unlike `WriteState`,
/// no buffer is accumulated — each chunk is handed to the user
/// callback and dropped.
struct StreamWriteState {
    const HttpChunkCallback*  on_chunk    = nullptr;
    const std::atomic<bool>*  cancel_flag = nullptr;
};

// libcurl streaming write callback — invokes the user callback once per
// chunk. Returns 0 to abort libcurl when (a) cancellation was requested,
// (b) the user callback returned false, or (c) the callback is missing.
std::size_t stream_write_callback(char* ptr, std::size_t size,
                                  std::size_t nmemb, void* userdata)
{
    const std::size_t total = size * nmemb;
    auto* state = static_cast<StreamWriteState*>(userdata);

    if (state == nullptr || state->on_chunk == nullptr || !*state->on_chunk) {
        return 0;
    }
    if (state->cancel_flag != nullptr &&
        state->cancel_flag->load(std::memory_order_acquire))
    {
        return 0;
    }

    const bool keep_going = (*state->on_chunk)(std::string_view(ptr, total));
    return keep_going ? total : 0;
}

/// libcurl progress callback — fires periodically (~every few hundred ms)
/// regardless of whether data has arrived. Lets `cancel_stream()` abort a
/// response that is stalled between tokens. Returning nonzero aborts with
/// CURLE_ABORTED_BY_CALLBACK.
int stream_progress_callback(void* userdata,
                             curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
    auto* state = static_cast<StreamWriteState*>(userdata);
    if (state != nullptr && state->cancel_flag != nullptr &&
        state->cancel_flag->load(std::memory_order_acquire))
    {
        return 1;
    }
    return 0;
}

// libcurl header callback — parses response headers into a map.
std::size_t header_callback(char* buffer, std::size_t size, std::size_t nitems,
                            void* userdata)
{
    const std::size_t total = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    std::string_view line(buffer, total);

    // Trim trailing \r\n.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }

    // Skip the status line (e.g. "HTTP/1.1 200 OK") and empty lines.
    if (line.empty() || line.substr(0, 5) == "HTTP/") {
        return total;
    }

    // Split on first ':'.
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return total;
    }

    std::string key{line.substr(0, colon)};
    std::string_view val = line.substr(colon + 1);

    // Trim leading whitespace from value.
    while (!val.empty() && val.front() == ' ') {
        val.remove_prefix(1);
    }

    // Lowercase the key for case-insensitive lookup.
    for (char& ch : key) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    (*headers)[std::move(key)] = std::string(val);
    return total;
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct HttpClient::Impl {
    CURL* handle = nullptr;
};

// ============================================================================
// Lifecycle
// ============================================================================

HttpClient::HttpClient() : m_impl(std::make_unique<Impl>())
{
    ensure_curl_init();
    m_impl->handle = curl_easy_init();
    if (m_impl->handle == nullptr) {
        VECTIS_LOG_ERROR("HttpClient: curl_easy_init() returned null");
    }
}

HttpClient::~HttpClient()
{
    if (m_impl && m_impl->handle != nullptr) {
        curl_easy_cleanup(m_impl->handle);
    }
}

HttpClient::HttpClient(HttpClient&&) noexcept            = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

// ============================================================================
// send
// ============================================================================

Result<HttpResponse> HttpClient::send(const HttpRequest& req)
{
    if (m_impl->handle == nullptr) {
        return make_error(ErrorKind::NetworkError, "curl handle not initialized");
    }

    CURL* h = m_impl->handle;
    curl_easy_reset(h);

    // URL
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());

    // Method
    if (req.method == "POST") {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body.size()));
    } else if (req.method == "PUT") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body.size()));
    } else if (req.method == "DELETE") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    // GET is the default

    // Timeout
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));

    // TLS verification — always enforce, even on minimal systems.
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);

    // Follow redirects
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);

    // Headers — check each append for OOM.
    curl_slist* header_list = nullptr;
    for (const auto& [key, value] : req.headers) {
        const std::string header_line = key + ": " + value;
        curl_slist* appended = curl_slist_append(header_list, header_line.c_str());
        if (appended == nullptr) {
            curl_slist_free_all(header_list);
            return make_error(ErrorKind::NetworkError,
                              "curl_slist_append allocation failed");
        }
        header_list = appended;
    }
    if (header_list != nullptr) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, header_list);
    }

    // Response body with size cap.
    std::string response_body;
    WriteState write_state{&response_body, req.max_body_bytes};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &write_state);

    // Response headers
    std::map<std::string, std::string> response_headers;
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &response_headers);

    // Perform
    const CURLcode res = curl_easy_perform(h);

    // Clean up request headers.
    if (header_list != nullptr) {
        curl_slist_free_all(header_list);
    }

    if (res != CURLE_OK) {
        return make_error(ErrorKind::NetworkError,
                          curl_easy_strerror(res), req.url);
    }

    HttpResponse response;
    response.body    = std::move(response_body);
    response.headers = std::move(response_headers);

    long status_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status_code);
    response.status_code = static_cast<int>(status_code);

    double total_time = 0.0;
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total_time);
    response.total_time_ms = total_time * 1000.0;

    return response;
}

// ============================================================================
// send_streaming
// ============================================================================

Result<HttpResponse> HttpClient::send_streaming(const HttpStreamRequest& req)
{
    if (m_impl->handle == nullptr) {
        return make_error(ErrorKind::NetworkError, "curl handle not initialized");
    }
    if (!req.on_chunk) {
        return make_error(ErrorKind::NetworkError,
                          "send_streaming requires a non-empty on_chunk callback");
    }

    CURL* h = m_impl->handle;
    curl_easy_reset(h);

    // URL
    curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());

    // Method (matches the regular send() semantics).
    if (req.method == "POST") {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body.size()));
    } else if (req.method == "PUT") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(req.body.size()));
    } else if (req.method == "DELETE") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    // Timeout — streaming requests typically need a generous budget,
    // but we still honor the request's explicit value to let callers
    // cap long-running streams.
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(req.timeout_ms));

    // TLS + redirects (mirrors send()).
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);

    // Headers.
    curl_slist* header_list = nullptr;
    for (const auto& [key, value] : req.headers) {
        const std::string header_line = key + ": " + value;
        curl_slist* appended = curl_slist_append(header_list, header_line.c_str());
        if (appended == nullptr) {
            curl_slist_free_all(header_list);
            return make_error(ErrorKind::NetworkError,
                              "curl_slist_append allocation failed");
        }
        header_list = appended;
    }
    if (header_list != nullptr) {
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, header_list);
    }

    // Streaming write callback — drops each chunk into the user callback.
    StreamWriteState write_state{&req.on_chunk, req.cancel_flag};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &write_state);

    // Progress callback — lets cancel_stream() interrupt a stalled
    // response that is waiting between tokens (no bytes → no write
    // callback firing). CURLOPT_NOPROGRESS must be off for the
    // progress callback to be invoked.
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, stream_progress_callback);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA,     &write_state);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS,       0L);

    // Response headers (still collected — useful for status / content type).
    std::map<std::string, std::string> response_headers;
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &response_headers);

    const CURLcode res = curl_easy_perform(h);

    if (header_list != nullptr) {
        curl_slist_free_all(header_list);
    }

    // Distinguish cancellation from real network errors. Two different
    // libcurl codes map to "caller asked us to stop":
    //   - WRITE_ERROR: write callback returned 0 mid-chunk.
    //   - ABORTED_BY_CALLBACK: progress callback returned nonzero.
    if (res == CURLE_WRITE_ERROR || res == CURLE_ABORTED_BY_CALLBACK) {
        const bool cancelled =
            req.cancel_flag != nullptr &&
            req.cancel_flag->load(std::memory_order_acquire);
        if (cancelled) {
            return make_error(ErrorKind::Cancelled,
                              "streaming request cancelled by caller");
        }
    }

    if (res != CURLE_OK) {
        return make_error(ErrorKind::NetworkError,
                          curl_easy_strerror(res), req.url);
    }

    HttpResponse response;
    response.headers = std::move(response_headers);

    long status_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status_code);
    response.status_code = static_cast<int>(status_code);

    double total_time = 0.0;
    curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total_time);
    response.total_time_ms = total_time * 1000.0;

    return response;
}

// ============================================================================
// url_encode
// ============================================================================

std::string url_encode(std::string_view input)
{
    std::string result;
    result.reserve(input.size() * 3); // worst case

    for (const auto ch : input) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(ch);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", uch);
            result.append(buf);
        }
    }
    return result;
}

} // namespace vectis::platform
