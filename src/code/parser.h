#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "code/language.h"
#include "code/symbol.h"

namespace vectis::code {

/// Tree-sitter wrapper that parses source files and extracts symbols.
///
/// Each instance owns one reusable `TSParser*` plus a compiled
/// `TSQuery*` per registered language. Queries come from
/// `parser_queries.h` and the grammars come from the seven
/// `ts_grammar_*` static libraries linked by CMake.
///
/// Thread-safety: **not** thread-safe. Each worker thread that wants
/// to parse must own its own `TreeSitterParser` instance.
/// One raw import extracted from a file's parse tree. The scanner
/// collects a vector of these per file and the dependency resolver
/// turns them into `Dependency` edges in a second pass.
struct RawImport
{
    std::string import_string; ///< Raw path text ("./foo", "core/log.h", "fmt")
    std::string kind;          ///< "include" | "import" | "use" | "require" | "mod"
    int line = 0;              ///< 1-based source line
};

class TreeSitterParser
{
public:
    /// Outcome of parsing one file. `symbols.file_id` is always 0 —
    /// the scanner fills it in before handing the batch to
    /// `CodeIndex::add_symbols`.
    struct ParseResult
    {
        std::vector<Symbol> symbols;
    };

    TreeSitterParser();
    ~TreeSitterParser();

    TreeSitterParser(const TreeSitterParser&) = delete;
    TreeSitterParser& operator=(const TreeSitterParser&) = delete;
    TreeSitterParser(TreeSitterParser&&) = delete;
    TreeSitterParser& operator=(TreeSitterParser&&) = delete;

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

    /// Extract raw import statements from a file's contents. Returns
    /// an empty vector for languages that don't yet have an import
    /// query wired up, or when no imports are present. Never throws.
    [[nodiscard]] std::vector<RawImport> extract_imports(Language language,
                                                         std::string_view content);

    /// Extract the namespace declarations made by a file. Populated
    /// only for C# (block + file-scoped) and PHP today. Returns an
    /// empty vector for any language without a namespace query; never
    /// throws. Used by the dependency resolver to build a
    /// namespace → files map for `using`/`use` resolution.
    [[nodiscard]] std::vector<std::string> extract_namespaces(Language language,
                                                              std::string_view content);

    /// True if a grammar and query have been successfully registered
    /// for this language.
    [[nodiscard]] bool supports(Language language) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vectis::code
