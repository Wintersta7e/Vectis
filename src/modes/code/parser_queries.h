#pragma once

#include <string_view>

#include "modes/code/language.h"

namespace vectis::modes::code {

/// S-expression tree-sitter query strings for symbol extraction,
/// one per supported language.
///
/// Each query is a raw string literal compiled once by
/// `TreeSitterParser::register_builtin_languages()`. Captures follow
/// a convention: the kind capture (e.g. `@function`, `@class`) maps
/// to `SymbolKind`, and `@name` identifies the identifier node from
/// which the symbol's name is extracted.
///
/// These are Step 2 minimums — functions, classes, methods, types.
/// Step 3/4 can refine them without touching callers.
[[nodiscard]] std::string_view query_for(Language language) noexcept;

} // namespace vectis::modes::code
