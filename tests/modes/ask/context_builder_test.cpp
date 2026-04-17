#include "modes/ask/context_builder.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/ask/conversation.h"
#include "modes/ask/prompt_templates.h"
#include "modes/ask/web_search.h"
#include "services/index_engine/index_engine.h"

namespace {

using vectis::modes::ask::assemble_user_prompt;
using vectis::modes::ask::build_codebase_context;
using vectis::modes::ask::build_conversation_history;
using vectis::modes::ask::build_web_context;
using vectis::modes::ask::k_codebase_header;
using vectis::modes::ask::k_history_header;
using vectis::modes::ask::k_question_prefix;
using vectis::modes::ask::k_web_header;
using vectis::modes::ask::Message;
using vectis::modes::ask::WebSearchResult;
using vectis::services::SearchResult;

// ---- build_codebase_context -------------------------------------------------

TEST(ContextBuilderTest, CodebaseEmptyReturnsEmpty)
{
    EXPECT_EQ(build_codebase_context({}), "");
}

TEST(ContextBuilderTest, CodebaseFormatsEachHit)
{
    std::vector<SearchResult> hits{
        {"file", 1, "src/auth.cpp", "void authenticate() {...}", 0.1},
        {"file", 2, "src/user.cpp", "class User {...}",          0.2},
    };
    const auto out = build_codebase_context(hits);
    EXPECT_NE(out.find("File: src/auth.cpp"), std::string::npos);
    EXPECT_NE(out.find("void authenticate"), std::string::npos);
    EXPECT_NE(out.find("File: src/user.cpp"), std::string::npos);
    EXPECT_NE(out.find("class User"), std::string::npos);
}

// ---- build_web_context ------------------------------------------------------

TEST(ContextBuilderTest, WebEmptyReturnsEmpty)
{
    EXPECT_EQ(build_web_context({}), "");
}

TEST(ContextBuilderTest, WebFormatsEachHit)
{
    std::vector<WebSearchResult> hits{
        {"CAP theorem - Wikipedia", "https://en.wikipedia.org/wiki/CAP_theorem",
         "In database theory, the CAP theorem states..."},
    };
    const auto out = build_web_context(hits);
    EXPECT_NE(out.find("CAP theorem"), std::string::npos);
    EXPECT_NE(out.find("wikipedia.org"), std::string::npos);
    EXPECT_NE(out.find("database theory"), std::string::npos);
}

// ---- build_conversation_history --------------------------------------------

TEST(ContextBuilderTest, ConversationEmptyReturnsEmpty)
{
    EXPECT_EQ(build_conversation_history({}), "");
}

TEST(ContextBuilderTest, ConversationFormatsRoleAndContent)
{
    std::vector<Message> msgs;
    msgs.push_back(Message{0, 0, "user", "what is REST?", {}, 0});
    msgs.push_back(Message{0, 0, "assistant", "REST is...", {}, 0});

    const auto out = build_conversation_history(msgs);
    EXPECT_NE(out.find("user: what is REST?"), std::string::npos);
    EXPECT_NE(out.find("assistant: REST is..."), std::string::npos);
}

TEST(ContextBuilderTest, ConversationTrimsToTailOfMaxPairs)
{
    // 4 pairs (8 messages); request max_pairs=2 → only last 4 messages.
    std::vector<Message> msgs;
    for (int i = 1; i <= 4; ++i) {
        msgs.push_back(Message{0, 0, "user",      "u" + std::to_string(i), {}, 0});
        msgs.push_back(Message{0, 0, "assistant", "a" + std::to_string(i), {}, 0});
    }

    const auto out = build_conversation_history(msgs, 2);
    EXPECT_EQ(out.find("u1"), std::string::npos);
    EXPECT_EQ(out.find("a1"), std::string::npos);
    EXPECT_EQ(out.find("u2"), std::string::npos);
    EXPECT_EQ(out.find("a2"), std::string::npos);
    EXPECT_NE(out.find("u3"), std::string::npos);
    EXPECT_NE(out.find("a4"), std::string::npos);
}

// ---- assemble_user_prompt --------------------------------------------------

TEST(ContextBuilderTest, AssembleFitsIncludesAllBlocks)
{
    const auto out = assemble_user_prompt(
        "Q?", "codebase-ctx\n", "web-ctx\n", "history-ctx\n", 10000);

    EXPECT_NE(out.find(k_history_header),  std::string::npos);
    EXPECT_NE(out.find("history-ctx"),      std::string::npos);
    EXPECT_NE(out.find(k_codebase_header), std::string::npos);
    EXPECT_NE(out.find("codebase-ctx"),     std::string::npos);
    EXPECT_NE(out.find(k_web_header),      std::string::npos);
    EXPECT_NE(out.find("web-ctx"),          std::string::npos);
    EXPECT_NE(out.find(k_question_prefix), std::string::npos);
    EXPECT_NE(out.find("Q?"),               std::string::npos);
}

TEST(ContextBuilderTest, AssembleDropsHistoryFirst)
{
    // Make the history so large it can't fit in a tiny budget, but
    // codebase + web + question together do fit.
    const std::string big_history(4000U, 'h');
    const auto out = assemble_user_prompt(
        "Q?", "codebase\n", "web\n", big_history, 500);

    EXPECT_EQ(out.find(big_history), std::string::npos);
    EXPECT_EQ(out.find(k_history_header), std::string::npos);
    EXPECT_NE(out.find("codebase"),  std::string::npos);
    EXPECT_NE(out.find("web"),       std::string::npos);
    EXPECT_NE(out.find("Q?"),        std::string::npos);
}

TEST(ContextBuilderTest, AssembleDropsHistoryAndWebWhenNeeded)
{
    const std::string big_history(4000U, 'h');
    const std::string big_web(4000U, 'w');
    const auto out = assemble_user_prompt(
        "Q?", "codebase\n", big_web, big_history, 500);

    EXPECT_EQ(out.find(big_history), std::string::npos);
    EXPECT_EQ(out.find(big_web),     std::string::npos);
    EXPECT_NE(out.find("codebase"),  std::string::npos);
    EXPECT_NE(out.find("Q?"),        std::string::npos);
}

TEST(ContextBuilderTest, AssemblePreservesQuestionEvenWhenEverythingElseDropped)
{
    const std::string big(4000U, 'x');
    const auto out = assemble_user_prompt(
        "important question?", big, big, big, 500);

    EXPECT_EQ(out.find(big), std::string::npos);
    EXPECT_NE(out.find("important question?"), std::string::npos);
    EXPECT_NE(out.find(k_question_prefix), std::string::npos);
}

} // namespace
