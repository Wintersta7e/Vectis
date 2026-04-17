#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/mode.h"
#include "core/task_queue.h"
#include "modes/ask/conversation.h"
#include "modes/ask/web_search.h"
#include "services/index_engine/index_engine.h"

namespace vectis::core {
class ConfigManager;
class ContextBus;
class ServiceRegistry;
} // namespace vectis::core

namespace vectis::modes::ask {

class ConversationStore;

/// Ask mode — the second IMode plugin.
///
/// Provides a chat-like UI for asking questions. Results come from
/// web search (DuckDuckGo / Brave) and/or the FTS5 code index.
/// No AI summarization in this step — results are displayed directly.
/// Conversations are persisted to SQLite.
class AskMode final : public vectis::core::IMode {
public:
    AskMode();
    ~AskMode() override;

    AskMode(const AskMode&)            = delete;
    AskMode& operator=(const AskMode&) = delete;

    std::string_view id()   const override { return "ask"; }
    std::string_view name() const override { return "Ask"; }

    void initialize(vectis::core::ServiceRegistry& services) override;
    void render() override;
    void on_activate() override   {}
    void on_deactivate() override {}
    void shutdown() override;

private:
    // Actions
    void on_submit_question();
    void on_stop_streaming();
    void on_new_conversation();
    void on_select_conversation(std::int64_t conversation_id);
    void on_delete_conversation(std::int64_t conversation_id);

    // Search
    void execute_search(std::string_view query);

    // Rendering
    void render_conversation_sidebar();
    void render_chat_area();
    void render_message(const Message& msg);
    void render_input_bar();
    void ensure_docking_layout(unsigned int dockspace_id);
    void refresh_conversation_list();

    // Services (borrowed)
    vectis::core::ServiceRegistry* m_services = nullptr;
    vectis::core::ConfigManager*   m_config   = nullptr;
    vectis::core::ContextBus*      m_bus      = nullptr;

    // Owned
    std::unique_ptr<vectis::core::TaskQueue>        m_task_queue;
    std::unique_ptr<ConversationStore>              m_store;
    std::unique_ptr<WebSearch>                      m_web_search;
    std::unique_ptr<vectis::platform::HttpClient>   m_http;

    // State
    std::vector<Conversation> m_conversation_list;
    Conversation              m_current_conversation;
    std::int64_t              m_active_conversation_id = 0;
    char                      m_input_buffer[1024] = {};
    std::atomic<bool>         m_codebase_available{false};
    std::atomic<bool>         m_search_running{false};
    std::uint64_t             m_bus_sub_id = 0;
    bool                      m_scroll_to_bottom = false;

    // Live streaming state. `m_stream_buffer` is written by the AI
    // token callback on the TaskQueue worker and read by the render
    // loop on the main thread — always under `m_stream_mutex`.
    // `m_streaming` is an independent atomic signal so the render
    // path can skip the lock when no stream is active.
    std::mutex        m_stream_mutex;
    std::string       m_stream_buffer;
    std::atomic<bool> m_streaming{false};

    // Docking
    bool m_dock_layout_built = false;
};

} // namespace vectis::modes::ask
