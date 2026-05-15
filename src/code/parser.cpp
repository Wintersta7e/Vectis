#include "code/parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tree_sitter/api.h>

#include "code/complexity_analyzer.h"
#include "code/language.h"
#include "code/parser_queries.h"
#include "code/symbol.h"
#include "core/log.h"

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
    if (capture_name == "function") {
        return SymbolKind::Function;
    }
    if (capture_name == "method") {
        return SymbolKind::Method;
    }
    if (capture_name == "class") {
        return SymbolKind::Class;
    }
    if (capture_name == "struct") {
        return SymbolKind::Struct;
    }
    if (capture_name == "interface") {
        return SymbolKind::Interface;
    }
    if (capture_name == "enum") {
        return SymbolKind::Enum;
    }
    if (capture_name == "type") {
        return SymbolKind::Type;
    }
    if (capture_name == "namespace") {
        return SymbolKind::Namespace;
    }
    return SymbolKind::Unknown;
}

/// Normalize internal whitespace in a source fragment: collapse runs
/// of spaces / tabs / newlines into a single space and trim the ends.
/// Used to keep multi-line signatures compact in the digest.
[[nodiscard]] std::string normalize_whitespace(std::string_view source)
{
    std::string out;
    out.reserve(source.size());
    bool last_was_space = true; // leading whitespace becomes nothing
    for (const char ch : source) {
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!last_was_space) {
                out.push_back(' ');
                last_was_space = true;
            }
        }
        else {
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
    const std::uint32_t node_start = ts_node_start_byte(node);
    std::uint32_t node_end = ts_node_end_byte(node);

    if (node_start >= content.size() || node_end > content.size() || node_end <= node_start) {
        return {};
    }

    if (node_type == "function_definition") {
        // Walk named children and stop at the body — either a normal
        // compound_statement or a function-try-block.
        const std::uint32_t child_count = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < child_count; ++i) {
            const TSNode child = ts_node_named_child(node, i);
            const std::string_view child_type{ts_node_type(child)};
            if (child_type == "compound_statement" || child_type == "try_statement" ||
                child_type == "field_initializer_list") {
                node_end = ts_node_start_byte(child);
                break;
            }
        }
    }
    else if (node_type == "declaration" || node_type == "field_declaration") {
        // Trim trailing whitespace and the terminating semicolon so the
        // signature doesn't carry a dangling ';'.
        while (node_end > node_start &&
               (content[node_end - 1] == ' ' || content[node_end - 1] == '\t' ||
                content[node_end - 1] == '\n' || content[node_end - 1] == '\r' ||
                content[node_end - 1] == ';')) {
            --node_end;
        }
    }

    if (node_end <= node_start) {
        return {};
    }
    return normalize_whitespace(content.substr(node_start, node_end - node_start));
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
    const std::uint32_t end = ts_node_end_byte(node);
    if (start > content.size() || end > content.size() || end < start) {
        return {};
    }
    return std::string{content.substr(start, end - start)};
}

/// Walk `parent`'s named children and return the first one whose type
/// matches `type_name`. Returns a null TSNode (test with
/// `ts_node_is_null`) if no child matches. Tree-sitter grammars often
/// follow a "outer node has at most one list-typed child" shape, so
/// "find first" is the natural primitive.
[[nodiscard]] TSNode first_named_child_of_type(TSNode parent, std::string_view type_name)
{
    const std::uint32_t n = ts_node_named_child_count(parent);
    for (std::uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(parent, i);
        if (std::string_view{ts_node_type(child)} == type_name) {
            return child;
        }
    }
    return TSNode{};
}

