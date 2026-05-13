#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "code/xml_reader.h"

namespace vectis::code::spring {

/// Cheap pre-filter for the 4 KB content peek the manifest scanner
/// already does on every file. Returns true if the peek contains a
/// `<beans` opening-tag substring; the full check requires parsing
/// the document and calling `is_spring_beans_xml`. The pre-filter
/// rules out 99% of non-Spring XML files without parsing them.
[[nodiscard]] bool maybe_spring_beans(std::string_view content_peek) noexcept;

/// True if `doc` is a Spring `<beans>` file by structure. Two
/// acceptance paths (either suffices):
///
///   * **Namespace path** — any element in the tree uses the
///     namespace URI `http://www.springframework.org/schema/beans`.
///     Catches every modern Spring XML regardless of root prefix.
///   * **DTD / legacy path** — root has empty namespace AND local
///     name `beans` AND at least one direct `<bean>` child element.
///     The `<bean>`-child requirement rules out random no-namespace
///     XML files that happen to use `beans` as a tag name.
///
/// Requires `doc` to already be parsed by `xml::parse`. The cheap
/// 4 KB pre-filter (`maybe_spring_beans`) is the right gate before
/// paying the full parse cost.
[[nodiscard]] bool is_spring_beans_xml(const xml::Document& doc) noexcept;

/// One `<bean ...>` declaration. `id` is the optional `id="..."`
/// attribute; `fqcn` is the raw `class="..."` value (no nested-class
/// stripping at parse time — call `fqcn_without_nested` separately
/// when resolving against `match_java_dotted_candidates`).
struct SpringBeanRef
{
    std::optional<std::string> id;
    std::string fqcn;
};

/// One `<import resource="..."/>` declaration. The raw `resource`
/// attribute value is preserved verbatim; 3b's handler will pick
/// apart `classpath:`, `classpath*:`, leading-slash, relative, and
/// `${...}` placeholder forms.
struct SpringImportRef
{
    std::string resource;
};

/// One `<context:component-scan base-package="X,Y,Z"/>` declaration
/// already split into individual package names. Comma-separated
/// values, multi-line whitespace, and empty segments are normalised
/// at parse time; `${...}`-only segments are preserved as a single
/// literal package entry so 3b emits one external edge per
/// placeholder.
struct SpringComponentScanRef
{
    std::vector<std::string> packages;
};

/// Aggregate of what `parse_spring_xml` extracts. Order between the
/// three vectors carries no meaning — edge emission is per-kind.
struct ParsedSpringXml
{
    std::vector<SpringBeanRef> beans;
    std::vector<SpringImportRef> imports;
    std::vector<SpringComponentScanRef> scans;
};

/// Walk `doc` and extract bean/import/component-scan declarations.
/// Pure transformation — no file resolution, no edge emission.
/// `<bean>` and `<import>` are matched in either the Spring beans
/// namespace OR with no namespace (DTD-style files); `<component-
/// scan>` is matched by local name only, so any namespace prefix
/// (`context:component-scan`, `c:component-scan`, …) resolves.
/// `<aop:>`, `<tx:>`, `<util:>`, `<jee:>` and other Spring sub-
/// namespaces are skipped (handlers we don't implement). Beans
/// without a `class=` attribute are skipped (no FQCN to emit).
[[nodiscard]] ParsedSpringXml parse_spring_xml(const xml::Document& doc);

/// Strip a nested-class suffix from a Java FQCN so the path-shape
/// resolver can find the enclosing `.java` file. `com.x.Outer$Inner`
/// → `com.x.Outer`. Returns the input unchanged if no `$` is
/// present. Used by Phase 3b when resolving `<bean class="X$Inner">`
/// — the emitted edge's `import_string` keeps the full original
/// FQCN so agents see the nested-class hint.
///
/// The returned view aliases `fqcn`; the caller must keep the
/// source string alive while using the result.
[[nodiscard]] std::string_view fqcn_without_nested(std::string_view fqcn) noexcept;

} // namespace vectis::code::spring
