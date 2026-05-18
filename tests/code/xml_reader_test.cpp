#include <string_view>

#include <gtest/gtest.h>

#include "code/xml_reader.h"
#include "core/result.h"

namespace {

using vectis::code::xml::Document;
using vectis::code::xml::Element;
using vectis::code::xml::parse;
using vectis::core::ErrorKind;

Document parse_or_die(std::string_view content)
{
    auto result = parse(content);
    if (!result) {
        ADD_FAILURE() << "parse() unexpectedly failed: " << result.error().message;
        return Document{};
    }
    return std::move(*result);
}

TEST(XmlReaderTest, ParsesRootElementWithUnprefixedName)
{
    const Document doc = parse_or_die("<project/>");
    const Element root = doc.root();
    ASSERT_TRUE(root.valid());
    EXPECT_EQ(root.local_name(), "project");
    EXPECT_EQ(root.prefix(), "");
}

TEST(XmlReaderTest, StripsLeadingUtf8Bom)
{
    // Visual Studio saves csproj / .props / .slnx with a UTF-8 BOM by
    // default. Without explicit BOM handling the `<` lookahead at the
    // start of parse() rejects the file outright, and every consumer
    // (csproj handler, packages.props CPM, .slnx walker) silently loses
    // all edges from that file.
    constexpr std::string_view with_bom = "\xEF\xBB\xBF<project/>";
    const Document doc = parse_or_die(with_bom);
    const Element root = doc.root();
    ASSERT_TRUE(root.valid());
    EXPECT_EQ(root.local_name(), "project");
}

TEST(XmlReaderTest, StripsBomAheadOfLeadingComment)
{
    // Real-world csprojs commonly carry both a BOM and a copyright
    // comment ahead of the `<Project>` element.
    constexpr std::string_view input = "\xEF\xBB\xBF<!-- Copyright (c) Example. --><project/>";
    const Document doc = parse_or_die(input);
    EXPECT_EQ(doc.root().local_name(), "project");
}

TEST(XmlReaderTest, LocalNameStripsNamespacePrefix)
{
    const Document doc = parse_or_die("<context:component-scan/>");
    const Element root = doc.root();
    EXPECT_EQ(root.local_name(), "component-scan");
}

TEST(XmlReaderTest, PrefixIsExposedSeparatelyFromLocalName)
{
    const Document doc = parse_or_die("<context:component-scan/>");
    const Element root = doc.root();
    EXPECT_EQ(root.prefix(), "context");
}

TEST(XmlReaderTest, NamespaceUriResolvesViaXmlnsDeclarationOnSameElement)
{
    const Document doc = parse_or_die(R"(<project xmlns="http://maven.apache.org/POM/4.0.0"/>)");
    const Element root = doc.root();
    EXPECT_EQ(root.namespace_uri(), "http://maven.apache.org/POM/4.0.0");
}

TEST(XmlReaderTest, NamespaceUriInheritsFromAncestor)
{
    const Document doc = parse_or_die(
        R"(<project xmlns="http://maven.apache.org/POM/4.0.0"><modelVersion>4.0.0</modelVersion></project>)");
    const Element root = doc.root();
    const auto child = root.first_child("modelVersion");
    ASSERT_TRUE(child.has_value());
    EXPECT_EQ(child->namespace_uri(), "http://maven.apache.org/POM/4.0.0");
}

TEST(XmlReaderTest, AttributeLookupByLocalNameIgnoresPrefix)
{
    // `<bean xsi:type="X"/>` — agent reads via local name "type".
    const Document doc = parse_or_die(R"(<bean xsi:type="java.lang.String"/>)");
    const Element root = doc.root();
    EXPECT_EQ(root.attribute("type"), "java.lang.String");
}

TEST(XmlReaderTest, AttributeLookupReturnsEmptyForMissingAttribute)
{
    const Document doc = parse_or_die(R"(<bean class="X"/>)");
    const Element root = doc.root();
    EXPECT_EQ(root.attribute("missing"), "");
}

TEST(XmlReaderTest, ChildrenByLocalNameIsNamespaceAgnostic)
{
    const Document doc = parse_or_die(R"(<a xmlns:x="http://x"><b/><x:b/><c/></a>)");
    const Element root = doc.root();
    const auto matches = root.children("b");
    EXPECT_EQ(matches.size(), 2U);
}

TEST(XmlReaderTest, ChildrenNsRequiresBothLocalNameAndNamespaceUri)
{
    const Document doc =
        parse_or_die(R"(<a xmlns:x="http://x" xmlns:y="http://y"><x:b/><y:b/><x:c/></a>)");
    const Element root = doc.root();
    const auto matches = root.children_ns("b", "http://x");
    EXPECT_EQ(matches.size(), 1U);
    EXPECT_EQ(matches[0].namespace_uri(), "http://x");
}

TEST(XmlReaderTest, FirstChildReturnsNulloptWhenAbsent)
{
    const Document doc = parse_or_die("<a><b/></a>");
    const Element root = doc.root();
    EXPECT_FALSE(root.first_child("c").has_value());
}

TEST(XmlReaderTest, TextDecodesNamedEntities)
{
    const Document doc = parse_or_die("<a>5 &lt; 6 &amp;&amp; 6 &gt; 5</a>");
    const Element root = doc.root();
    EXPECT_EQ(root.text(), "5 < 6 && 6 > 5");
}

TEST(XmlReaderTest, TextDecodesNumericEntities)
{
    // &#65; → 'A', &#x41; → 'A', &#10; → '\n' (collapsed to space in text())
    const Document doc = parse_or_die("<a>&#65;&#x41;</a>");
    const Element root = doc.root();
    EXPECT_EQ(root.text(), "AA");
}

TEST(XmlReaderTest, UnknownNamedEntityIsLeftAsIs)
{
    const Document doc = parse_or_die("<a>&nope;</a>");
    const Element root = doc.root();
    EXPECT_EQ(root.text(), "&nope;");
}

TEST(XmlReaderTest, TextInlinesCdataSections)
{
    const Document doc = parse_or_die("<a><![CDATA[raw <stuff> & things]]></a>");
    const Element root = doc.root();
    EXPECT_EQ(root.text(), "raw <stuff> & things");
}

TEST(XmlReaderTest, TextCollapsesWhitespaceAndTrims)
{
    const Document doc = parse_or_die("<a>   hello\n\t  world   </a>");
    const Element root = doc.root();
    EXPECT_EQ(root.text(), "hello world");
}

TEST(XmlReaderTest, CommentsAndProcessingInstructionsAreSkipped)
{
    const Document doc = parse_or_die(
        "<?xml version=\"1.0\"?><!-- header --><a><?route GET /api?>value<!-- inner --></a>");
    const Element root = doc.root();
    EXPECT_EQ(root.local_name(), "a");
    EXPECT_EQ(root.text(), "value");
}

TEST(XmlReaderTest, DoctypeIsSkippedWithoutValidation)
{
    const Document doc = parse_or_die(
        R"(<!DOCTYPE beans PUBLIC "-//SPRING//DTD BEAN//EN" "http://www.springframework.org/dtd/spring-beans.dtd">
<beans><bean id="x"/></beans>)");
    const Element root = doc.root();
    EXPECT_EQ(root.local_name(), "beans");
    ASSERT_TRUE(root.first_child("bean").has_value());
}

TEST(XmlReaderTest, MalformedInputReturnsParseError)
{
    // Unclosed tag.
    const auto bad = parse("<a><b></a>");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind, ErrorKind::ParseError);
}

} // namespace
