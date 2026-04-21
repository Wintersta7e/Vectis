#pragma once

#include <string_view>

#include "code/language.h"

namespace vectis::code {

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

/// S-expression tree-sitter query for extracting import / include /
/// use / require statements from a file. Returns an empty view for
/// languages Vectis doesn't yet support for dependency extraction —
/// the parser treats an empty query as "no imports".
[[nodiscard]] std::string_view import_query_for(Language language) noexcept;

/// S-expression tree-sitter query for extracting namespace declarations.
/// Currently populated for C# (`namespace Foo.Bar { ... }` and the C# 10
/// file-scoped form `namespace Foo.Bar;`) and PHP (`namespace Foo\Bar;`).
/// Returns an empty view for languages without a namespace concept or
/// where namespaces don't map to file-level dependencies.
///
/// The query is expected to capture a single `@name` on the node that
/// holds the namespace's dotted path (C#: `qualified_name`/`identifier`,
/// PHP: `namespace_name`).
[[nodiscard]] std::string_view namespace_query_for(Language language) noexcept;

} // namespace vectis::code
