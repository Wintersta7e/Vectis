#include "modes/ask/conversation_store.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "modes/ask/conversation.h"
#include "services/storage_engine/storage_engine.h"

namespace {

namespace fs = std::filesystem;
using namespace vectis::modes::ask;
using vectis::services::StorageEngine;

class ConversationStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_tmp_dir = fs::temp_directory_path() / "vectis_conv_test";
        fs::create_directories(m_tmp_dir);
        ASSERT_TRUE(m_storage.open(m_tmp_dir / "conv_test.db"));
        ASSERT_TRUE(m_storage.migrate());
        m_store = std::make_unique<ConversationStore>(m_storage);
    }

    void TearDown() override
    {
        m_store.reset();
        m_storage.close();
        std::error_code ec;
        fs::remove_all(m_tmp_dir, ec);
    }

    StorageEngine                      m_storage;
    std::unique_ptr<ConversationStore> m_store;
    fs::path                           m_tmp_dir;
};

TEST_F(ConversationStoreTest, CreateConversation)
{
    auto r = m_store->create_conversation("Test Chat");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_GT(*r, 0);
}

TEST_F(ConversationStoreTest, ListConversations_OrderedByCreatedDesc)
{
    ASSERT_TRUE(m_store->create_conversation("First"));
    ASSERT_TRUE(m_store->create_conversation("Second"));

    auto r = m_store->list_conversations();
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_EQ(r->size(), 2U);
    // Newest first — but with same-second granularity, order by ID desc.
    EXPECT_EQ((*r)[0].title, "Second");
    EXPECT_EQ((*r)[1].title, "First");
}

TEST_F(ConversationStoreTest, ListConversations_Empty)
{
    auto r = m_store->list_conversations();
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->empty());
}

TEST_F(ConversationStoreTest, AddMessage_RoundTrip)
{
    auto conv_id = m_store->create_conversation("Chat");
    ASSERT_TRUE(conv_id);

    ASSERT_TRUE(m_store->add_message(*conv_id, "user", "How does auth work?"));
    ASSERT_TRUE(m_store->add_message(*conv_id, "assistant", "Auth uses JWT tokens."));

    auto loaded = m_store->load_conversation(*conv_id);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->title, "Chat");
    ASSERT_EQ(loaded->messages.size(), 2U);
    EXPECT_EQ(loaded->messages[0].role, "user");
    EXPECT_EQ(loaded->messages[0].content, "How does auth work?");
    EXPECT_EQ(loaded->messages[1].role, "assistant");
    EXPECT_EQ(loaded->messages[1].content, "Auth uses JWT tokens.");
}

TEST_F(ConversationStoreTest, AddMessage_WithSources_JsonRoundTrip)
{
    auto conv_id = m_store->create_conversation("Sourced");
    ASSERT_TRUE(conv_id);

    std::vector<SourceCitation> sources = {
        {"file", "src/auth.cpp:42", "bool validate_token(...)"},
        {"url",  "https://example.com/docs", "JWT documentation"},
    };

    ASSERT_TRUE(m_store->add_message(*conv_id, "assistant", "Results:", sources));

    auto loaded = m_store->load_conversation(*conv_id);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->messages.size(), 1U);

    const auto& msg = loaded->messages[0];
    ASSERT_EQ(msg.sources.size(), 2U);
    EXPECT_EQ(msg.sources[0].type, "file");
    EXPECT_EQ(msg.sources[0].location, "src/auth.cpp:42");
    EXPECT_EQ(msg.sources[0].snippet, "bool validate_token(...)");
    EXPECT_EQ(msg.sources[1].type, "url");
    EXPECT_EQ(msg.sources[1].location, "https://example.com/docs");
}

TEST_F(ConversationStoreTest, DeleteConversation_CascadesMessages)
{
    auto conv_id = m_store->create_conversation("Doomed");
    ASSERT_TRUE(conv_id);
    ASSERT_TRUE(m_store->add_message(*conv_id, "user", "Hello"));
    ASSERT_TRUE(m_store->add_message(*conv_id, "assistant", "World"));

    ASSERT_TRUE(m_store->delete_conversation(*conv_id));

    auto list = m_store->list_conversations();
    ASSERT_TRUE(list);
    EXPECT_TRUE(list->empty());

    auto loaded = m_store->load_conversation(*conv_id);
    EXPECT_FALSE(loaded); // not found
}

TEST_F(ConversationStoreTest, UpdateTitle)
{
    auto conv_id = m_store->create_conversation("Original");
    ASSERT_TRUE(conv_id);

    ASSERT_TRUE(m_store->update_title(*conv_id, "Updated Title"));

    auto loaded = m_store->load_conversation(*conv_id);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->title, "Updated Title");
}

TEST_F(ConversationStoreTest, LoadConversation_NonExistent)
{
    auto r = m_store->load_conversation(9999);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error().kind, vectis::core::ErrorKind::StorageError);
}

} // namespace
