#include "code/parser.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tree_sitter/api.h>

#include "core/log.h"
#include "code/complexity_analyzer.h"
#include "code/language.h"
#include "code/parser_queries.h"
#include "code/symbol.h"

// -----------------------------------------------------------------------------
// Tree-sitter grammar entry points.
//
// Each grammar we link from the `ts_grammar_*` static libs exposes a single
// C function returning a `TSLanguage*`. Declaring them here in one place is
// the canonical way to avoid C++ name-mangling surprises and it surfaces the
// full supported set at a glance.
// -----------------------------------------------------------------------------
extern "C" {
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_rust();
const TSLanguage* tree_sitter_java();
const TSLanguage* tree_sitter_c_sharp();
const TSLanguage* tree_sitter_go();
const TSLanguage* tree_sitter_ruby();
const TSLanguage* tree_sitter_php();
const TSLanguage* tree_sitter_sql();
}

namespace vectis::code {

namespace {

/// Map a tree-sitter capture name (e.g. "function", "class") to our
/// `SymbolKind`. Returns `Unknown` for unrecognised names — the
/// caller should skip those captures.
[[nodiscard]] SymbolKind capture_name_to_kind(std::string_view capture_name) noexcept
{
    if (capture_name == "function")  { return SymbolKind::Function;  }
    if (capture_name == "method")    { return SymbolKind::Method;    }
    if (capture_name == "class")     { return SymbolKind::Class;     }
    if (capture_name == "struct")    { return SymbolKind::Struct;    }
    if (capture_name == "interface") { return SymbolKind::Interface; }
    if (capture_name == "enum")      { return SymbolKind::Enum;      }
    if (capture_name == "type")      { return SymbolKind::Type;      }
    if (capture_name == "namespace") { return SymbolKind::Namespace; }
    return SymbolKind::Unknown;
}

/// Normalize internal whitespace in a source fragment: collapse runs
/// of spaces / tabs / newlines into a single space and trim the ends.
/// Used to keep multi-line signatures compact in the digest.
[[nodiscard]] std::string normalize_whitespace(std::string_view source)
{
    std::string out;
    out.reserve(source.size());
    bool last_was_space = true;  // leading whitespace becomes nothing
    for (const char ch : source) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!last_was_space) {
                out.push_back(' ');
                last_was_space = true;
            }
        } else {
            out.push_back(ch);
            last_was_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// Extract a human-readable signature for a function-like node.
///
/// For `function_definition`, the signature is everything before the
/// body (so `void Foo::bar(int x) const` without the braces or body).
/// For `declaration`, it's the full declaration minus the trailing
/// semicolon. Whitespace is collapsed and the result trimmed.
///
/// Returns an empty string if the node type is not function-like or
/// if byte offsets fall outside `content`.
[[nodiscard]] std::string extract_signature(TSNode node, std::string_view content)
{
    const std::string_view node_type{ts_node_type(node)};
    const std::uint32_t    node_start = ts_node_start_byte(node);
    std::uint32_t          node_end   = ts_node_end_byte(node);

    if (node_start >= content.size() || node_end > content.size() || node_end <= node_start) {
        return {};
    }

    if (node_type == "function_definition") {
        // Walk named children and stop at the body — either a normal
        // compound_statement or a function-try-block.
        const std::uint32_t child_count = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < child_count; ++i) {
            const TSNode           child      = ts_node_named_child(node, i);
            const std::string_view child_type{ts_node_type(child)};
            if (child_type == "compound_statement" ||
                child_type == "try_statement" ||
                child_type == "field_initializer_list")
            {
                node_end = ts_node_start_byte(child);
                break;
            }
        }
    } else if (node_type == "declaration" || node_type == "field_declaration") {
        // Trim trailing whitespace and the terminating semicolon so the
        // signature doesn't carry a dangling ';'.
        while (node_end > node_start &&
               (content[node_end - 1] == ' ' ||
                content[node_end - 1] == '\t' ||
                content[node_end - 1] == '\n' ||
                content[node_end - 1] == '\r' ||
                content[node_end - 1] == ';'))
        {
            --node_end;
        }
    }

    if (node_end <= node_start) {
        return {};
    }
    return normalize_whitespace(
        content.substr(node_start, node_end - node_start));
}

/// True if a given symbol kind is function-like and should carry a
/// signature string.
[[nodiscard]] constexpr bool kind_has_signature(SymbolKind kind) noexcept
{
    return kind == SymbolKind::Function || kind == SymbolKind::Method;
}

/// True if `node` is (transitively) inside a `compound_statement`,
/// i.e. it lives inside a function body rather than at namespace /
/// translation-unit scope. Used to reject false-positive matches for
/// local constructor-style declarations like
/// `std::scoped_lock lock(m_mutex);` which tree-sitter-cpp
/// represents as a `declaration` with a `function_declarator` child.
[[nodiscard]] bool is_inside_function_body(TSNode node) noexcept
{
    TSNode parent = ts_node_parent(node);
    while (!ts_node_is_null(parent)) {
        const std::string_view type{ts_node_type(parent)};
        if (type == "compound_statement") {
            return true;
        }
        parent = ts_node_parent(parent);
    }
    return false;
}

/// Return the bytes of `node` as a freshly-allocated string, or an
/// empty string if the node's range falls outside `content`.
[[nodiscard]] std::string node_text(TSNode node, std::string_view content)
{
    const std::uint32_t start = ts_node_start_byte(node);
    const std::uint32_t end   = ts_node_end_byte(node);
    if (start > content.size() || end > content.size() || end < start) {
        return {};
    }
    return std::string{content.substr(start, end - start)};
}

/// Walk an `enum_specifier` node and append the name of every
/// enumerator (enum value) to `out`. Tree-sitter-cpp represents these
/// as `enum_specifier > enumerator_list > enumerator > identifier`.
void collect_enum_values(TSNode enum_node, std::string_view content,
                         std::vector<std::string>& out)
{
    const std::uint32_t n = ts_node_named_child_count(enum_node);
    for (std::uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(enum_node, i);
        if (std::string_view{ts_node_type(child)} != "enumerator_list") {
            continue;
        }
        const std::uint32_t en = ts_node_named_child_count(child);
        for (std::uint32_t j = 0; j < en; ++j) {
            const TSNode enumerator = ts_node_named_child(child, j);
            if (std::string_view{ts_node_type(enumerator)} != "enumerator") {
                continue;
            }
            // An enumerator's first named child is its name identifier.
            // Some enumerators have a following expression (for `= 1`);
            // we want only the first identifier.
            if (ts_node_named_child_count(enumerator) == 0) {
                continue;
            }
            const TSNode name_node = ts_node_named_child(enumerator, 0);
            if (std::string_view{ts_node_type(name_node)} == "identifier") {
                std::string name = node_text(name_node, content);
                if (!name.empty()) {
                    out.push_back(std::move(name));
                }
            }
        }
        return;
    }
}

/// Recursively walk a `field_declaration` subtree and append every
/// bare `field_identifier` encountered to `out`. Skips anything
/// nested inside a `function_declarator` (methods — those are
/// reported via the method queries, not as members).
void collect_field_identifiers_rec(TSNode node, std::string_view content,
                                   std::vector<std::string>& out)
{
    const std::string_view type{ts_node_type(node)};
    if (type == "function_declarator") {
        return;
    }
    if (type == "field_identifier") {
        std::string name = node_text(node, content);
        if (!name.empty()) {
            out.push_back(std::move(name));
        }
        return;
    }
    const std::uint32_t n = ts_node_named_child_count(node);
    for (std::uint32_t i = 0; i < n; ++i) {
        collect_field_identifiers_rec(ts_node_named_child(node, i), content, out);
    }
}

/// Walk a `struct_specifier` node and append the name of every
/// data-member field to `out`. Methods are deliberately skipped —
/// they appear as separate symbols via the method query patterns.
void collect_struct_fields(TSNode struct_node, std::string_view content,
                           std::vector<std::string>& out)
{
    const std::uint32_t n = ts_node_named_child_count(struct_node);
    for (std::uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(struct_node, i);
        if (std::string_view{ts_node_type(child)} != "field_declaration_list") {
            continue;
        }
        const std::uint32_t fn = ts_node_named_child_count(child);
        for (std::uint32_t j = 0; j < fn; ++j) {
            const TSNode field = ts_node_named_child(child, j);
            if (std::string_view{ts_node_type(field)} == "field_declaration") {
                collect_field_identifiers_rec(field, content, out);
            }
        }
        return;
    }
}

} // namespace