/// Invoke `fn(child)` for each named child of `parent` whose type
/// matches `type_name`, in source order. The visitor pattern keeps
/// callers free of the index/count boilerplate that tree-sitter's C
/// API requires.
///
/// `Fn` is taken by value because the helper invokes the callable
/// repeatedly inside the loop — a forwarding reference would risk a
/// move-from-rvalue on the second iteration, and the by-value copy is
/// cheap for the lambdas we pass at every call site.
template <typename Fn>
void for_each_named_child_of_type(TSNode parent, std::string_view type_name, Fn fn)
{
    const std::uint32_t n = ts_node_named_child_count(parent);
    for (std::uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(parent, i);
        if (std::string_view{ts_node_type(child)} == type_name) {
            fn(child);
        }
    }
}

/// Walk an `enum_specifier` node and append the name of every
/// enumerator (enum value) to `out`. Tree-sitter-cpp represents these
/// as `enum_specifier > enumerator_list > enumerator > identifier`.
void collect_enum_values(TSNode enum_node, std::string_view content, std::vector<std::string>& out)
{
    const TSNode list = first_named_child_of_type(enum_node, "enumerator_list");
    if (ts_node_is_null(list)) {
        return;
    }
    for_each_named_child_of_type(list, "enumerator", [&](TSNode enumerator) {
        // An enumerator's first named child is its name identifier.
        // Some enumerators have a following expression (for `= 1`); we
        // want only the first identifier.
        if (ts_node_named_child_count(enumerator) == 0) {
            return;
        }
        const TSNode name_node = ts_node_named_child(enumerator, 0);
        if (std::string_view{ts_node_type(name_node)} != "identifier") {
            return;
        }
        std::string name = node_text(name_node, content);
        if (!name.empty()) {
            out.push_back(std::move(name));
        }
    });
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
    const TSNode list = first_named_child_of_type(struct_node, "field_declaration_list");
    if (ts_node_is_null(list)) {
        return;
    }
    for_each_named_child_of_type(list, "field_declaration", [&](TSNode field) {
        collect_field_identifiers_rec(field, content, out);
    });
}

/// Compute the visibility/API-surface category for a symbol. Languages
/// vary widely in how they encode this; we collapse each into the
/// `Visibility` enum documented on `Symbol::visibility`. `Unknown`
/// means "language doesn't express visibility / not yet implemented" —
/// consumers should treat that as Public when filtering.
/// Extract Java / C# / TypeScript visibility by walking word-token
/// modifiers (`public`, `private`, `protected`, `internal`). Uses raw
/// `ts_node_child` because modifier keyword leaves are anonymous
/// tokens in the tree-sitter grammars. Returns `Visibility::Unknown`
/// if no explicit modifier was found — caller picks the per-language
/// default.
[[nodiscard]] Visibility extract_modifier_visibility(TSNode node)
{
    const std::uint32_t child_count = ts_node_child_count(node);
    for (std::uint32_t i = 0; i < child_count; ++i) {
        const TSNode child = ts_node_child(node, i);
        const std::string_view type{ts_node_type(child)};
        if (type != "modifiers" && type != "modifier" && type != "accessibility_modifier") {
            continue;
        }
        const std::uint32_t mod_count = ts_node_child_count(child);
        for (std::uint32_t j = 0; j < mod_count; ++j) {
            const TSNode mod = ts_node_child(child, j);
            const std::string_view mt{ts_node_type(mod)};
            if (mt == "public") {
                return Visibility::Public;
            }
            if (mt == "private") {
                return Visibility::Private;
            }
            if (mt == "protected") {
                return Visibility::Protected;
            }
            if (mt == "internal") {
                return Visibility::Internal;
            }
        }
    }
    return Visibility::Unknown;
}

