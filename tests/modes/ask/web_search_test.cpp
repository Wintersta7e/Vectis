#include "modes/ask/web_search.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "platform/http_client.h"

namespace {

using vectis::modes::ask::parse_brave_response;
using vectis::modes::ask::parse_duckduckgo_response;
using vectis::platform::url_encode;

// ---- Brave JSON parsing ----------------------------------------------------

TEST(WebSearchTest, ParseBraveResponse_ValidJson)
{
    const std::string json = R"json({
        "web": {
            "results": [
                {
                    "title": "C++ Reference",
                    "url": "https://en.cppreference.com/",
                    "description": "C++ language reference documentation"
                },
                {
                    "title": "Learn C++",
                    "url": "https://learncpp.com/",
                    "description": "Free tutorials for learning C++"
                }
            ]
        }
    })json";

    auto result = parse_brave_response(json);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ((*result)[0].title, "C++ Reference");
    EXPECT_EQ((*result)[0].url, "https://en.cppreference.com/");
    EXPECT_EQ((*result)[0].snippet, "C++ language reference documentation");
    EXPECT_EQ((*result)[1].title, "Learn C++");
}

TEST(WebSearchTest, ParseBraveResponse_EmptyResults)
{
    auto result = parse_brave_response(R"json({"web": {"results": []}})json");
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->empty());
}

TEST(WebSearchTest, ParseBraveResponse_NoWebKey)
{
    auto result = parse_brave_response(R"json({"query": {"original": "test"}})json");
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->empty());
}

TEST(WebSearchTest, ParseBraveResponse_InvalidJson)
{
    auto result = parse_brave_response("not json at all");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().kind, vectis::core::ErrorKind::ParseError);
}

// ---- DuckDuckGo JSON parsing -----------------------------------------------

TEST(WebSearchTest, ParseDuckDuckGoResponse_WithAbstract)
{
    const std::string json = R"json({
        "Heading": "C++ (programming language)",
        "AbstractText": "C++ is a general-purpose programming language.",
        "AbstractURL": "https://en.wikipedia.org/wiki/Cpp",
        "RelatedTopics": [
            {
                "Text": "Standard Template Library - A collection of templates",
                "FirstURL": "https://en.wikipedia.org/wiki/STL"
            }
        ]
    })json";

    auto result = parse_duckduckgo_response(json, 5);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_GE(result->size(), 1U);
    EXPECT_EQ((*result)[0].title, "C++ (programming language)");
    EXPECT_EQ((*result)[0].snippet, "C++ is a general-purpose programming language.");
}

TEST(WebSearchTest, ParseDuckDuckGoResponse_EmptyAbstract)
{
    const std::string json = R"json({
        "Heading": "",
        "AbstractText": "",
        "AbstractURL": "",
        "RelatedTopics": [
            {
                "Text": "CMake - A cross-platform build system",
                "FirstURL": "https://cmake.org/"
            }
        ]
    })json";

    auto result = parse_duckduckgo_response(json, 5);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ((*result)[0].url, "https://cmake.org/");
}

TEST(WebSearchTest, ParseDuckDuckGoResponse_InvalidJson)
{
    auto result = parse_duckduckgo_response("{broken", 5);
    EXPECT_FALSE(result);
}

} // namespace