struct TreeSitterParser::Impl {
    TSParser* parser = nullptr;

    struct LanguageEntry {
        const TSLanguage* grammar          = nullptr;
        TSQuery*          query            = nullptr; ///< symbol query
        TSQuery*          import_query     = nullptr; ///< imports query, may be null
        TSQuery*          namespace_query  = nullptr; ///< namespace-decl query, may be null
    };

    std::unordered_map<Language, LanguageEntry> languages;

    ~Impl()
    {
        for (auto& [_lang, entry] : languages) {
            if (entry.query != nullptr) {
                ts_query_delete(entry.query);
            }
            if (entry.import_query != nullptr) {
                ts_query_delete(entry.import_query);
            }
            if (entry.namespace_query != nullptr) {
                ts_query_delete(entry.namespace_query);
            }
        }
        if (parser != nullptr) {
            ts_parser_delete(parser);
        }
    }

    /// Compile both the symbol query and (optionally) the import
    /// query for one language and store the grammar + queries.
    /// Returns true if the symbol query compiled (the primary
    /// requirement for "supported"). An import-query compile
    /// failure is logged but non-fatal.
    bool register_one(Language lang, const TSLanguage* grammar)
    {
        const std::string_view query_source = query_for(lang);
        if (query_source.empty()) {
            return false;
        }

        std::uint32_t error_offset = 0;
        TSQueryError  error_type   = TSQueryErrorNone;
        TSQuery*      query        = ts_query_new(
            grammar,
            query_source.data(),
            static_cast<std::uint32_t>(query_source.size()),
            &error_offset,
            &error_type);

        if (query == nullptr) {
            VECTIS_LOG_WARN(
                "failed to compile tree-sitter query for {} (error {}, offset {})",
                language_name(lang),
                static_cast<int>(error_type),
                error_offset);
            return false;
        }

        // Import query is optional — if it fails or is empty, the
        // language is still "supported" for symbols; it just won't
        // contribute to the dependency graph.
        TSQuery*               import_query       = nullptr;
        const std::string_view import_query_source = import_query_for(lang);
        if (!import_query_source.empty()) {
            std::uint32_t import_error_offset = 0;
            TSQueryError  import_error_type   = TSQueryErrorNone;
            import_query = ts_query_new(
                grammar,
                import_query_source.data(),
                static_cast<std::uint32_t>(import_query_source.size()),
                &import_error_offset,
                &import_error_type);
            if (import_query == nullptr) {
                VECTIS_LOG_WARN(
                    "failed to compile import query for {} (error {}, offset {})",
                    language_name(lang),
                    static_cast<int>(import_error_type),
                    import_error_offset);
            }
        }

        // Namespace-declaration query — also optional. Currently only
        // C# and PHP populate this; see `namespace_query_for`.
        TSQuery*               namespace_query        = nullptr;
        const std::string_view namespace_query_source = namespace_query_for(lang);
        if (!namespace_query_source.empty()) {
            std::uint32_t ns_error_offset = 0;
            TSQueryError  ns_error_type   = TSQueryErrorNone;
            namespace_query = ts_query_new(
                grammar,
                namespace_query_source.data(),
                static_cast<std::uint32_t>(namespace_query_source.size()),
                &ns_error_offset,
                &ns_error_type);
            if (namespace_query == nullptr) {
                VECTIS_LOG_WARN(
                    "failed to compile namespace query for {} (error {}, offset {})",
                    language_name(lang),
                    static_cast<int>(ns_error_type),
                    ns_error_offset);
            }
        }

        languages[lang] = LanguageEntry{
            grammar, query, import_query, namespace_query};
        return true;
    }
};

