#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "modes/ask/conversation.h"

namespace vectis::services { class StorageEngine; }

namespace vectis::modes::ask {

/// CRUD layer for conversations and messages over StorageEngine.
class ConversationStore {
public:
    explicit ConversationStore(vectis::services::StorageEngine& storage);

    /// Create a new conversation. Returns its ID.
    [[nodiscard]] vectis::core::Result<std::int64_t>
    create_conversation(std::string_view title);

    /// Update a conversation's title.
    [[nodiscard]] vectis::core::Result<void>
    update_title(std::int64_t conversation_id, std::string_view title);

    /// Delete a conversation and all its messages (CASCADE).
    [[nodiscard]] vectis::core::Result<void>
    delete_conversation(std::int64_t conversation_id);

    /// Add a message to a conversation. Returns the message ID.
    [[nodiscard]] vectis::core::Result<std::int64_t>
    add_message(std::int64_t             conversation_id,
                std::string_view         role,
                std::string_view         content,
                const std::vector<SourceCitation>& sources = {});

    /// List all conversations (without messages), newest first.
    [[nodiscard]] vectis::core::Result<std::vector<Conversation>>
    list_conversations();

    /// Load a single conversation with all its messages.
    [[nodiscard]] vectis::core::Result<Conversation>
    load_conversation(std::int64_t conversation_id);

private:
    vectis::services::StorageEngine* m_storage;
};

} // namespace vectis::modes::ask
