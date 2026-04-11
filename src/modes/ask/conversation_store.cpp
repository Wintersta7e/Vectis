#include "modes/ask/conversation_store.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/result.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::modes::ask {

using vectis::core::ErrorKind;
using vectis::core::make_error;
using vectis::core::Result;
using vectis::services::StorageEngine;

namespace {

std::int64_t now_epoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// Serialize sources to JSON string.
std::string serialize_sources(const std::vector<SourceCitation>& sources)
{
    if (sources.empty()) return {};
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sources) {
        arr.push_back({{"type", s.type}, {"location", s.location}, {"snippet", s.snippet}});
    }
    return arr.dump();
}

/// Deserialize sources from JSON string.
/// Maximum size of sources JSON we'll attempt to parse (1 MB).
constexpr std::size_t k_max_sources_json_bytes = 1024 * 1024;

std::vector<SourceCitation> deserialize_sources(const std::string& json_str)
{
    std::vector<SourceCitation> result;
    if (json_str.empty()) return result;
    if (json_str.size() > k_max_sources_json_bytes) {
        VECTIS_LOG_WARN("ConversationStore: sources JSON too large ({} bytes), skipping",
                        json_str.size());
        return result;
    }
    try {
        auto arr = nlohmann::json::parse(json_str);
        for (const auto& item : arr) {
            SourceCitation sc;
            sc.type     = item.value("type", "");
            sc.location = item.value("location", "");
            sc.snippet  = item.value("snippet", "");
            result.push_back(std::move(sc));
        }
    } catch (const nlohmann::json::exception& e) {
        VECTIS_LOG_WARN("ConversationStore: failed to parse sources JSON: {}", e.what());
    }
    return result;
}

} // namespace

ConversationStore::ConversationStore(StorageEngine& storage)
    : m_storage(&storage)
{
}

Result<std::int64_t> ConversationStore::create_conversation(std::string_view title)
{
    auto stmt = m_storage->prepare(
        "INSERT INTO conversations (title, created_at) VALUES (?, ?)");
    if (!stmt) return tl::unexpected(stmt.error());

    stmt->bind(1, title);
    stmt->bind(2, now_epoch());
    if (auto r = stmt->execute(); !r) return tl::unexpected(r.error());

    return stmt->last_insert_id();
}

Result<void> ConversationStore::update_title(std::int64_t conversation_id,
                                              std::string_view title)
{
    auto stmt = m_storage->prepare(
        "UPDATE conversations SET title = ? WHERE id = ?");
    if (!stmt) return tl::unexpected(stmt.error());

    stmt->bind(1, title);
    stmt->bind(2, conversation_id);
    return stmt->execute();
}

Result<void> ConversationStore::delete_conversation(std::int64_t conversation_id)
{
    auto stmt = m_storage->prepare("DELETE FROM conversations WHERE id = ?");
    if (!stmt) return tl::unexpected(stmt.error());

    stmt->bind(1, conversation_id);
    return stmt->execute();
}

Result<std::int64_t> ConversationStore::add_message(
    std::int64_t                        conversation_id,
    std::string_view                    role,
    std::string_view                    content,
    const std::vector<SourceCitation>&  sources)
{
    auto stmt = m_storage->prepare(
        "INSERT INTO messages (conversation_id, role, content, sources, created_at) "
        "VALUES (?, ?, ?, ?, ?)");
    if (!stmt) return tl::unexpected(stmt.error());

    const auto sources_json = serialize_sources(sources);

    stmt->bind(1, conversation_id);
    stmt->bind(2, role);
    stmt->bind(3, content);
    if (sources_json.empty()) {
        stmt->bind_null(4);
    } else {
        stmt->bind(4, std::string_view{sources_json});
    }
    stmt->bind(5, now_epoch());

    if (auto r = stmt->execute(); !r) return tl::unexpected(r.error());
    return stmt->last_insert_id();
}

Result<std::vector<Conversation>> ConversationStore::list_conversations()
{
    auto stmt = m_storage->prepare(
        "SELECT id, title, created_at FROM conversations ORDER BY id DESC");
    if (!stmt) return tl::unexpected(stmt.error());

    auto rows = stmt->query();
    if (!rows) return tl::unexpected(rows.error());

    std::vector<Conversation> result;
    result.reserve(rows->size());
    for (const auto& row : *rows) {
        Conversation c;
        c.id         = row.get_int(0);
        c.title      = row.get_text(1);
        c.created_at = row.get_int(2);
        result.push_back(std::move(c));
    }
    return result;
}

Result<Conversation> ConversationStore::load_conversation(std::int64_t conversation_id)
{
    // Load conversation header.
    auto conv_stmt = m_storage->prepare(
        "SELECT id, title, created_at FROM conversations WHERE id = ?");
    if (!conv_stmt) return tl::unexpected(conv_stmt.error());
    conv_stmt->bind(1, conversation_id);

    auto conv_rows = conv_stmt->query();
    if (!conv_rows) return tl::unexpected(conv_rows.error());
    if (conv_rows->empty()) {
        return make_error(ErrorKind::StorageError, "conversation not found");
    }

    Conversation c;
    c.id         = (*conv_rows)[0].get_int(0);
    c.title      = (*conv_rows)[0].get_text(1);
    c.created_at = (*conv_rows)[0].get_int(2);

    // Load messages.
    auto msg_stmt = m_storage->prepare(
        "SELECT id, conversation_id, role, content, sources, created_at "
        "FROM messages WHERE conversation_id = ? ORDER BY created_at ASC");
    if (!msg_stmt) return tl::unexpected(msg_stmt.error());
    msg_stmt->bind(1, conversation_id);

    auto msg_rows = msg_stmt->query();
    if (!msg_rows) return tl::unexpected(msg_rows.error());

    c.messages.reserve(msg_rows->size());
    for (const auto& row : *msg_rows) {
        Message m;
        m.id              = row.get_int(0);
        m.conversation_id = row.get_int(1);
        m.role            = row.get_text(2);
        m.content         = row.get_text(3);
        m.sources         = deserialize_sources(row.get_text(4));
        m.created_at      = row.get_int(5);
        c.messages.push_back(std::move(m));
    }
    return c;
}

} // namespace vectis::modes::ask