TreeSitterParser::TreeSitterParser() : m_impl(std::make_unique<Impl>())
{
    m_impl->parser = ts_parser_new();
}

TreeSitterParser::~TreeSitterParser() = default;

std::size_t TreeSitterParser::register_builtin_languages()
{
    if (!m_impl->languages.empty()) {
        return m_impl->languages.size();
    }

    struct Entry {
        Language          lang;
        const TSLanguage* grammar;
    };
    const std::array<Entry, 12> builtins = {{
        {Language::Python,     tree_sitter_python()},
        {Language::JavaScript, tree_sitter_javascript()},
        {Language::TypeScript, tree_sitter_typescript()},
        {Language::C,          tree_sitter_c()},
        {Language::Cpp,        tree_sitter_cpp()},
        {Language::Rust,       tree_sitter_rust()},
        {Language::Java,       tree_sitter_java()},
        {Language::CSharp,     tree_sitter_c_sharp()},
        {Language::Go,         tree_sitter_go()},
        {Language::Ruby,       tree_sitter_ruby()},
        {Language::Php,        tree_sitter_php()},
        {Language::Sql,        tree_sitter_sql()},
    }};

    std::size_t registered = 0;
    for (const auto& entry : builtins) {
        if (m_impl->register_one(entry.lang, entry.grammar)) {
            ++registered;
        }
    }
    return registered;
}

