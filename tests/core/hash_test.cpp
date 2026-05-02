#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/hash.h"

namespace {

using vectis::core::fnv1a_hex;

TEST(HashTest, EmptyString)
{
    const auto hash = fnv1a_hex("");
    EXPECT_EQ(hash.size(), 16U);
    // FNV-1a of empty input equals the offset basis.
    EXPECT_EQ(hash, "cbf29ce484222325");
}

TEST(HashTest, KnownVector_Hello)
{
    // "hello" has a well-known FNV-1a 64 hash.
    const auto hash = fnv1a_hex("hello");
    EXPECT_EQ(hash.size(), 16U);
    EXPECT_EQ(hash, "a430d84680aabd0b");
}

TEST(HashTest, DifferentInputsDiffer)
{
    const auto a = fnv1a_hex("alpha");
    const auto b = fnv1a_hex("bravo");
    EXPECT_NE(a, b);
}

TEST(HashTest, DeterministicAcrossCalls)
{
    const auto a = fnv1a_hex("some file content\nwith newlines\n");
    const auto b = fnv1a_hex("some file content\nwith newlines\n");
    EXPECT_EQ(a, b);
}

TEST(HashTest, BinaryData)
{
    const std::string data{'\0', '\x01', '\xff', '\x80'};
    const auto hash = fnv1a_hex(data);
    EXPECT_EQ(hash.size(), 16U);
}

} // namespace
