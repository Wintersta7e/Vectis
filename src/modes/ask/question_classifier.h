#pragma once

#include <string_view>

namespace vectis::modes::ask {

/// Where to pull context from when answering a user's question.
enum class QuestionSource {
    Codebase,   ///< Question is about the currently-loaded project.
    Web,        ///< General knowledge; answer from web search.
    Mixed,      ///< Both codebase and web context are useful.
};

/// Pure heuristic classifier. No IO, no service lookups — just
/// pattern-matching on the question text plus the caller-supplied
/// "is a codebase loaded" signal. Deterministic and trivially
/// unit-testable.
///
/// Priority order (first matching rule wins, except for the Mixed
/// combination at step 4):
///   1. File-path / extension pattern → Codebase
///   2. Code-domain keyword AND codebase_loaded → Codebase (candidate)
///   3. Web-domain keyword → Web (candidate)
///   4. Both codebase and web signals present → Mixed
///   5. Default: Codebase if loaded, else Web
[[nodiscard]] QuestionSource classify_question(std::string_view question,
                                               bool             codebase_loaded);

} // namespace vectis::modes::ask