bool TreeSitterParser::supports(Language language) const noexcept
{
    return m_impl->languages.find(language) != m_impl->languages.end();
}

TreeSitterParser::ParseResult
TreeSitterParser::parse_file(Language language, std::string_view content)
{
    ParseResult result;

    if (language == Language::Unknown || content.empty()) {
        return result;
    }

    const auto lang_it = m_impl->languages.find(language);
    if (lang_it == m_impl->languages.end()) {
        return result;
    }

    const Impl::LanguageEntry& entry = lang_it->second;

    if (!ts_parser_set_language(m_impl->parser, entry.grammar)) {
        VECTIS_LOG_WARN("ts_parser_set_language failed for {}", language_name(language));
        return result;
    }

    TSTree* tree = ts_parser_parse_string(
        m_impl->parser,
        nullptr,
        content.data(),
        static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        VECTIS_LOG_WARN("ts_parser_parse_string returned null for {}", language_name(language));
        return result;
    }

    const TSNode   root   = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        Symbol symbol;
        bool   has_name = false;
        bool   has_kind = false;
        bool   skip_this_match = false;  // set for local-scope false positives

        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture       = match.captures[i];
            std::uint32_t         capture_len   = 0;
            const char*           capture_chars = ts_query_capture_name_for_id(
                entry.query, capture.index, &capture_len);
            const std::string_view capture_name(capture_chars, capture_len);

            if (capture_name == "name") {
                const std::uint32_t start = ts_node_start_byte(capture.node);
                const std::uint32_t end   = ts_node_end_byte(capture.node);
                if (end <= content.size() && start <= end) {
                    symbol.name.assign(content.data() + start, end - start);
                    has_name = true;
                }
            } else {
                const SymbolKind kind = capture_name_to_kind(capture_name);
                if (kind != SymbolKind::Unknown) {
                    symbol.kind = kind;
                    has_kind = true;
                    const TSPoint start_pt = ts_node_start_point(capture.node);
                    const TSPoint end_pt   = ts_node_end_point(capture.node);
                    symbol.line_start = static_cast<int>(start_pt.row) + 1;
                    symbol.line_end   = static_cast<int>(end_pt.row) + 1;

                    // Reject local-scope declarations that look like
                    // function declarations to tree-sitter but are
                    // actually constructor-style variable decls (e.g.
                    // `std::scoped_lock lock(m_mutex);` inside a
                    // function body). These only affect the C/C++
                    // `declaration` → `function` patterns.
                    if (kind == SymbolKind::Function &&
                        std::string_view{ts_node_type(capture.node)} == "declaration" &&
                        is_inside_function_body(capture.node))
                    {
                        skip_this_match = true;
                    }

                    if (kind_has_signature(kind)) {
                        symbol.signature = extract_signature(capture.node, content);
                        symbol.complexity = compute_cyclomatic_complexity(
                            capture.node, language);
                    }
                    if (kind == SymbolKind::Enum) {
                        collect_enum_values(capture.node, content, symbol.members);
                    } else if (kind == SymbolKind::Struct) {
                        collect_struct_fields(capture.node, content, symbol.members);
                    }
                }
            }
        }

        if (has_name && has_kind && !symbol.name.empty() && !skip_this_match) {
            result.symbols.push_back(std::move(symbol));
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);

    return result;
}

