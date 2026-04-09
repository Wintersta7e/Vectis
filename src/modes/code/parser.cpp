#include "modes/code/parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tree_sitter/api.h>

#include "core/log.h"
#include "modes/code/language.h"
#include "modes/code/parser_queries.h"
#include "modes/code/symbol.h"

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

namespace vectis::modes::code {

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

} // namespace

struct TreeSitterParser::Impl {
    TSParser* parser = nullptr;

    struct LanguageEntry {
        const TSLanguage* grammar = nullptr;
        TSQuery*          query   = nullptr;
    };

    std::unordered_map<Language, LanguageEntry> languages;

    ~Impl()
    {
        for (auto& [_lang, entry] : languages) {
            if (entry.query != nullptr) {
                ts_query_delete(entry.query);
            }
        }
        if (parser != nullptr) {
            ts_parser_delete(parser);
        }
    }

    /// Compile the query for one language and store the grammar+query
    /// pair. Returns true on success; on failure logs a WARN and
    /// leaves the language unregistered so `supports()` returns false.
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

        languages[lang] = LanguageEntry{grammar, query};
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

                    // Capture the node for post-match processing —
                    // we can't call extract_signature yet because we
                    // may not have seen the @name capture.
                    if (kind_has_signature(kind)) {
                        symbol.signature = extract_signature(capture.node, content);
                    }
                }
            }
        }

        if (has_name && has_kind && !symbol.name.empty()) {
            result.symbols.push_back(std::move(symbol));
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);

    return result;
}

} // namespace vectis::modes::code
