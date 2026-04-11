#include "modes/ask/ask_mode.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/ask/conversation.h"
#include "modes/ask/conversation_store.h"
#include "modes/ask/web_search.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace {

namespace fs = std::filesystem;
using namespace vectis::modes::ask;
using vectis::services::IndexEngine;
using vectis::services::StorageEngine;

// ---- AskMode identity tests ------------------------------------------------

TEST(AskModeTest, IdAndName)
{
    AskMode mode;
    EXPECT_EQ(mode.id(), "ask");
    EXPECT_EQ(mode.name(), "Ask");
}

// ---- Integration tests using StorageEngine + IndexEngine -------------------

class AskModeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / "vectis_ask_int_test";
        fs::create_directories(m_tmp_dir);
        ASSERT_TRUE(m_storage.open(m_tmp_dir / "ask_int.db"));
        ASSERT_TRUE(m_storage.migrate());
    }

    void TearDown() override
    {
        m_storage.close();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    StorageEngine m_storage;
    fs::path      m_tmp_dir;
};

TEST_F(AskModeIntegrationTest, ConversationFullLifecycle)
{
    ConversationStore store(m_storage);

    // Create
    auto conv_id = store.create_conversation("Test Chat");
    ASSERT_TRUE(conv_id);

    // Add messages
    ASSERT_TRUE(store.add_message(*conv_id, "user", "What is tree-sitter?"));
    std::vector<SourceCitation> sources = {
        {"url", "https://tree-sitter.github.io/", "Parsing framework"},
    };
    ASSERT_TRUE(store.add_message(*conv_id, "assistant", "Results:", sources));

    // Load and verify
    auto loaded = store.load_conversation(*conv_id);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->messages.size(), 2U);
    EXPECT_EQ(loaded->messages[0].role, "user");
    EXPECT_EQ(loaded->messages[1].role, "assistant");
    ASSERT_EQ(loaded->messages[1].sources.size(), 1U);
    EXPECT_EQ(loaded->messages[1].sources[0].type, "url");

    // Delete
    ASSERT_TRUE(store.delete_conversation(*conv_id));
    auto list = store.list_conversations();
    ASSERT_TRUE(list);
    EXPECT_TRUE(list->empty());
}

TEST_F(AskModeIntegrationTest, CodeSearch_ReturnsResults)
{
    IndexEngine idx;
    idx.initialize(m_storage);

    // Index some content.
    idx.index_file(1, "src/scanner.cpp",
        "void Scanner::run() { parse all files in the directory tree; }");
    idx.index_file(2, "src/parser.cpp",
        "TreeSitterParser wraps tree-sitter for symbol extraction.");

    auto results = idx.search_files("scanner");
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].source, "file");
    EXPECT_EQ(results[0].source_id, 1);
}

TEST_F(AskModeIntegrationTest, SourceCitation_JsonRoundTrip)
{
    ConversationStore store(m_storage);

    auto conv_id = store.create_conversation("Sources Test");
    ASSERT_TRUE(conv_id);

    std::vector<SourceCitation> sources = {
        {"file", "src/main.cpp:10", "int main()"},
        {"url",  "https://example.com", "Example page"},
        {"symbol", "Scanner::run", "void Scanner::run()"},
    };

    ASSERT_TRUE(store.add_message(*conv_id, "assistant", "Here are the results:", sources));

    auto loaded = store.load_conversation(*conv_id);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->messages.size(), 1U);

    const auto& loaded_sources = loaded->messages[0].sources;
    ASSERT_EQ(loaded_sources.size(), 3U);
    EXPECT_EQ(loaded_sources[0].type, "file");
    EXPECT_EQ(loaded_sources[0].location, "src/main.cpp:10");
    EXPECT_EQ(loaded_sources[0].snippet, "int main()");
    EXPECT_EQ(loaded_sources[1].type, "url");
    EXPECT_EQ(loaded_sources[2].type, "symbol");
    EXPECT_EQ(loaded_sources[2].location, "Scanner::run");
}

TEST_F(AskModeIntegrationTest, ConversationAutoTitle)
{
    ConversationStore store(m_storage);

    // Create with generic title, update with first question.
    auto conv_id = store.create_conversation("New Chat");
    ASSERT_TRUE(conv_id);

    const std::string question = "How does the dependency resolver work in this project?";
    ASSERT_TRUE(store.update_title(*conv_id, question.substr(0, 50)));
    ASSERT_TRUE(store.add_message(*conv_id, "user", question));

    auto loaded = store.load_conversation(*conv_id);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->title, "How does the dependency resolver work in this proj");
}

} // namespace
