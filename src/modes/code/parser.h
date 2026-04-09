#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace vectis::modes::code {

/// Tree-sitter wrapper that parses source files and extracts symbols.
///
/// Each instance owns one reusable `TSParser*` plus a compiled
/// `TSQuery*` per registered language. Queries come from
/// `parser_queries.h` and the grammars come from the seven
/// `ts_grammar_*` static libraries linked by CMake.
///
/// Thread-safety: **not** thread-safe. Each worker thread that wants
/// to parse must own its own `TreeSitterParser` instance.
class TreeSitterParser {
public:
    /// Outcome of parsing one file. `symbols.file_id` is always 0 —
    /// the scanner fills it in before handing the batch to
    /// `CodeIndex::add_symbols`.
    struct ParseResult {
        std::vector<Symbol> symbols;
    };

    TreeSitterParser();
    ~TreeSitterParser();

    TreeSitterParser(const TreeSitterParser&)            = delete;
    TreeSitterParser& operator=(const TreeSitterParser&) = delete;

    /// Register the built-in set of grammars (Python, JavaScript,
    /// TypeScript, C, C++, Rust, Java). Idempotent — calling twice is
    /// a no-op. Returns the number of languages that were successfully
    /// registered (query compilation failures are logged and skipped).
    std::size_t register_builtin_languages();

    /// Parse one file's contents and extract symbols according to the
    /// language's query. Returns an empty result for `Unknown`,
    /// unregistered, or unparsable content — all non-fatal conditions
    /// are logged and surfaced as an empty result, never an exception.
    [[nodiscard]] ParseResult parse_file(Language language, std::string_view content);

    /// True if a grammar and query have been successfully registered
    /// for this language.
    [[nodiscard]] bool supports(Language language) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::modes::code
