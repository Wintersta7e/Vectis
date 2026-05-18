#include "code/xml_reader.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vectis::code::xml {

namespace {

// =============================================================================
// XML parser
// =============================================================================
//
// Hand-rolled recursive-descent parser, tolerant per the spec in
// `xml_reader.h`:
//   * DOCTYPE skipped (not validated).
//   * Entity refs (named + numeric) decoded; unknown named entities left
//     as-is so caller-visible text is never silently corrupted.
//   * CDATA inlined into text content (verbatim, no entity decoding).
//   * Comments and processing instructions skipped wherever they appear.
//   * Malformed input returns `ErrorKind::ParseError`.
//
// Whitespace collapse and trimming for `Element::text()` happens at read
// time, not parse time — direct_text stores the raw post-entity-decode
// fragments so the reader can choose its own normalisation.
//
class XmlParser
{
public:
    explicit XmlParser(std::string_view src) noexcept : m_src(src) {}

    [[nodiscard]] vectis::core::Result<std::pair<std::vector<ElementNode>, std::size_t>> parse()
    {
        skip_misc(); // leading whitespace, XML decl, doctype, comments, PIs

        if (eof()) {
            return err("empty document — no root element");
        }
        if (peek() != '<' || (m_pos + 1 < m_src.size() && is_disallowed_start(peek(1)))) {
            return err("expected '<' followed by a tag name");
        }

        auto root_idx = parse_element(ElementNode::k_no_parent);
        if (!root_idx) {
            return tl::unexpected<vectis::core::Error>(root_idx.error());
        }

        skip_misc();
        if (!eof()) {
            return err("trailing content after root element");
        }

        return std::make_pair(std::move(m_nodes), *root_idx);
    }

private:
    std::string_view m_src;
    std::size_t m_pos = 0;
    std::vector<ElementNode> m_nodes;

    // -----------------------------------------------------------------------
    // Cursor helpers
    // -----------------------------------------------------------------------

    [[nodiscard]] bool eof() const noexcept { return m_pos >= m_src.size(); }

    [[nodiscard]] char peek(std::size_t offset = 0) const noexcept
    {
        const std::size_t p = m_pos + offset;
        return p < m_src.size() ? m_src[p] : '\0';
    }

    [[nodiscard]] bool starts_with(std::string_view needle) const noexcept
    {
        return m_src.compare(m_pos, needle.size(), needle) == 0;
    }

    void advance(std::size_t n = 1) noexcept { m_pos += n; }