namespace {

/// Strip one leading and one trailing character from `text` if both
/// are the given quote character. Used to peel the surrounding quotes
/// off tree-sitter `string_literal` captures (e.g. `"core/log.h"` ->
/// `core/log.h`).
[[nodiscard]] std::string_view unquote(std::string_view text, char quote) noexcept
{
    if (text.size() >= 2 && text.front() == quote && text.back() == quote) {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

} // namespace

std::vector<RawImport>
TreeSitterParser::extract_imports(Language language, std::string_view content)
{
    std::vector<RawImport> result;

    if (language == Language::Unknown || content.empty()) {
        return result;
    }

    const auto lang_it = m_impl->languages.find(language);
    if (lang_it == m_impl->languages.end()) {
        return result;
    }
    const Impl::LanguageEntry& entry = lang_it->second;
    if (entry.import_query == nullptr) {
        return result;
    }

    if (!ts_parser_set_language(m_impl->parser, entry.grammar)) {
        return result;
    }

    TSTree* tree = ts_parser_parse_string(
        m_impl->parser, nullptr, content.data(),
        static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        return result;
    }

    const TSNode   root   = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.import_query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        RawImport       raw;
        bool            has_path = false;
        bool            has_kind = false;

        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture       = match.captures[i];
            std::uint32_t         capture_len   = 0;
            const char*           capture_chars = ts_query_capture_name_for_id(
                entry.import_query, capture.index, &capture_len);
            const std::string_view capture_name(capture_chars, capture_len);

            if (capture_name == "path") {
                const std::uint32_t start = ts_node_start_byte(capture.node);
                const std::uint32_t end   = ts_node_end_byte(capture.node);
                if (end > content.size() || start > end) {
                    continue;
                }
                std::string_view path_text = content.substr(start, end - start);
                path_text = unquote(path_text, '"');
                path_text = unquote(path_text, '\'');
                raw.import_string.assign(path_text);
                has_path = true;
            } else if (!capture_name.empty() && capture_name.front() == '_') {
                // Convention: `@_xxx` captures exist only to feed
                // `#match?`/`#eq?` predicates (e.g. the Ruby method-name
                // check). They must not be confused with the kind tag.
                continue;
            } else {
                // `@include`, `@import`, `@use`, `@require`, `@mod`
                // are treated as the kind tag — whichever fires is
                // stored verbatim.
                raw.kind.assign(capture_name);
                has_kind = true;
                const TSPoint start_pt = ts_node_start_point(capture.node);
                raw.line = static_cast<int>(start_pt.row) + 1;
            }
        }

        if (has_path && has_kind && !raw.import_string.empty()) {
            result.push_back(std::move(raw));
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    return result;
}

std::vector<std::string>
TreeSitterParser::extract_namespaces(Language language, std::string_view content)
{
    std::vector<std::string> result;

    if (language == Language::Unknown || content.empty()) {
        return result;
    }
    const auto lang_it = m_impl->languages.find(language);
    if (lang_it == m_impl->languages.end()) {
        return result;
    }
    const Impl::LanguageEntry& entry = lang_it->second;
    if (entry.namespace_query == nullptr) {
        return result;
    }

    if (!ts_parser_set_language(m_impl->parser, entry.grammar)) {
        return result;
    }
    TSTree* tree = ts_parser_parse_string(
        m_impl->parser, nullptr, content.data(),
        static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        return result;
    }

    const TSNode   root   = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.namespace_query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture     = match.captures[i];
            std::uint32_t         name_len    = 0;
            const char*           name_chars  = ts_query_capture_name_for_id(
                entry.namespace_query, capture.index, &name_len);
            const std::string_view capture_name(name_chars, name_len);
            if (capture_name != "name") {
                continue;
            }
            const std::uint32_t start = ts_node_start_byte(capture.node);
            const std::uint32_t end   = ts_node_end_byte(capture.node);
            if (end > content.size() || start > end) {
                continue;
            }
            std::string ns{content.substr(start, end - start)};
            // Strip stray whitespace from the captured span.
            while (!ns.empty() &&
                   std::isspace(static_cast<unsigned char>(ns.back())))
            {
                ns.pop_back();
            }
            if (!ns.empty()) {
                result.push_back(std::move(ns));
            }
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    return result;
}

} // namespace vectis::code
