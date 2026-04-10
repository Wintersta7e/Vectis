#include "modes/ask/web_search.h"

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config_manager.h"
#include "core/log.h"
#include "core/result.h"
#include "platform/http_client.h"

namespace vectis::modes::ask {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;

// ============================================================================
// JSON response parsers (free functions, testable without network)
// ============================================================================

Result<std::vector<WebSearchResult>>
parse_brave_response(const std::string& json_body)
{
    std::vector<WebSearchResult> results;
    try {
        auto root = nlohmann::json::parse(json_body);

        if (!root.contains("web") || !root["web"].contains("results")) {
            return results; // no web results — not an error
        }

        for (const auto& item : root["web"]["results"]) {
            WebSearchResult r;
            r.title   = item.value("title", "");
            r.url     = item.value("url", "");
            r.snippet = item.value("description", "");
            if (!r.url.empty()) {
                results.push_back(std::move(r));
            }
        }
    } catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("Brave JSON parse failed: ") + e.what());
    }
    return results;
}

Result<std::vector<WebSearchResult>>
parse_duckduckgo_response(const std::string& json_body, int max_results)
{
    std::vector<WebSearchResult> results;
    try {
        auto root = nlohmann::json::parse(json_body);

        // Abstract (primary answer)
        const auto abstract_text = root.value("AbstractText", "");
        const auto abstract_url  = root.value("AbstractURL", "");
        if (!abstract_text.empty()) {
            WebSearchResult r;
            r.title   = root.value("Heading", "Answer");
            r.url     = abstract_url;
            r.snippet = abstract_text;
            results.push_back(std::move(r));
        }

        // Related topics
        if (root.contains("RelatedTopics") && root["RelatedTopics"].is_array()) {
            for (const auto& topic : root["RelatedTopics"]) {
                if (static_cast<int>(results.size()) >= max_results) break;

                // Topics can be objects with Text/FirstURL or sub-groups.
                if (topic.contains("Text") && topic.contains("FirstURL")) {
                    WebSearchResult r;
                    r.snippet = topic.value("Text", "");
                    r.url     = topic.value("FirstURL", "");
                    // Extract title from the snippet (first sentence or bold text).
                    const auto dash = r.snippet.find(" - ");
                    r.title = (dash != std::string::npos)
                        ? r.snippet.substr(0, dash)
                        : r.snippet.substr(0, 60);
                    if (!r.url.empty()) {
                        results.push_back(std::move(r));
                    }
                }
            }
        }
    } catch (const nlohmann::json::exception& e) {
        return make_error(ErrorKind::ParseError,
                          std::string("DuckDuckGo JSON parse failed: ") + e.what());
    }
    return results;
}

// ============================================================================
// WebSearch
// ============================================================================

WebSearch::WebSearch(vectis::platform::HttpClient& http,
                     vectis::core::ConfigManager&  config)
    : m_http(&http), m_config(&config)
{
    // Determine default provider.
    auto key = m_config->get_env("BRAVE_API_KEY");
    m_active_provider = (key.has_value() && !key->empty()) ? "brave" : "duckduckgo";
    VECTIS_LOG_INFO("WebSearch: active provider = {}", m_active_provider);
}

Result<std::vector<WebSearchResult>>
WebSearch::search(std::string_view query, int max_results)
{
    if (query.empty()) {
        return std::vector<WebSearchResult>{};
    }

    if (m_active_provider == "brave") {
        return search_brave(query, max_results);
    }
    return search_duckduckgo(query, max_results);
}

std::string_view WebSearch::active_provider() const
{
    return m_active_provider;
}

// ============================================================================
// Brave Search
// ============================================================================

Result<std::vector<WebSearchResult>>
WebSearch::search_brave(std::string_view query, int max_results)
{
    auto key = m_config->get_env("BRAVE_API_KEY");
    if (!key || key->empty()) {
        return make_error(ErrorKind::ConfigError, "BRAVE_API_KEY not set");
    }

    const std::string encoded_query = vectis::platform::url_encode(query);
    const std::string url =
        "https://api.search.brave.com/res/v1/web/search?q=" + encoded_query +
        "&count=" + std::to_string(max_results);

    vectis::platform::HttpRequest req;
    req.method     = "GET";
    req.url        = url;
    req.timeout_ms = 10000;
    req.headers["Accept"]               = "application/json";
    req.headers["X-Subscription-Token"] = *key;

    auto resp = m_http->send(req);
    if (!resp) {
        return tl::unexpected(resp.error());
    }

    if (resp->status_code != 200) {
        return make_error(ErrorKind::NetworkError,
                          "Brave API returned status " + std::to_string(resp->status_code));
    }

    return parse_brave_response(resp->body);
}

// ============================================================================
// DuckDuckGo
// ============================================================================

Result<std::vector<WebSearchResult>>
WebSearch::search_duckduckgo(std::string_view query, int max_results)
{
    const std::string encoded_query = vectis::platform::url_encode(query);
    const std::string url =
        "https://api.duckduckgo.com/?q=" + encoded_query +
        "&format=json&no_redirect=1&no_html=1";

    vectis::platform::HttpRequest req;
    req.method     = "GET";
    req.url        = url;
    req.timeout_ms = 10000;

    auto resp = m_http->send(req);
    if (!resp) {
        return tl::unexpected(resp.error());
    }

    if (resp->status_code != 200) {
        return make_error(ErrorKind::NetworkError,
                          "DuckDuckGo API returned status " + std::to_string(resp->status_code));
    }

    return parse_duckduckgo_response(resp->body, max_results);
}

} // namespace vectis::modes::ask
