#include "modes/ask/question_classifier.h"

#include <gtest/gtest.h>

namespace {

using vectis::modes::ask::classify_question;
using vectis::modes::ask::QuestionSource;

// ---- Rule 1: file path patterns -------------------------------------------

TEST(QuestionClassifierTest, FilePathPrefixIsCodebase)
{
    EXPECT_EQ(classify_question("What does src/auth/guard.ts do?", true),
              QuestionSource::Codebase);
}

TEST(QuestionClassifierTest, FileExtensionIsCodebase)
{
    EXPECT_EQ(classify_question("Explain the logic in user_model.py", true),
              QuestionSource::Codebase);
    EXPECT_EQ(classify_question("fix this bug in server.go", true),
              QuestionSource::Codebase);
}

TEST(QuestionClassifierTest, FilePathWithoutCodebaseRoutesToWeb)
{
    // With no codebase loaded, a file-path question can't be answered
    // from FTS. Routing to Codebase would leave the pipeline with zero
    // context — code search is skipped because codebase_loaded is
    // false, web search is skipped because source is Codebase, and
    // the AI ends up answering from priors (hallucinating the file
    // contents). Route to Web so at least search snippets are in
    // play.
    EXPECT_EQ(classify_question("What's in src/main.cpp?", false),
              QuestionSource::Web);
}

// ---- Rule 2: code keywords --------------------------------------------------

TEST(QuestionClassifierTest, CodeKeywordWithCodebaseIsCodebase)
{
    EXPECT_EQ(classify_question("show me the auth function", true),
              QuestionSource::Codebase);
    EXPECT_EQ(classify_question("where is the User class defined", true),
              QuestionSource::Codebase);
}

TEST(QuestionClassifierTest, CodeKeywordWithoutCodebaseFallsBackToWeb)
{
    // No codebase → code keywords alone aren't a Codebase signal.
    EXPECT_EQ(classify_question("show me the auth function", false),
              QuestionSource::Web);
}

// ---- Rule 3: web keywords ---------------------------------------------------

TEST(QuestionClassifierTest, WebPhrasingIsWeb)
{
    EXPECT_EQ(classify_question("How do I install Rust on WSL?", true),
              QuestionSource::Web);
    EXPECT_EQ(classify_question("What is CAP theorem?", true),
              QuestionSource::Web);
    EXPECT_EQ(classify_question("difference between mutex and semaphore", false),
              QuestionSource::Web);
}

// ---- Rule 4: mixed ---------------------------------------------------------

TEST(QuestionClassifierTest, CodeKeywordPlusWebPhrasingIsMixed)
{
    // "how do I" (web) + "function" (code) + codebase loaded → Mixed
    EXPECT_EQ(classify_question("how do I call this function from python?", true),
              QuestionSource::Mixed);
}

TEST(QuestionClassifierTest, MixedWithoutCodebaseIsWeb)
{
    EXPECT_EQ(classify_question("how do I write a good function?", false),
              QuestionSource::Web);
}

// ---- Rule 5: defaults -------------------------------------------------------

TEST(QuestionClassifierTest, DefaultWithCodebaseIsCodebase)
{
    EXPECT_EQ(classify_question("tell me about this", true),
              QuestionSource::Codebase);
}

TEST(QuestionClassifierTest, DefaultWithoutCodebaseIsWeb)
{
    EXPECT_EQ(classify_question("tell me something interesting", false),
              QuestionSource::Web);
}

// ---- Case insensitivity -----------------------------------------------------

TEST(QuestionClassifierTest, CaseInsensitive)
{
    // Mixed: "how do i" (web) + "class " (code) + codebase loaded.
    EXPECT_EQ(classify_question("HOW DO I WRITE A CLASS IN RUST?", true),
              QuestionSource::Mixed);
    EXPECT_EQ(classify_question("where is SRC/MAIN.CPP", true),
              QuestionSource::Codebase);
}

// ---- Word-boundary matching (L-4) ------------------------------------------

TEST(QuestionClassifierTest, FunctionKeywordDoesNotMatchDysfunction)
{
    // "dysfunction" contains "function" as a suffix but shouldn't
    // count as a code keyword — with the word-start matcher,
    // preceding character is 's' (alnum) so this rejects.
    EXPECT_EQ(classify_question("is this code dysfunctional?", false),
              QuestionSource::Web);
}

TEST(QuestionClassifierTest, FunctionKeywordDoesMatchRealUsage)
{
    // Normal usage: "the function foo()" — 'function' preceded by
    // ' ' (non-alnum) → code keyword matches.
    EXPECT_EQ(classify_question("what does the function foo() do?", true),
              QuestionSource::Codebase);
}

} // namespace
