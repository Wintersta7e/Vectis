#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "modes/ask/conversation.h"
#include "modes/ask/web_search.h"
#include "services/index_engine/index_engine.h"

namespace vectis::modes::ask {

/// Default budget for the assembled user prompt, expressed in
/// approximate tokens (via services::estimate_tokens). Conservative
/// enough to fit small local models (Ollama llama3 8k) with room to
/// spare for the system prompt and completion.
constexpr int k_default_prompt_token_budget = 8000;

/// Format code-search hits into a prompt-ready text block. Returns
/// an empty string when `hits` is empty. No header is prepended — the
/// assembler adds `k_codebase_header` around a non-empty result.
[[nodiscard]] std::string build_codebase_context(
    const std::vector<vectis::services::SearchResult>& hits);

/// Format web-search hits into a prompt-ready text block. Same
/// empty-string-on-empty-input contract as `build_codebase_context`.
[[nodiscard]] std::string build_web_context(
    const std::vector<WebSearchResult>& hits);

/// Format the last `max_pairs` user+assistant turns into a
/// "role: content" conversation-history block. Caller passes messages
/// in oldest-first order; this function selects the tail. Returns an
/// empty string when the list is empty.
[[nodiscard]] std::string build_conversation_history(
    const std::vector<Message>& messages,
    std::size_t                 max_pairs = 5);

/// Assemble the final user-turn content: history + codebase context +
/// web context + the question itself. If the total estimated tokens
/// exceed `max_tokens`, drop blocks in order (history → web →
/// codebase) until the total fits. The question is inviolate.
[[nodiscard]] std::string assemble_user_prompt(
    std::string_view question,
    std::string      codebase_context,
    std::string      web_context,
    std::string      conversation_history,
    int              max_tokens = k_default_prompt_token_budget);

} // namespace vectis::modes::ask
