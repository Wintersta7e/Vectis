#include "modes/ask/context_builder.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modes/ask/conversation.h"
#include "modes/ask/prompt_templates.h"
#include "modes/ask/web_search.h"
#include "services/ai_engine/token_budget.h"
#include "services/index_engine/index_engine.h"

namespace vectis::modes::ask {

std::string build_codebase_context(
    const std::vector<vectis::services::SearchResult>& hits)
{
    if (hits.empty()) return {};

    std::string out;
    out.reserve(hits.size() * 256U);
    for (const auto& h : hits) {
        out.append("File: ");
        out.append(h.title);
        out.append(1, '\n');
        if (!h.snippet.empty()) {
            out.append(h.snippet);
            out.append(1, '\n');
        }
        out.append(1, '\n');
    }
    return out;
}

std::string build_web_context(const std::vector<WebSearchResult>& hits)
{
    if (hits.empty()) return {};

    std::string out;
    out.reserve(hits.size() * 256U);
    for (const auto& h : hits) {
        out.append("Source: ");
        out.append(h.title);
        if (!h.url.empty()) {
            out.append(" (");
            out.append(h.url);
            out.append(1, ')');
        }
        out.append(1, '\n');
        if (!h.snippet.empty()) {
            out.append(h.snippet);
            out.append(1, '\n');
        }
        out.append(1, '\n');
    }
    return out;
}

std::string build_conversation_history(const std::vector<Message>& messages,
                                       std::size_t                 max_pairs)
{
    if (messages.empty() || max_pairs == 0) return {};

    // A "pair" here is a user + assistant turn; render up to
    // 2 * max_pairs messages from the tail.
    const std::size_t max_msgs = max_pairs * 2U;
    const std::size_t start =
        messages.size() > max_msgs ? messages.size() - max_msgs : 0U;

    std::string out;
    out.reserve((messages.size() - start) * 128U);
    for (std::size_t i = start; i < messages.size(); ++i) {
        const auto& m = messages[i];
        out.append(m.role);
        out.append(": ");
        out.append(m.content);
        out.append("\n\n");
    }
    return out;
}

namespace {

/// Compose the final prompt by concatenating the non-empty blocks in
/// order: history, codebase, web, question.
std::string compose(std::string_view history, std::string_view codebase,
                    std::string_view web,     std::string_view question)
{
    std::string out;
    out.reserve(history.size() + codebase.size() + web.size() +
                k_question_prefix.size() + question.size() + 64U);

    if (!history.empty()) {
        out.append(k_history_header);
        out.append(history);
    }
    if (!codebase.empty()) {
        out.append(k_codebase_header);
        out.append(codebase);
    }
    if (!web.empty()) {
        out.append(k_web_header);
        out.append(web);
    }
    out.append(k_question_prefix);
    out.append(question);
    return out;
}

} // namespace

std::string assemble_user_prompt(std::string_view question,
                                 std::string      codebase_context,
                                 std::string      web_context,
                                 std::string      conversation_history,
                                 int              max_tokens)
{
    auto over_budget = [max_tokens](const std::string& s) {
        return vectis::services::estimate_tokens(s) > max_tokens;
    };

    auto try_compose = [&]() {
        return compose(conversation_history, codebase_context,
                       web_context,          question);
    };

    std::string prompt = try_compose();
    if (!over_budget(prompt)) return prompt;

    // Drop oldest / least important context first: history, then web,
    // then codebase. The question itself is inviolate.
    conversation_history.clear();
    prompt = try_compose();
    if (!over_budget(prompt)) return prompt;

    web_context.clear();
    prompt = try_compose();
    if (!over_budget(prompt)) return prompt;

    codebase_context.clear();
    return try_compose();
}

} // namespace vectis::modes::ask
