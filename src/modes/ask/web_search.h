#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace vectis::core { class ConfigManager; }
namespace vectis::platform { class HttpClient; }

namespace vectis::modes::ask {

/// One web search result.
struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

/// Parse a Brave Search API JSON response into results.
/// Exposed for testing without network access.
[[nodiscard]] vectis::core::Result<std::vector<WebSearchResult>>
parse_brave_response(const std::string& json_body);

/// Parse a DuckDuckGo instant answer API JSON response into results.
/// Exposed for testing without network access.
[[nodiscard]] vectis::core::Result<std::vector<WebSearchResult>>
parse_duckduckgo_response(const std::string& json_body, int max_results);

/// Web search abstraction with DuckDuckGo (zero-config) and Brave
/// (requires `BRAVE_API_KEY`) backends.
class WebSearch {
public:
    WebSearch(vectis::platform::HttpClient& http,
              vectis::core::ConfigManager&  config);

    /// Perform a web search.
    [[nodiscard]] vectis::core::Result<std::vector<WebSearchResult>>
    search(std::string_view query, int max_results = 5);

    /// Which backend is currently active.
    [[nodiscard]] std::string_view active_provider() const;

private:
    vectis::platform::HttpClient* m_http;
    vectis::core::ConfigManager*  m_config;
    std::string                   m_active_provider;

    [[nodiscard]] vectis::core::Result<std::vector<WebSearchResult>>
    search_brave(std::string_view query, int max_results);

    [[nodiscard]] vectis::core::Result<std::vector<WebSearchResult>>
    search_duckduckgo(std::string_view query, int max_results);
};

} // namespace vectis::modes::ask
