#include "modes/ask/ask_mode.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "core/log.h"
#include "core/service_registry.h"
#include "modes/ask/conversation.h"
#include "modes/ask/conversation_store.h"
#include "modes/ask/web_search.h"
#include "platform/http_client.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"

namespace vectis::modes::ask {

namespace {

constexpr const char* k_panel_sidebar = "Conversations";
constexpr const char* k_panel_chat    = "Chat";
constexpr const char* k_dockspace     = "AskDockspace";

/// Build a formatted "assistant" message from search results.
std::string format_results(
    const std::vector<WebSearchResult>&              web_results,
    const std::vector<vectis::services::SearchResult>& code_results)
{
    std::string content;

    if (!code_results.empty()) {
        content += "Code Results:\n\n";
        for (const auto& r : code_results) {
            content += "  [" + r.source + "] " + r.title + "\n";
            if (!r.snippet.empty()) {
                content += "    " + r.snippet + "\n";
            }
            content += "\n";
        }
    }

    if (!web_results.empty()) {
        if (!content.empty()) content += "\n";
        content += "Web Results:\n\n";
        for (const auto& r : web_results) {
            content += "  " + r.title + "\n";
            if (!r.url.empty()) {
                content += "    " + r.url + "\n";
            }
            if (!r.snippet.empty()) {
                content += "    " + r.snippet + "\n";
            }
            content += "\n";
        }
    }

    if (content.empty()) {
        content = "No results found for your query.";
    }

    return content;
}

/// Build source citations from search results.
std::vector<SourceCitation> build_citations(
    const std::vector<WebSearchResult>&              web_results,
    const std::vector<vectis::services::SearchResult>& code_results)
{
    std::vector<SourceCitation> citations;
    for (const auto& r : code_results) {
        citations.push_back({r.source, r.title, r.snippet});
    }
    for (const auto& r : web_results) {
        citations.push_back({"url", r.url, r.snippet});
    }
    return citations;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

AskMode::AskMode() = default;
AskMode::~AskMode() = default;

void AskMode::initialize(vectis::core::ServiceRegistry& services)
{
    m_services = &services;
    m_config   = &services.config();
    m_bus      = &services.context();

    m_store = std::make_unique<ConversationStore>(services.storage());
    m_http  = std::make_unique<vectis::platform::HttpClient>();
    m_web_search = std::make_unique<WebSearch>(*m_http, *m_config);

    // Subscribe to codebase availability.
    m_bus_sub_id = m_bus->subscribe("codebase.indexed",
        [this](const vectis::core::ContextData&) {
            m_codebase_available.store(true, std::memory_order_release);
        });

    refresh_conversation_list();

    VECTIS_LOG_INFO("AskMode initialized (web provider: {})",
                    m_web_search->active_provider());
}

void AskMode::shutdown()
{
    if (m_bus != nullptr && m_bus_sub_id != 0) {
        m_bus->unsubscribe(m_bus_sub_id);
        m_bus_sub_id = 0;
    }
    m_web_search.reset();
    m_http.reset();
    m_store.reset();
    VECTIS_LOG_INFO("AskMode shut down");
}

// ============================================================================
// Actions
// ============================================================================

void AskMode::on_new_conversation()
{
    auto r = m_store->create_conversation("New Chat");
    if (!r) {
        VECTIS_LOG_ERROR("Failed to create conversation: {}", r.error().message);
        return;
    }
    m_active_conversation_id = *r;
    if (auto loaded = m_store->load_conversation(*r); loaded) {
        m_current_conversation = std::move(*loaded);
    }
    refresh_conversation_list();
    std::memset(m_input_buffer, 0, sizeof(m_input_buffer));
}

void AskMode::on_select_conversation(std::int64_t conversation_id)
{
    m_active_conversation_id = conversation_id;
    if (auto loaded = m_store->load_conversation(conversation_id); loaded) {
        m_current_conversation = std::move(*loaded);
    }
    m_scroll_to_bottom = true;
}

void AskMode::on_delete_conversation(std::int64_t conversation_id)
{
    (void)m_store->delete_conversation(conversation_id);
    if (m_active_conversation_id == conversation_id) {
        m_active_conversation_id = 0;
        m_current_conversation = {};
    }
    refresh_conversation_list();
}

void AskMode::on_submit_question()
{
    const std::string question(m_input_buffer);
    if (question.empty()) return;

    // Create a conversation if none is active.
    if (m_active_conversation_id == 0) {
        auto r = m_store->create_conversation(question.substr(0, 50));
        if (!r) {
            VECTIS_LOG_ERROR("Failed to create conversation: {}", r.error().message);
            return;
        }
        m_active_conversation_id = *r;
        refresh_conversation_list();
    } else if (m_current_conversation.messages.empty()) {
        // Auto-title from first question.
        (void)m_store->update_title(m_active_conversation_id, question.substr(0, 50));
        refresh_conversation_list();
    }

    // Save user message.
    (void)m_store->add_message(m_active_conversation_id, "user", question);

    // Clear input.
    std::memset(m_input_buffer, 0, sizeof(m_input_buffer));

    // Execute search and create assistant response.
    execute_search(question);

    // Reload the conversation.
    if (auto loaded = m_store->load_conversation(m_active_conversation_id); loaded) {
        m_current_conversation = std::move(*loaded);
    }
    m_scroll_to_bottom = true;
}

void AskMode::execute_search(std::string_view query)
{
    // Code search.
    std::vector<vectis::services::SearchResult> code_results;
    if (m_codebase_available.load(std::memory_order_acquire) && m_services != nullptr) {
        code_results = m_services->index().search(query, 5);
    }

    // Web search.
    std::vector<WebSearchResult> web_results;
    auto web_r = m_web_search->search(query, 5);
    if (web_r) {
        web_results = std::move(*web_r);
    } else {
        VECTIS_LOG_WARN("Web search failed: {}", web_r.error().message);
    }

    // Build and save assistant message.
    const auto content  = format_results(web_results, code_results);
    const auto citations = build_citations(web_results, code_results);
    (void)m_store->add_message(m_active_conversation_id, "assistant", content, citations);
}

void AskMode::refresh_conversation_list()
{
    if (auto r = m_store->list_conversations(); r) {
        m_conversation_list = std::move(*r);
    }
}

// ============================================================================
// Rendering
// ============================================================================

void AskMode::ensure_docking_layout(unsigned int dockspace_id)
{
    if (m_dock_layout_built) return;
    m_dock_layout_built = true;

    auto* node = ImGui::DockBuilderGetNode(dockspace_id);
    if (node != nullptr && node->IsSplitNode()) return;

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    ImGuiID main_id = dockspace_id;
    ImGuiID left_id = ImGui::DockBuilderSplitNode(
        main_id, ImGuiDir_Left, 0.22F, nullptr, &main_id);

    ImGui::DockBuilderDockWindow(k_panel_sidebar, left_id);
    ImGui::DockBuilderDockWindow(k_panel_chat, main_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void AskMode::render()
{
    const ImGuiID dockspace_id = ImGui::GetID(k_dockspace);
    ensure_docking_layout(dockspace_id);

    render_conversation_sidebar();
    render_chat_area();
}

void AskMode::render_conversation_sidebar()
{
    if (!ImGui::Begin(k_panel_sidebar)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("New Chat", ImVec2(-1.0F, 0.0F))) {
        on_new_conversation();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("##conv_list", ImVec2(0.0F, 0.0F), false)) {
        for (const auto& conv : m_conversation_list) {
            const bool is_selected = (conv.id == m_active_conversation_id);
            const std::string label =
                conv.title.empty() ? "(untitled)" : conv.title;

            if (ImGui::Selectable(label.c_str(), is_selected)) {
                on_select_conversation(conv.id);
            }

            // Context menu for delete.
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    on_delete_conversation(conv.id);
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

void AskMode::render_chat_area()
{
    if (!ImGui::Begin(k_panel_chat)) {
        ImGui::End();
        return;
    }

    if (m_active_conversation_id == 0) {
        // Empty state.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* headline = "Ask a question";
        const ImVec2 text_size = ImGui::CalcTextSize(headline);
        ImGui::SetCursorPos(ImVec2(
            (avail.x - text_size.x) * 0.5F,
            avail.y * 0.35F));
        ImGui::TextUnformatted(headline);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Type a question below and press Enter.\n"
            "Results come from web search%s.",
            m_codebase_available.load(std::memory_order_relaxed)
                ? " and your loaded codebase"
                : "");
    } else {
        // Message history (scrollable).
        const float input_h = ImGui::GetFrameHeightWithSpacing() * 2.0F + 8.0F;
        if (ImGui::BeginChild("##messages", ImVec2(0.0F, -input_h), false)) {
            for (const auto& msg : m_current_conversation.messages) {
                render_message(msg);
            }
            if (m_scroll_to_bottom) {
                ImGui::SetScrollHereY(1.0F);
                m_scroll_to_bottom = false;
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
    }

    render_input_bar();
    ImGui::End();
}

void AskMode::render_message(const Message& msg)
{
    const bool is_user = (msg.role == "user");

    // Role label.
    if (is_user) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45F, 0.82F, 0.52F, 1.0F));
        ImGui::TextUnformatted("Q:");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55F, 0.75F, 0.95F, 1.0F));
        ImGui::TextUnformatted("A:");
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextWrapped("%s", msg.content.c_str());

    // Source citations (for assistant messages).
    if (!is_user && !msg.sources.empty()) {
        ImGui::Indent(16.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6F, 0.6F, 0.7F, 1.0F));
        for (const auto& src : msg.sources) {
            const std::string cite = "[" + src.type + "] " + src.location;
            ImGui::BulletText("%s", cite.c_str());
            if (ImGui::IsItemHovered() && !src.snippet.empty()) {
                ImGui::SetTooltip("%s", src.snippet.c_str());
            }
        }
        ImGui::PopStyleColor();
        ImGui::Unindent(16.0F);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}

void AskMode::render_input_bar()
{
    const float button_w = 60.0F;
    const float spacing  = ImGui::GetStyle().ItemSpacing.x;

    ImGui::PushItemWidth(-(button_w + spacing));
    const bool submitted = ImGui::InputText(
        "##ask_input", m_input_buffer, sizeof(m_input_buffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Send", ImVec2(button_w, 0.0F)) || submitted) {
        on_submit_question();
    }
}

} // namespace vectis::modes::ask