    static bool is_space(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    static bool is_name_start(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
               static_cast<unsigned char>(c) >= 0x80;
    }

    static bool is_name_char(char c) noexcept
    {
        return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
    }

    static bool is_disallowed_start(char c) noexcept
    {
        // After `<`, characters that mean "not a tag start" — `!`, `?`, `/`
        // ARE valid tag-start contexts (DOCTYPE/CDATA, PI, end-tag). The
        // truly disallowed leaders are whitespace and `=`.
        return is_space(c) || c == '=' || c == '>';
    }

    void skip_ws() noexcept
    {
        while (!eof() && is_space(peek())) {
            advance();
        }
    }

    // -----------------------------------------------------------------------
    // Skip prolog / DOCTYPE / comments / PIs in any combination
    // -----------------------------------------------------------------------

    void skip_misc() noexcept
    {
        while (!eof()) {
            skip_ws();
            if (eof()) {
                return;
            }
            if (starts_with("<!--")) {
                skip_comment();
            }
            else if (starts_with("<?")) {
                skip_pi();
            }
            else if (starts_with("<!DOCTYPE") || starts_with("<!doctype")) {
                skip_doctype();
            }
            else {
                return;
            }
        }
    }

    void skip_comment() noexcept
    {
        // Already at "<!--". Find the next "-->" or EOF.
        advance(4);
        while (!eof()) {
            if (starts_with("-->")) {
                advance(3);
                return;
            }
            advance();
        }
    }

    void skip_pi() noexcept
    {
        // Already at "<?". Find the next "?>" or EOF.
        advance(2);
        while (!eof()) {
            if (starts_with("?>")) {
                advance(2);
                return;
            }
            advance();
        }
    }

    void skip_doctype() noexcept
    {
        // Already at "<!DOCTYPE". Skip until matching '>' at depth 0,
        // honouring the optional `[...]` internal subset.
        advance(9);
        int bracket_depth = 0;
        while (!eof()) {
            const char c = peek();
            if (c == '[') {
                ++bracket_depth;
            }
            else if (c == ']') {
                if (bracket_depth > 0) {
                    --bracket_depth;
                }
            }
            else if (c == '>' && bracket_depth == 0) {
                advance();
                return;
            }
            advance();
        }
    }

    // -----------------------------------------------------------------------
    // Element parsing
    // -----------------------------------------------------------------------

    [[nodiscard]] vectis::core::Result<std::size_t> parse_element(std::size_t parent_idx)
    {
        if (peek() != '<') {
            return err("internal: parse_element called outside '<'");
        }
        advance(); // consume '<'

        // Tag name (prefix:local).
        std::string prefix;
        std::string local_name;
        if (!read_qname(prefix, local_name)) {
            return err("expected tag name after '<'");
        }

        // Reserve a slot now so child elements know their parent index.
        // After this push_back we MUST NOT hold any reference into m_nodes
        // — child recursion may reallocate. Always re-index via my_idx.
        ElementNode node;
        node.prefix = std::move(prefix);
        node.local_name = std::move(local_name);
        node.parent = parent_idx;
        m_nodes.push_back(std::move(node));
        const std::size_t my_idx = m_nodes.size() - 1;

        // Attributes / xmlns declarations.
        for (;;) {
            skip_ws();
            if (eof()) {
                return err("unexpected EOF inside start tag");
            }
            const char c = peek();
            if (c == '/' && peek(1) == '>') {
                advance(2);
                return my_idx;
            }
            if (c == '>') {
                advance();
                break; // fall through to content
            }
            if (!is_name_start(c)) {
                return err("unexpected character in start tag");
            }

            std::string attr_prefix;
            std::string attr_local;
            if (!read_qname(attr_prefix, attr_local)) {
                return err("expected attribute name");
            }
            skip_ws();
            if (peek() != '=') {
                return err("expected '=' after attribute name");
            }
            advance();
            skip_ws();
            std::string value;
            auto v = read_attribute_value();
            if (!v) {
                return tl::unexpected<vectis::core::Error>(v.error());
            }
            value = std::move(*v);

            if (attr_prefix.empty() && attr_local == "xmlns") {
                m_nodes[my_idx].ns_decls.push_back({"", std::move(value)});
            }
            else if (attr_prefix == "xmlns") {
                m_nodes[my_idx].ns_decls.push_back({std::move(attr_local), std::move(value)});
            }
            else {
                Attribute a;
                a.prefix = std::move(attr_prefix);
                a.local_name = std::move(attr_local);
                a.value = std::move(value);
                m_nodes[my_idx].attrs.push_back(std::move(a));
            }
        }

        // Content: alternating text runs and child elements until matching
        // end tag. Accumulate text into one buffer; flush once at close.
        // The inner loop hoists `peek()` once per iteration and gates the
        // `<…` probes behind `c == '<'` so plain-text characters (the bulk
        // of manifest bytes) take a single comparison.
        std::string text_buf;
        for (;;) {
            if (eof()) {
                return err("unexpected EOF inside element content");
            }
            const char c = peek();

            if (c == '<') {
                if (starts_with("</")) {
                    advance(2);
                    std::string end_prefix;
                    std::string end_local;
                    if (!read_qname(end_prefix, end_local)) {
                        return err("expected tag name in end tag");
                    }
                    skip_ws();
                    if (peek() != '>') {
                        return err("expected '>' to close end tag");
                    }
                    advance();
                    if (end_prefix != m_nodes[my_idx].prefix ||
                        end_local != m_nodes[my_idx].local_name) {
                        return err("end tag does not match start tag");
                    }
                    if (!text_buf.empty()) {
                        m_nodes[my_idx].direct_text.push_back(std::move(text_buf));
                    }
                    return my_idx;
                }
                if (starts_with("<!--")) {
                    skip_comment();
                    continue;
                }
                if (starts_with("<?")) {
                    skip_pi();
                    continue;
                }
                if (starts_with("<![CDATA[")) {
                    advance(9);
                    while (!eof() && !starts_with("]]>")) {
                        text_buf.push_back(peek());
                        advance();
                    }
                    if (eof()) {
                        return err("unterminated CDATA section");
                    }
                    advance(3); // consume "]]>"
                    continue;
                }
                // Child element — recurse.
                auto child_idx = parse_element(my_idx);
                if (!child_idx) {
                    return tl::unexpected<vectis::core::Error>(child_idx.error());
                }
                m_nodes[my_idx].children.push_back(*child_idx);
                continue;
            }

            if (c == '&') {
                if (!decode_entity_into(text_buf)) {
                    return err("malformed entity reference");
                }
                continue;
            }

            // Plain character content — preserve whitespace verbatim
            // until `text()` normalises it.
            text_buf.push_back(c);
            advance();
        }
    }

    // -----------------------------------------------------------------------
    // Attribute value (entity-decoded)
    // -----------------------------------------------------------------------

    [[nodiscard]] vectis::core::Result<std::string> read_attribute_value()
    {
        const char quote = peek();
        if (quote != '"' && quote != '\'') {
            return err("expected quoted attribute value");
        }
        advance();

        std::string out;
        while (!eof() && peek() != quote) {
            if (peek() == '<') {
                return err("'<' is not allowed inside an attribute value");
            }
            if (peek() == '&') {
                if (!decode_entity_into(out)) {
                    return err("malformed entity reference in attribute value");
                }
                continue;
            }
            out.push_back(peek());
            advance();
        }
        if (eof()) {
            return err("unterminated attribute value");
        }
        advance(); // consume closing quote
        return out;
    }

    // -----------------------------------------------------------------------
    // Entity decoding
    // -----------------------------------------------------------------------

    [[nodiscard]] bool decode_entity_into(std::string& out)
    {
        // Already at '&'. Find the next ';' within a small window (32 is
        // enough for any well-formed entity ref we accept).
        const std::size_t start = m_pos;
        const std::size_t limit = std::min(m_src.size(), start + 32);
        std::size_t semi = m_pos + 1;
        while (semi < limit && m_src[semi] != ';') {
            ++semi;
        }
        if (semi >= limit || m_src[semi] != ';') {
            return false;
        }

        const std::string_view body = m_src.substr(start + 1, semi - start - 1);
        // Named entities.
        if (body == "amp") {
            out.push_back('&');
        }
        else if (body == "lt") {
            out.push_back('<');
        }
        else if (body == "gt") {
            out.push_back('>');
        }
        else if (body == "quot") {
            out.push_back('"');
        }
        else if (body == "apos") {
            out.push_back('\'');
        }
        else if (!body.empty() && body.front() == '#') {
            // Numeric: decimal &#NN; or hex &#xHH;.
            std::uint32_t code = 0;
            if (body.size() >= 2 && (body[1] == 'x' || body[1] == 'X')) {
                for (std::size_t i = 2; i < body.size(); ++i) {
                    const char c = body[i];
                    std::uint32_t digit = 0;
                    if (c >= '0' && c <= '9') {
                        digit = static_cast<std::uint32_t>(c - '0');
                    }
                    else if (c >= 'a' && c <= 'f') {
                        digit = static_cast<std::uint32_t>(c - 'a') + 10U;
                    }
                    else if (c >= 'A' && c <= 'F') {
                        digit = static_cast<std::uint32_t>(c - 'A') + 10U;
                    }
                    else {
                        return false;
                    }
                    code = (code << 4U) | digit;
                }
            }
            else {
                for (std::size_t i = 1; i < body.size(); ++i) {
                    const char c = body[i];
                    if (c < '0' || c > '9') {
                        return false;
                    }
                    code = (code * 10U) + static_cast<std::uint32_t>(c - '0');
                }
            }
            append_utf8(out, code);
        }
        else {
            // Unknown named entity: copy "&body;" through verbatim so the
            // caller can recover what the source said.
            out.push_back('&');
            out.append(body);
            out.push_back(';');
        }
        m_pos = semi + 1;
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80U) {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else if (code < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        else if (code <= 0x10FFFFU) {
            out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
        }
        // Codepoints above U+10FFFF are silently dropped.
    }

    // -----------------------------------------------------------------------
    // QName reading
    // -----------------------------------------------------------------------

    bool read_qname(std::string& prefix, std::string& local) noexcept
    {
        prefix.clear();
        local.clear();
        if (eof() || !is_name_start(peek())) {
            return false;
        }
        std::string first;
        while (!eof() && is_name_char(peek())) {
            first.push_back(peek());
            advance();
        }
        if (!eof() && peek() == ':') {
            advance();
            while (!eof() && is_name_char(peek())) {
                local.push_back(peek());
                advance();
            }
            if (local.empty()) {
                return false;
            }
            prefix = std::move(first);
            return true;
        }
        local = std::move(first);
        return true;
    }

    // -----------------------------------------------------------------------
    // Error helper
    // -----------------------------------------------------------------------

    [[nodiscard]] tl::unexpected<vectis::core::Error> err(std::string msg) const
    {
        return vectis::core::make_error(vectis::core::ErrorKind::ParseError, std::move(msg),
                                        "byte_offset=" + std::to_string(m_pos));
    }
};

// =============================================================================
// text() normalisation
// =============================================================================

std::string normalise_text(const std::vector<std::string>& runs)
{
    std::string raw;
    for (const std::string& s : runs) {
        raw.append(s);
    }

    // Collapse runs of ASCII whitespace to a single space, trim ends.
    std::string out;
    out.reserve(raw.size());
    bool in_space = false;
    bool wrote_any = false;
    for (char c : raw) {
        const bool is_ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (is_ws) {
            if (wrote_any) {
                in_space = true;
            }
            continue;
        }
        if (in_space) {
            out.push_back(' ');
            in_space = false;
        }
        out.push_back(c);
        wrote_any = true;
    }
    return out;
}

} // namespace

// =============================================================================
// Element accessors
// =============================================================================

std::string_view Element::local_name() const noexcept
{
    if (m_doc == nullptr) {
        return {};
    }
    return m_doc->node(m_index).local_name;
}

std::string_view Element::prefix() const noexcept
{
    if (m_doc == nullptr) {
        return {};
    }
    return m_doc->node(m_index).prefix;
}

std::string_view Element::namespace_uri() const noexcept
{
    if (m_doc == nullptr) {
        return {};
    }
    const std::string& want = m_doc->node(m_index).prefix;
    std::size_t idx = m_index;
    while (idx != ElementNode::k_no_parent) {
        const ElementNode& n = m_doc->node(idx);
        for (const NamespaceDecl& d : n.ns_decls) {
            if (d.prefix == want) {
                return d.uri;
            }
        }
        idx = n.parent;
    }
    return {};
}

std::string Element::text() const
{
    if (m_doc == nullptr) {
        return {};
    }
    return normalise_text(m_doc->node(m_index).direct_text);
}

std::string_view Element::attribute(std::string_view local_name) const noexcept
{
    if (m_doc == nullptr) {
        return {};
    }
    for (const Attribute& a : m_doc->node(m_index).attrs) {
        if (a.local_name == local_name) {
            return a.value;
        }
    }
    return {};
}

std::vector<Element> Element::children() const
{
    std::vector<Element> out;
    if (m_doc == nullptr) {
        return out;
    }
    const auto& child_indices = m_doc->node(m_index).children;
    out.reserve(child_indices.size());
    for (std::size_t child_idx : child_indices) {
        out.emplace_back(m_doc, child_idx);
    }
    return out;
}

std::vector<Element> Element::children(std::string_view local_name) const
{
    std::vector<Element> out;
    if (m_doc == nullptr) {
        return out;
    }
    for (std::size_t child_idx : m_doc->node(m_index).children) {
        if (m_doc->node(child_idx).local_name == local_name) {
            out.emplace_back(m_doc, child_idx);
        }
    }
    return out;
}

std::vector<Element> Element::children_ns(std::string_view local_name,
                                          std::string_view ns_uri) const
{
    std::vector<Element> out;
    if (m_doc == nullptr) {
        return out;
    }
    for (std::size_t child_idx : m_doc->node(m_index).children) {
        if (m_doc->node(child_idx).local_name != local_name) {
            continue;
        }
        const Element child(m_doc, child_idx);
        if (child.namespace_uri() == ns_uri) {
            out.push_back(child);
        }
    }
    return out;
}

std::optional<Element> Element::first_child(std::string_view local_name) const
{
    if (m_doc == nullptr) {
        return std::nullopt;
    }
    for (std::size_t child_idx : m_doc->node(m_index).children) {
        if (m_doc->node(child_idx).local_name == local_name) {
            return Element(m_doc, child_idx);
        }
    }
    return std::nullopt;
}

// =============================================================================
// Document accessors
// =============================================================================

Element Document::root() const noexcept
{
    if (m_nodes.empty()) {
        return {};
    }
    return {this, m_root_index};
}

// =============================================================================
// parse()
// =============================================================================

vectis::core::Result<Document> parse(std::string_view content)
{
    // Strip a leading UTF-8 BOM (EF BB BF). Visual Studio writes csproj,
    // .props, and .slnx files with a BOM by default, and the parser's
    // `<` lookahead in `parse()` would otherwise reject the file outright.
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.remove_prefix(3);
    }
    XmlParser p(content);
    auto built = p.parse();
    if (!built) {
        return tl::unexpected<vectis::core::Error>(built.error());
    }
    Document doc;
    doc.m_nodes = std::move(built->first);
    doc.m_root_index = built->second;
    return doc;
}

} // namespace vectis::code::xml
