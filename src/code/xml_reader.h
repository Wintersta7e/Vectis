#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"

namespace vectis::code::xml {

/// One attribute on an element. Both the local-name half and the value
/// are owned by the parent Document so views handed to callers stay
/// valid for the document's lifetime.
struct Attribute
{
    std::string prefix;     ///< empty if no prefix (`<foo bar="x"/>`)
    std::string local_name; ///< e.g. `bar` for `bar="x"` or `xsi:type`
    std::string value;      ///< already entity-decoded at parse time
};

/// xmlns declaration on an element (`xmlns="..."` or `xmlns:p="...").
/// `prefix` is empty for the default namespace; otherwise the prefix
/// being declared. `uri` is the resolved namespace URI.
struct NamespaceDecl
{
    std::string prefix;
    std::string uri;
};

/// One parsed element. Stored flat in `Document::m_nodes`; `Element`
/// handles index into that flat array. `parent` is `k_no_parent` for the
/// root, otherwise the index of the parent node. `direct_text` holds
/// the text content of this element's *direct* text runs (CDATA
/// inlined, entity refs decoded at parse time); concatenation +
/// whitespace collapse happens lazily in `Element::text()`.
struct ElementNode
{
    static constexpr std::size_t k_no_parent = static_cast<std::size_t>(-1);

    std::string prefix;
    std::string local_name;
    std::vector<NamespaceDecl> ns_decls;
    std::vector<Attribute> attrs;
    std::size_t parent = k_no_parent;
    std::vector<std::string> direct_text;
    std::vector<std::size_t> children;
};

class Document;

/// Non-owning handle to one element in a parsed `Document`. The
/// referenced `Document` must outlive every `Element` it produced.
/// Copies are cheap — pointer + index.
class Element
{
public:
    Element() noexcept = default;
    Element(const Document* doc, std::size_t index) noexcept : m_doc(doc), m_index(index) {}

    /// True if this handle refers to a real element. Default-constructed
    /// handles and the empty-optional sentinel from `first_child()` are
    /// the only ways to obtain an invalid handle in normal use.
    [[nodiscard]] bool valid() const noexcept { return m_doc != nullptr; }

    /// Local tag name (without namespace prefix).
    /// `<context:component-scan>` → `"component-scan"`.
    [[nodiscard]] std::string_view local_name() const noexcept;

    /// Namespace prefix as written, e.g. `"context"` for
    /// `<context:component-scan>`. Empty string for unprefixed.
    [[nodiscard]] std::string_view prefix() const noexcept;

    /// Full namespace URI resolved via xmlns declarations on the
    /// element or any ancestor. Empty if no namespace was declared.
    /// Walks the ancestor chain so a default `xmlns` on the root
    /// applies to every unprefixed descendant.
    [[nodiscard]] std::string_view namespace_uri() const noexcept;

    /// Decoded text content. Direct-child text runs are concatenated
    /// in source order, entity refs were collapsed and CDATA inlined
    /// at parse time, then runs of whitespace are collapsed to single
    /// spaces and the result is trimmed. Mixed content with
    /// interleaved elements gives back only the direct text portions
    /// (sufficient for the manifest XML this reader targets).
    [[nodiscard]] std::string text() const;

    /// Attribute lookup by local name. Namespace prefix on the
    /// attribute is ignored — agents read attributes by local name in
    /// 99% of cases (`<bean class="X"/>`, `<dependency scope="..."/>`).
    /// Returns an empty view if absent.
    [[nodiscard]] std::string_view attribute(std::string_view local_name) const noexcept;

    /// Direct children with the given local name (namespace-agnostic).
    [[nodiscard]] std::vector<Element> children(std::string_view local_name) const;

    /// Direct children with both local name AND namespace URI matching.
    [[nodiscard]] std::vector<Element> children_ns(std::string_view local_name,
                                                   std::string_view ns_uri) const;

    /// First direct child matching `local_name`, or `std::nullopt`.
    [[nodiscard]] std::optional<Element> first_child(std::string_view local_name) const;

private:
    const Document* m_doc = nullptr;
    std::size_t m_index = 0;
};

/// Owning storage for a parsed XML tree. Move-only; copies would be
/// expensive and the API is built around taking handles into one
/// stable instance.
class Document
{
public:
    Document() = default;
    ~Document() = default;

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;

    /// Root element. Behaviour is undefined if the document is empty
    /// (parse() never returns an empty document — it errors instead).
    [[nodiscard]] Element root() const noexcept;

    // ----- Internal storage accessors (used by Element) ----------------

    [[nodiscard]] const ElementNode& node(std::size_t index) const noexcept
    {
        return m_nodes[index];
    }
    [[nodiscard]] std::size_t node_count() const noexcept { return m_nodes.size(); }

private:
    std::vector<ElementNode> m_nodes;
    std::size_t m_root_index = 0;

    friend vectis::core::Result<Document> parse(std::string_view content);
};

/// Parse XML with tolerant settings.
///
///   * DOCTYPE declarations are skipped (not validated against).
///   * Entity references &amp; &lt; &gt; &quot; &apos; are decoded;
///     numeric refs (&#10; &#x41;) are decoded; unknown named entities
///     left as-is.
///   * CDATA sections are inlined into text content.
///   * Comments and processing instructions are skipped.
///   * Malformed input (unclosed tags, bad attribute syntax) returns
///     `ErrorKind::ParseError`; the caller logs WARN and skips the
///     file.
[[nodiscard]] vectis::core::Result<Document> parse(std::string_view content);

} // namespace vectis::code::xml