[[nodiscard]] Visibility extract_visibility(TSNode node, Language language, std::string_view name)
{
    switch (language) {
    case Language::Go:
        // Go encodes visibility purely in the symbol name: an uppercase
        // first letter exports from the package; lowercase keeps it
        // package-private. The convention is compiler-enforced.
        if (name.empty()) {
            return Visibility::Unknown;
        }
        return std::isupper(static_cast<unsigned char>(name[0])) != 0 ? Visibility::Public
                                                                      : Visibility::Private;

    case Language::Python: {
        // Leading underscore = internal; dunder names (`__init__`,
        // `__name__`) are stdlib metadata and public by intention.
        if (name.empty()) {
            return Visibility::Unknown;
        }
        const bool is_dunder = name.size() >= 4 && name.starts_with("__") && name.ends_with("__");
        if (is_dunder) {
            return Visibility::Public;
        }
        return name.front() == '_' ? Visibility::Private : Visibility::Public;
    }

    case Language::Rust: {
        // The grammar names the visibility modifier as
        //   (visibility_modifier "pub")           — fully public
        //   (visibility_modifier "pub" "(crate)") — pub(crate) → Internal
        //   (visibility_modifier "pub" "(super)") — pub(super) → Internal
        // Absence means private (Rust default).
        const TSNode modifier = first_named_child_of_type(node, "visibility_modifier");
        if (ts_node_is_null(modifier)) {
            return Visibility::Private;
        }
        const std::uint32_t qual_count = ts_node_child_count(modifier);
        for (std::uint32_t j = 0; j < qual_count; ++j) {
            const TSNode qual = ts_node_child(modifier, j);
            if (std::string_view{ts_node_type(qual)} == "(") {
                return Visibility::Internal;
            }
        }
        return Visibility::Public;
    }

    case Language::Java:
    case Language::CSharp:
    case Language::TypeScript: {
        // Java/C#/TS all use word-token modifiers; defer to the helper
        // and apply per-language defaults when no modifier is present.
        const Visibility found = extract_modifier_visibility(node);
        if (found != Visibility::Unknown) {
            return found;
        }
        // Per-language defaults when no modifier was found:
        //   Java: package-private (reported as Internal).
        //   C#:   `internal` for top-level types, `private` for class
        //         members — we collapse both to Internal since we
        //         don't have parent-kind context in this helper, and
        //         "Internal" is closer to the truth than "Public".
        //   TypeScript: members default to public.
        if (language == Language::TypeScript) {
            return Visibility::Public;
        }
        return Visibility::Internal;
    }

    default:
        return Visibility::Unknown;
    }
}

/// Extract decorator/annotation text attached to a symbol. Currently
/// supports Python `@decorator` syntax (the most common case in real
/// codebases) plus Java annotations, C# attributes, and Rust outer
/// attributes — the four common shapes where each language wraps the
/// declaration in a parent node containing the markers as siblings or
/// attaches them as preceding sibling children.
///
/// Output: marker text in source order with the language's leading
/// sigil stripped (e.g. `app.route("/")` not `@app.route("/")`,
/// `RestController` not `@RestController`, `derive(Debug)` not
/// `#[derive(Debug)]`, `HttpGet` not `[HttpGet]`).
/// Extract the source bytes of `marker`, trim outer whitespace, then
/// strip the language sigil (`leading` and optional `trailing`) — e.g.
/// `"@"` for Python/Java/TS decorators, `"#["` + `"]"` for Rust outer
/// attributes, both empty for C# attribute children that already lack
/// the `[ ]`. Returns an empty string if the byte range is invalid.
[[nodiscard]] std::string extract_marker_text(TSNode marker, std::string_view leading,
                                              std::string_view trailing, std::string_view content)
{
    const std::uint32_t start = ts_node_start_byte(marker);
    const std::uint32_t end = ts_node_end_byte(marker);
    if (start >= end || end > content.size()) {
        return {};
    }
    std::string_view text(content.data() + start, end - start);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\t')) {
        text.remove_suffix(1);
    }
    if (!leading.empty() && text.size() >= leading.size() && text.starts_with(leading)) {
        text.remove_prefix(leading.size());
    }
    if (!trailing.empty() && text.size() >= trailing.size() && text.ends_with(trailing)) {
        text.remove_suffix(trailing.size());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return std::string{text};
}

