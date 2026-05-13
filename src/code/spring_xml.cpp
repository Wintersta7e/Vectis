#include "code/spring_xml.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vectis::code::spring {

namespace {

constexpr std::string_view k_spring_beans_ns = "http://www.springframework.org/schema/beans";

/// Recursive walk: does any element in `el`'s subtree (including `el`
/// itself) carry the Spring beans namespace URI?
[[nodiscard]] bool any_element_in_spring_ns(const xml::Element& el) noexcept
{
    if (el.namespace_uri() == k_spring_beans_ns) {
        return true;
    }
    const auto children = el.children();
    return std::ranges::any_of(children, [](const xml::Element& child) noexcept {
        return any_element_in_spring_ns(child);
    });
}

/// Split `value` on commas, trim ASCII whitespace from each entry,
/// drop empty entries. Used for `<context:component-scan
/// base-package="X, Y, Z"/>` and structurally similar lists.
[[nodiscard]] std::vector<std::string> split_csv_trim(std::string_view value)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find(',', start);
        if (end == std::string_view::npos) {
            end = value.size();
        }
        std::string_view segment = value.substr(start, end - start);
        // Trim ASCII whitespace (space, tab, CR, LF).
        while (!segment.empty() && (segment.front() == ' ' || segment.front() == '\t' ||
                                    segment.front() == '\r' || segment.front() == '\n')) {
            segment.remove_prefix(1);
        }
        while (!segment.empty() && (segment.back() == ' ' || segment.back() == '\t' ||
                                    segment.back() == '\r' || segment.back() == '\n')) {
            segment.remove_suffix(1);
        }
        if (!segment.empty()) {
            out.emplace_back(segment);
        }
        if (end == value.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

/// Recursive walker that visits every element in the subtree and
/// appends bean/import/component-scan entries to `out`.
void walk_collect(const xml::Element& el, ParsedSpringXml& out)
{
    const std::string_view ns = el.namespace_uri();
    const std::string_view local = el.local_name();

    // <bean class="X"> in the Spring beans namespace OR no-namespace
    // (DTD-style legacy files).
    if (local == "bean" && (ns == k_spring_beans_ns || ns.empty())) {
        const std::string_view class_attr = el.attribute("class");
        if (!class_attr.empty()) {
            SpringBeanRef bean;
            bean.fqcn.assign(class_attr);
            const std::string_view id_attr = el.attribute("id");
            if (!id_attr.empty()) {
                bean.id = std::string{id_attr};
            }
            out.beans.push_back(std::move(bean));
        }
    }

    // <import resource="...">
    if (local == "import" && (ns == k_spring_beans_ns || ns.empty())) {
        const std::string_view resource_attr = el.attribute("resource");
        if (!resource_attr.empty()) {
            SpringImportRef imp;
            imp.resource.assign(resource_attr);
            out.imports.push_back(std::move(imp));
        }
    }

    // <context:component-scan base-package="..."> — match by local
    // name so namespace-prefix variations don't drop the edge.
    if (local == "component-scan") {
        const std::string_view base_attr = el.attribute("base-package");
        if (!base_attr.empty()) {
            SpringComponentScanRef scan;
            scan.packages = split_csv_trim(base_attr);
            if (!scan.packages.empty()) {
                out.scans.push_back(std::move(scan));
            }
        }
    }

    for (const auto& child : el.children()) {
        walk_collect(child, out);
    }
}

} // namespace

bool maybe_spring_beans(std::string_view content_peek) noexcept
{
    return content_peek.find("<beans") != std::string_view::npos;
}

bool is_spring_beans_xml(const xml::Document& doc) noexcept
{
    const xml::Element root = doc.root();
    if (!root.valid()) {
        return false;
    }

    // Namespace path: any element carries the Spring beans URI.
    if (any_element_in_spring_ns(root)) {
        return true;
    }

    // DTD / legacy path: empty namespace + root local name "beans" +
    // at least one direct <bean> child.
    if (root.namespace_uri().empty() && root.local_name() == "beans" &&
        !root.children("bean").empty()) {
        return true;
    }

    return false;
}

ParsedSpringXml parse_spring_xml(const xml::Document& doc)
{
    ParsedSpringXml out;
    const xml::Element root = doc.root();
    if (!root.valid()) {
        return out;
    }
    walk_collect(root, out);
    return out;
}

std::string_view fqcn_without_nested(std::string_view fqcn) noexcept
{
    const auto pos = fqcn.find('$');
    if (pos == std::string_view::npos) {
        return fqcn;
    }
    return fqcn.substr(0, pos);
}

} // namespace vectis::code::spring
