#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vectis::modes::ask {

/// A source citation attached to an assistant message.
struct SourceCitation {
    std::string type;       ///< "file", "url", "symbol"
    std::string location;   ///< file path:line, URL, or symbol name
    std::string snippet;    ///< relevant text excerpt
};

/// One message in a conversation.
struct Message {
    std::int64_t             id = 0;
    std::int64_t             conversation_id = 0;
    std::string              role;       ///< "user" or "assistant"
    std::string              content;
    std::vector<SourceCitation> sources;
    std::int64_t             created_at = 0; ///< unix timestamp
};

/// A conversation (with or without loaded messages).
struct Conversation {
    std::int64_t             id = 0;
    std::string              title;
    std::int64_t             created_at = 0; ///< unix timestamp
    std::vector<Message>     messages;       ///< empty when listed, populated when loaded
};

} // namespace vectis::modes::ask
