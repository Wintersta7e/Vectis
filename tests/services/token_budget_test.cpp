#include "services/ai_engine/token_budget.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace {

using vectis::services::estimate_tokens;

TEST(TokenBudgetTest, EmptyStringReturnsZero)
{
    EXPECT_EQ(estimate_tokens(""), 0);
}

TEST(TokenBudgetTest, ShortWordIsSmallPositive)
{
    // "hello" — 5 chars → 5 * 2 / 7 == 1 token.
    EXPECT_EQ(estimate_tokens("hello"), 1);
}

TEST(TokenBudgetTest, CodeSnippetScalesRoughlyWithLength)
{
    const std::string snippet =
        "int main() { return 0; }";       // 24 chars
    EXPECT_EQ(estimate_tokens(snippet), (24 * 2) / 7);
    EXPECT_GT(estimate_tokens(snippet), 0);
}

TEST(TokenBudgetTest, IsConstexpr)
{
    // Compile-time evaluation should succeed; this is the real
    // guarantee the heuristic offers for header-only budgeting.
    constexpr int value = estimate_tokens("constexpr-safe");
    static_assert(value > 0, "estimate_tokens must be constexpr-callable");
    EXPECT_EQ(value, (14 * 2) / 7);
}

} // namespace
