#include "platform/http_client.h"

#include <string>

#include <gtest/gtest.h>

namespace {

using vectis::platform::HttpClient;
using vectis::platform::HttpRequest;
using vectis::platform::url_encode;

// ---- url_encode tests ------------------------------------------------------

TEST(HttpClientTest, UrlEncode_PassthroughAlphanumeric)
{
    EXPECT_EQ(url_encode("hello123"), "hello123");
}

TEST(HttpClientTest, UrlEncode_EncodesSpaces)
{
    EXPECT_EQ(url_encode("hello world"), "hello%20world");
}

TEST(HttpClientTest, UrlEncode_EncodesSpecialChars)
{
    EXPECT_EQ(url_encode("a+b=c&d"), "a%2Bb%3Dc%26d");
}

TEST(HttpClientTest, UrlEncode_PreservesUnreserved)
{
    EXPECT_EQ(url_encode("a-b_c.d~e"), "a-b_c.d~e");
}

// ---- HttpClient lifecycle --------------------------------------------------

TEST(HttpClientTest, ConstructAndDestroy)
{
    HttpClient client;
    // Should not crash.
}

TEST(HttpClientTest, InvalidUrl_ReturnsNetworkError)
{
    HttpClient client;
    HttpRequest req;
    req.url        = "http://invalid.test.example.invalid/no-such-host";
    req.timeout_ms = 3000;

    auto result = client.send(req);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, vectis::core::ErrorKind::NetworkError);
}

} // namespace
