#pragma once

#include <string_view>

namespace vectis::modes::ask {

/// System prompt given to the AI on every Ask-mode query. Keeps the
/// tone consistent and reminds the model to cite sources.
///
/// Note on citations: the codebase context currently provides file
/// paths only (no line numbers — `IndexEngine::SearchResult` doesn't
/// track them). Asking the model to cite line numbers would invite
/// fabricated line references. Step 13 polish will revisit if we
/// start extracting line hints from FTS snippets.
constexpr std::string_view k_system_prompt =
    "You are Vectis, a developer assistant. Answer concisely and accurately. "
    "When answering from code, cite the relevant file paths. When "
    "answering from the web, cite URLs. If you don't know, say so — "
    "don't fabricate. Format code snippets with appropriate language tags.";

/// Section headers used by the context builder. Each block is prefixed
/// with its header and terminated by a blank line so the model can
/// tell them apart.
constexpr std::string_view k_codebase_header = "Context from the user's codebase:\n\n";
constexpr std::string_view k_web_header      = "Context from web search:\n\n";
constexpr std::string_view k_history_header  = "Previous conversation:\n\n";

/// Separator before the actual question.
constexpr std::string_view k_question_prefix = "\n\nUser question: ";

} // namespace vectis::modes::ask