/// Append every named child of `parent` whose type matches `child_type`
/// to `out` after running it through `extract_marker_text`. Empty
/// results are skipped so callers don't have to filter.
void collect_marker_children(TSNode parent, std::string_view child_type, std::string_view leading,
                             std::string_view trailing, std::string_view content,
                             std::vector<std::string>& out)
{
    for_each_named_child_of_type(parent, child_type, [&](TSNode child) {
        std::string text = extract_marker_text(child, leading, trailing, content);
        if (!text.empty()) {
            out.emplace_back(std::move(text));
        }
    });
}

/// Walk preceding named siblings of `start` while their type matches
/// `sibling_type`, collecting each via `extract_marker_text`. The walk
/// is in source-reverse order — the vector is reversed before return so
/// callers see source order. Used for Rust outer attributes
/// (`#[...]` placed before a declaration) and TypeScript decorators
/// attached as siblings of `class_declaration`.
[[nodiscard]] std::vector<std::string>
collect_marker_siblings_back(TSNode start, std::string_view sibling_type, std::string_view leading,
                             std::string_view trailing, std::string_view content)
{
    std::vector<std::string> out;
    TSNode cur = ts_node_prev_named_sibling(start);
    while (!ts_node_is_null(cur) && std::string_view{ts_node_type(cur)} == sibling_type) {
        std::string text = extract_marker_text(cur, leading, trailing, content);
        if (!text.empty()) {
            out.emplace_back(std::move(text));
        }
        cur = ts_node_prev_named_sibling(cur);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// Java annotations: `marker_annotation` and `annotation` nodes inside
/// a `modifiers` child. Source order across the two types matters so we
/// walk the list once and filter both kinds.
[[nodiscard]] std::vector<std::string> extract_java_annotations(TSNode node,
                                                                std::string_view content)
{
    std::vector<std::string> out;
    const TSNode modifiers = first_named_child_of_type(node, "modifiers");
    if (ts_node_is_null(modifiers)) {
        return out;
    }
    const std::uint32_t mod_count = ts_node_named_child_count(modifiers);
    for (std::uint32_t j = 0; j < mod_count; ++j) {
        const TSNode mod = ts_node_named_child(modifiers, j);
        const std::string_view mt{ts_node_type(mod)};
        if (mt != "marker_annotation" && mt != "annotation") {
            continue;
        }
        std::string text = extract_marker_text(mod, "@", {}, content);
        if (!text.empty()) {
            out.emplace_back(std::move(text));
        }
    }
    return out;
}

/// TypeScript decorators attach two ways: as children of the captured
/// declaration (method_definition, public_field_definition,
/// abstract_method_signature) or as preceding siblings (class_declaration,
/// abstract_class_declaration). Try children first; fall back to
/// walking siblings.
[[nodiscard]] std::vector<std::string> extract_typescript_decorators(TSNode node,
                                                                     std::string_view content)
{
    std::vector<std::string> out;
    collect_marker_children(node, "decorator", "@", {}, content, out);
    if (!out.empty()) {
        return out;
    }
    return collect_marker_siblings_back(node, "decorator", "@", {}, content);
}

[[nodiscard]] std::vector<std::string> extract_decorators(TSNode node, Language language,
                                                          std::string_view content)
{
    switch (language) {
    case Language::Python: {
        // tree-sitter-python wraps a decorated def/class in a
        // `decorated_definition` parent containing each `decorator` sibling.
        const TSNode parent = ts_node_parent(node);
        if (ts_node_is_null(parent) ||
            std::string_view{ts_node_type(parent)} != "decorated_definition") {
            return {};
        }
        std::vector<std::string> out;
        collect_marker_children(parent, "decorator", "@", {}, content, out);
        return out;
    }
    case Language::Java:
        return extract_java_annotations(node, content);
    case Language::CSharp: {
        // C# attributes group inside `attribute_list` children
        // (`[A, B]`); each `attribute` carries the bare name + args.
        std::vector<std::string> out;
        for_each_named_child_of_type(node, "attribute_list", [&](TSNode list) {
            collect_marker_children(list, "attribute", {}, {}, content, out);
        });
        return out;
    }
    case Language::Rust:
        // Outer `#[...]` attributes are preceding siblings of the
        // declaration. Walking back from `node` is O(attributes-here);
        // a forward parent walk would be O(siblings-of-parent).
        return collect_marker_siblings_back(node, "attribute_item", "#[", "]", content);
    case Language::TypeScript:
        return extract_typescript_decorators(node, content);
    default:
        return {};
    }
}

} // namespace

struct TreeSitterParser::Impl
{
    TSParser* parser = nullptr;

    struct LanguageEntry
    {
        const TSLanguage* grammar = nullptr;
        TSQuery* query = nullptr;           ///< symbol query
        TSQuery* import_query = nullptr;    ///< imports query, may be null
        TSQuery* namespace_query = nullptr; ///< namespace-decl query, may be null
    };

    std::unordered_map<Language, LanguageEntry> languages;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

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
        TSQueryError error_type = TSQueryErrorNone;
        TSQuery* query = ts_query_new(grammar, query_source.data(),
                                      static_cast<std::uint32_t>(query_source.size()),
                                      &error_offset, &error_type);

        if (query == nullptr) {
            VECTIS_LOG_WARN("failed to compile tree-sitter query for {} (error {}, offset {})",
                            language_name(lang), static_cast<int>(error_type), error_offset);
            return false;
        }

        // Import query is optional — if it fails or is empty, the
        // language is still "supported" for symbols; it just won't
        // contribute to the dependency graph.
        TSQuery* import_query = nullptr;
        const std::string_view import_query_source = import_query_for(lang);
        if (!import_query_source.empty()) {
            std::uint32_t import_error_offset = 0;
            TSQueryError import_error_type = TSQueryErrorNone;
            import_query = ts_query_new(grammar, import_query_source.data(),
                                        static_cast<std::uint32_t>(import_query_source.size()),
                                        &import_error_offset, &import_error_type);
            if (import_query == nullptr) {
                VECTIS_LOG_WARN("failed to compile import query for {} (error {}, offset {})",
                                language_name(lang), static_cast<int>(import_error_type),
                                import_error_offset);
            }
        }

        // Namespace-declaration query — also optional. Currently only
        // C# and PHP populate this; see `namespace_query_for`.
        TSQuery* namespace_query = nullptr;
        const std::string_view namespace_query_source = namespace_query_for(lang);
        if (!namespace_query_source.empty()) {
            std::uint32_t ns_error_offset = 0;
            TSQueryError ns_error_type = TSQueryErrorNone;
            namespace_query =
                ts_query_new(grammar, namespace_query_source.data(),
                             static_cast<std::uint32_t>(namespace_query_source.size()),
                             &ns_error_offset, &ns_error_type);
            if (namespace_query == nullptr) {
                VECTIS_LOG_WARN("failed to compile namespace query for {} (error {}, offset {})",
                                language_name(lang), static_cast<int>(ns_error_type),
                                ns_error_offset);
            }
        }

        languages[lang] = LanguageEntry{grammar, query, import_query, namespace_query};
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

    struct Entry
    {
        Language lang;
        const TSLanguage* grammar;
    };
    const std::array<Entry, 12> builtins = {{
        {Language::Python, tree_sitter_python()},
        {Language::JavaScript, tree_sitter_javascript()},
        {Language::TypeScript, tree_sitter_typescript()},
        {Language::C, tree_sitter_c()},
        {Language::Cpp, tree_sitter_cpp()},
        {Language::Rust, tree_sitter_rust()},
        {Language::Java, tree_sitter_java()},
        {Language::CSharp, tree_sitter_c_sharp()},
        {Language::Go, tree_sitter_go()},
        {Language::Ruby, tree_sitter_ruby()},
        {Language::Php, tree_sitter_php()},
        {Language::Sql, tree_sitter_sql()},
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

TreeSitterParser::ParseResult TreeSitterParser::parse_file(Language language,
                                                           std::string_view content)
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

    TSTree* tree = ts_parser_parse_string(m_impl->parser, nullptr, content.data(),
                                          static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        VECTIS_LOG_WARN("ts_parser_parse_string returned null for {}", language_name(language));
        return result;
    }

    const TSNode root = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        Symbol symbol;
        TSNode kind_node{}; // outer declaration node for visibility/decorators
        bool has_name = false;
        bool has_kind = false;
        bool skip_this_match = false; // set for local-scope false positives

        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            std::uint32_t capture_len = 0;
            const char* capture_chars =
                ts_query_capture_name_for_id(entry.query, capture.index, &capture_len);
            const std::string_view capture_name(capture_chars, capture_len);

            if (capture_name == "name") {
                const std::uint32_t start = ts_node_start_byte(capture.node);
                const std::uint32_t end = ts_node_end_byte(capture.node);
                if (end <= content.size() && start <= end) {
                    symbol.name.assign(content.data() + start, end - start);
                    has_name = true;
                }
            }
            else {
                const SymbolKind kind = capture_name_to_kind(capture_name);
                if (kind != SymbolKind::Unknown) {
                    symbol.kind = kind;
                    has_kind = true;
                    kind_node = capture.node;
                    const TSPoint start_pt = ts_node_start_point(capture.node);
                    const TSPoint end_pt = ts_node_end_point(capture.node);
                    symbol.line_start = static_cast<int>(start_pt.row) + 1;
                    symbol.line_end = static_cast<int>(end_pt.row) + 1;

                    // Reject local-scope declarations that look like
                    // function declarations to tree-sitter but are
                    // actually constructor-style variable decls (e.g.
                    // `std::scoped_lock lock(m_mutex);` inside a
                    // function body). These only affect the C/C++
                    // `declaration` → `function` patterns.
                    if (kind == SymbolKind::Function &&
                        std::string_view{ts_node_type(capture.node)} == "declaration" &&
                        is_inside_function_body(capture.node)) {
                        skip_this_match = true;
                    }

                    if (kind_has_signature(kind)) {
                        symbol.signature = extract_signature(capture.node, content);
                        symbol.complexity = compute_cyclomatic_complexity(capture.node, language);
                    }
                    if (kind == SymbolKind::Enum) {
                        collect_enum_values(capture.node, content, symbol.members);
                    }
                    else if (kind == SymbolKind::Struct) {
                        collect_struct_fields(capture.node, content, symbol.members);
                    }
                }
            }
        }

        if (has_name && has_kind && !symbol.name.empty() && !skip_this_match) {
            symbol.visibility = extract_visibility(kind_node, language, symbol.name);
            symbol.decorators = extract_decorators(kind_node, language, content);
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

/// Return a compiled regex for `pattern`, sharing one std::regex per
/// distinct pattern across all `match?` / `not-match?` invocations on
/// this thread. Tree-sitter query strings are stable for the query's
/// lifetime and `std::regex` ECMAScript construction costs ~5-20 µs,
/// so caching matters on Ruby corpora where a single `^require…` regex
/// would otherwise recompile on every `call`-with-string-literal node.
/// Returns nullptr if the pattern is malformed — cached as a failure
/// so a broken pattern doesn't repeatedly throw.
[[nodiscard]] const std::regex* compiled_regex_for(std::string_view pattern)
{
    thread_local std::unordered_map<std::string_view, std::unique_ptr<std::regex>> cache;
    if (const auto it = cache.find(pattern); it != cache.end()) {
        return it->second.get();
    }
    std::unique_ptr<std::regex> compiled;
    try {
        compiled = std::make_unique<std::regex>(pattern.begin(), pattern.end(),
                                                std::regex::ECMAScript | std::regex::optimize);
    }
    catch (const std::regex_error& e) {
        VECTIS_LOG_WARN("predicate regex compile failed: pattern='{}' error='{}'", pattern,
                        e.what());
    }
    const auto it = cache.emplace(pattern, std::move(compiled)).first;
    return it->second.get();
}

/// Evaluate tree-sitter `#eq?` / `#not-eq?` / `#match?` / `#not-match?`
/// predicates against a single match. Tree-sitter's C runtime returns
/// every structural match unfiltered — predicate steps are advisory and
/// the host has to test them. Without this filter the JS/TS
/// `(#eq? @_func "require")` constraint would let any single-identifier
/// call-with-string-literal (e.g. `fetch("https://…")`, `mock("x")`)
/// through as a `require()` import; same story for Ruby's
/// `(#match? @_m "^require(_relative)?$")`.
///
/// Unknown predicate names (e.g. `set!`, `any-of?`) are treated as
/// satisfied so we don't accidentally drop matches when a query gains
/// a meta-predicate.
[[nodiscard]] bool match_satisfies_predicates(const TSQuery* query, const TSQueryMatch& match,
                                              std::string_view content)
{
    std::uint32_t step_count = 0;
    const TSQueryPredicateStep* steps =
        ts_query_predicates_for_pattern(query, match.pattern_index, &step_count);
    if (step_count == 0 || steps == nullptr) {
        return true;
    }

    const auto step_text =
        [&](const TSQueryPredicateStep& step) noexcept -> std::optional<std::string_view> {
        if (step.type == TSQueryPredicateStepTypeString) {
            std::uint32_t len = 0;
            const char* chars = ts_query_string_value_for_id(query, step.value_id, &len);
            return std::string_view{chars, len};
        }
        if (step.type == TSQueryPredicateStepTypeCapture) {
            for (std::uint16_t i = 0; i < match.capture_count; ++i) {
                if (match.captures[i].index != step.value_id) {
                    continue;
                }
                const std::uint32_t start = ts_node_start_byte(match.captures[i].node);
                const std::uint32_t end = ts_node_end_byte(match.captures[i].node);
                if (start > end || end > content.size()) {
                    return std::nullopt;
                }
                return content.substr(start, end - start);
            }
            return std::nullopt;
        }
        return std::nullopt;
    };

    std::uint32_t i = 0;
    while (i < step_count) {
        std::uint32_t end = i;
        while (end < step_count && steps[end].type != TSQueryPredicateStepTypeDone) {
            ++end;
        }
        const std::uint32_t arity = end - i;
        if (arity == 0 || steps[i].type != TSQueryPredicateStepTypeString) {
            i = end + 1;
            continue;
        }
        std::uint32_t name_len = 0;
        const char* name_chars = ts_query_string_value_for_id(query, steps[i].value_id, &name_len);
        const std::string_view name{name_chars, name_len};

        if ((name == "eq?" || name == "not-eq?") && arity == 3) {
            const auto a = step_text(steps[i + 1]);
            const auto b = step_text(steps[i + 2]);
            if (!a || !b) {
                return false;
            }
            const bool equal = (*a == *b);
            if ((name == "eq?" && !equal) || (name == "not-eq?" && equal)) {
                return false;
            }
        }
        else if ((name == "match?" || name == "not-match?") && arity == 3 &&
                 steps[i + 2].type == TSQueryPredicateStepTypeString) {
            const auto subject = step_text(steps[i + 1]);
            if (!subject) {
                return false;
            }
            std::uint32_t pat_len = 0;
            const char* pat_chars =
                ts_query_string_value_for_id(query, steps[i + 2].value_id, &pat_len);
            const std::regex* re = compiled_regex_for({pat_chars, pat_len});
            if (re == nullptr) {
                return false;
            }
            const bool matched = std::regex_search(subject->begin(), subject->end(), *re);
            if ((name == "match?" && !matched) || (name == "not-match?" && matched)) {
                return false;
            }
        }

        i = end + 1;
    }
    return true;
}

/// Filter `$('<div>')`-style strings out of JS/TS imports. With proper
/// predicate evaluation in place (see `match_satisfies_predicates`),
/// jQuery's `$()` no longer matches the `require()` rule — but the
/// textual check stays as defence-in-depth against any future grammar
/// drift that might let the structural match through again.
[[nodiscard]] bool looks_like_css_selector(std::string_view s) noexcept
{
    if (s.empty()) {
        return false;
    }
    const char first = s.front();
    if (first == '<' || first == '#' || first == ':' || first == '[' || first == '*' ||
        first == '>' || first == '+' || first == '~') {
        return true;
    }
    // `.foo` is a CSS class but `./foo` is a relative path. Distinguish
    // by what follows the leading dot.
    if (first == '.' && s.size() >= 2 && s[1] != '/' && s[1] != '.') {
        return true;
    }
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\n') {
            return true;
        }
    }

    // Bare HTML tag names with no `-` or `.` — packages like
    // `body-parser` or `html2canvas` are not on the list since they
    // would not equal a tag.
    static constexpr std::array<std::string_view, 24> k_html_tags = {
        "html", "head", "body",   "div",    "span",    "a",       "p",     "img",
        "tr",   "td",   "th",     "ul",     "ol",      "li",      "form",  "input",
        "nav",  "main", "header", "footer", "section", "article", "aside", "table",
    };
    return std::ranges::any_of(k_html_tags, [&](std::string_view tag) { return s == tag; });
}

} // namespace

std::vector<RawImport> TreeSitterParser::extract_imports(Language language,
                                                         std::string_view content)
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

    TSTree* tree = ts_parser_parse_string(m_impl->parser, nullptr, content.data(),
                                          static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        return result;
    }

    const TSNode root = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.import_query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        if (!match_satisfies_predicates(entry.import_query, match, content)) {
            continue;
        }
        RawImport raw;
        bool has_path = false;
        bool has_kind = false;

        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            std::uint32_t capture_len = 0;
            const char* capture_chars =
                ts_query_capture_name_for_id(entry.import_query, capture.index, &capture_len);
            const std::string_view capture_name(capture_chars, capture_len);

            if (capture_name == "path") {
                const std::uint32_t start = ts_node_start_byte(capture.node);
                const std::uint32_t end = ts_node_end_byte(capture.node);
                if (end > content.size() || start > end) {
                    continue;
                }
                std::string_view path_text = content.substr(start, end - start);
                path_text = unquote(path_text, '"');
                path_text = unquote(path_text, '\'');
                raw.import_string.assign(path_text);
                has_path = true;
            }
            else if (!capture_name.empty() && capture_name.front() == '_') {
                // Convention: `@_xxx` captures exist only to feed
                // `#match?`/`#eq?` predicates (e.g. the Ruby method-name
                // check). They must not be confused with the kind tag.
                continue;
            }
            else {
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
            // Drop CSS / DOM selectors masquerading as imports — see
            // looks_like_css_selector for the precise filter.
            if ((language == Language::JavaScript || language == Language::TypeScript) &&
                looks_like_css_selector(raw.import_string)) {
                continue;
            }
            result.push_back(std::move(raw));
        }
    }

    ts_query_cursor_delete(cursor);
    ts_tree_delete(tree);
    return result;
}

std::vector<std::string> TreeSitterParser::extract_namespaces(Language language,
                                                              std::string_view content)
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
    TSTree* tree = ts_parser_parse_string(m_impl->parser, nullptr, content.data(),
                                          static_cast<std::uint32_t>(content.size()));
    if (tree == nullptr) {
        return result;
    }

    const TSNode root = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, entry.namespace_query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            std::uint32_t name_len = 0;
            const char* name_chars =
                ts_query_capture_name_for_id(entry.namespace_query, capture.index, &name_len);
            const std::string_view capture_name(name_chars, name_len);
            if (capture_name != "name") {
                continue;
            }
            const std::uint32_t start = ts_node_start_byte(capture.node);
            const std::uint32_t end = ts_node_end_byte(capture.node);
            if (end > content.size() || start > end) {
                continue;
            }
            std::string ns{content.substr(start, end - start)};
            // Strip stray whitespace from the captured span.
            while (!ns.empty() && std::isspace(static_cast<unsigned char>(ns.back())) != 0) {
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
