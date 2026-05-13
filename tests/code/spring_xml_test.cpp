#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "code/spring_xml.h"
#include "code/xml_reader.h"
#include "core/result.h"

namespace {

using vectis::code::spring::fqcn_without_nested;
using vectis::code::spring::is_spring_beans_xml;
using vectis::code::spring::maybe_spring_beans;
using vectis::code::spring::parse_spring_xml;
using vectis::code::spring::ParsedSpringXml;
using vectis::code::xml::Document;
using vectis::code::xml::parse;

/// Helper: parse XML or fail the test with a useful message.
Document parse_or_fail(std::string_view content)
{
    auto result = parse(content);
    if (!result) {
        ADD_FAILURE() << "xml::parse rejected input: " << result.error().message;
        return {};
    }
    return std::move(result.value());
}

} // namespace

// --- maybe_spring_beans -------------------------------------------------

TEST(SpringXmlTest, MaybeSpringBeans_AcceptsBeansTagInPeek)
{
    EXPECT_TRUE(maybe_spring_beans("<?xml version=\"1.0\"?>\n<beans xmlns=\"...\">"));
    EXPECT_TRUE(maybe_spring_beans("<!DOCTYPE beans PUBLIC ...><beans>"));
}

TEST(SpringXmlTest, MaybeSpringBeans_RejectsContentWithoutBeansSubstring)
{
    EXPECT_FALSE(maybe_spring_beans(""));
    EXPECT_FALSE(maybe_spring_beans("<root><child/></root>"));
    EXPECT_FALSE(maybe_spring_beans("<configuration><appender>...</appender></configuration>"));
}

// --- is_spring_beans_xml — namespace path -------------------------------

TEST(SpringXmlTest, IsSpringBeansXml_NamespacedRoot_Accepted)
{
    const auto doc = parse_or_fail(R"(
<?xml version="1.0"?>
<beans xmlns="http://www.springframework.org/schema/beans">
    <bean id="x" class="com.example.Foo"/>
</beans>
)");
    EXPECT_TRUE(is_spring_beans_xml(doc));
}

TEST(SpringXmlTest, IsSpringBeansXml_NamespacedWithPrefix_Accepted)
{
    // Default xmlns differs but a child uses the Spring beans namespace —
    // the sniffer accepts any element in the tree carrying that URI.
    const auto doc = parse_or_fail(R"(
<?xml version="1.0"?>
<config xmlns="http://example.com/config"
        xmlns:beans="http://www.springframework.org/schema/beans">
    <beans:beans>
        <beans:bean id="x" class="com.example.Foo"/>
    </beans:beans>
</config>
)");
    EXPECT_TRUE(is_spring_beans_xml(doc));
}

// --- is_spring_beans_xml — DTD / legacy path ----------------------------

TEST(SpringXmlTest, IsSpringBeansXml_DtdStyleWithBeanChild_Accepted)
{
    const auto doc = parse_or_fail(R"(
<?xml version="1.0"?>
<!DOCTYPE beans PUBLIC "-//SPRING//DTD BEAN 2.0//EN"
                       "http://www.springframework.org/dtd/spring-beans-2.0.dtd">
<beans>
    <bean id="x" class="com.example.Foo"/>
</beans>
)");
    EXPECT_TRUE(is_spring_beans_xml(doc));
}

TEST(SpringXmlTest, IsSpringBeansXml_DtdStyleNoBeanChild_Rejected)
{
    // No-namespace `<beans/>` with zero `<bean>` children is NOT
    // classified as Spring — rules out random XML reusing the name.
    const auto doc = parse_or_fail(R"(<beans><config/></beans>)");
    EXPECT_FALSE(is_spring_beans_xml(doc));
}

TEST(SpringXmlTest, IsSpringBeansXml_NonSpringNamespaceWithBeansRoot_Rejected)
{
    // Right local name, wrong namespace URI — not Spring.
    const auto doc = parse_or_fail(R"(
<?xml version="1.0"?>
<beans xmlns="http://other.example.com/beans">
    <bean id="x"/>
</beans>
)");
    EXPECT_FALSE(is_spring_beans_xml(doc));
}

// --- parse_spring_xml — beans -------------------------------------------

TEST(SpringXmlTest, ParseBeans_ExtractsIdAndClass)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans">
    <bean id="dataSource" class="com.example.DataSource"/>
    <bean class="com.example.NoIdBean"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.beans.size(), 2U);
    EXPECT_EQ(out.beans[0].id.value_or(""), "dataSource");
    EXPECT_EQ(out.beans[0].fqcn, "com.example.DataSource");
    EXPECT_FALSE(out.beans[1].id.has_value());
    EXPECT_EQ(out.beans[1].fqcn, "com.example.NoIdBean");
}

TEST(SpringXmlTest, ParseBeans_SkipsBeansWithoutClassAttribute)
{
    // Spring allows <bean factory-method="..."> with no class — no
    // FQCN to emit, so the parser drops the entry entirely.
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans">
    <bean id="x" factory-method="create"/>
    <bean id="y" class="com.example.Real"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.beans.size(), 1U);
    EXPECT_EQ(out.beans[0].fqcn, "com.example.Real");
}

// --- parse_spring_xml — imports -----------------------------------------

TEST(SpringXmlTest, ParseImports_PreservesResourceVerbatim)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans">
    <import resource="classpath:META-INF/spring/inner.xml"/>
    <import resource="../shared/common.xml"/>
    <import resource="${env.dir}/dynamic.xml"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.imports.size(), 3U);
    EXPECT_EQ(out.imports[0].resource, "classpath:META-INF/spring/inner.xml");
    EXPECT_EQ(out.imports[1].resource, "../shared/common.xml");
    EXPECT_EQ(out.imports[2].resource, "${env.dir}/dynamic.xml");
}

// --- parse_spring_xml — component-scan ---------------------------------

TEST(SpringXmlTest, ParseComponentScan_SinglePackage)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
    <context:component-scan base-package="com.example.svc"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.scans.size(), 1U);
    ASSERT_EQ(out.scans[0].packages.size(), 1U);
    EXPECT_EQ(out.scans[0].packages[0], "com.example.svc");
}

TEST(SpringXmlTest, ParseComponentScan_CommaSplitsToMultiplePackages)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
    <context:component-scan base-package="com.example.svc,com.example.repo"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.scans.size(), 1U);
    ASSERT_EQ(out.scans[0].packages.size(), 2U);
    EXPECT_EQ(out.scans[0].packages[0], "com.example.svc");
    EXPECT_EQ(out.scans[0].packages[1], "com.example.repo");
}

TEST(SpringXmlTest, ParseComponentScan_MultilineWhitespaceTrimmed)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
    <context:component-scan base-package="com.example.foo,
                                          com.example.bar"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.scans.size(), 1U);
    ASSERT_EQ(out.scans[0].packages.size(), 2U);
    EXPECT_EQ(out.scans[0].packages[0], "com.example.foo");
    EXPECT_EQ(out.scans[0].packages[1], "com.example.bar");
}

TEST(SpringXmlTest, ParseComponentScan_EmptySegmentsDropped)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
    <context:component-scan base-package=",com.example.foo,,com.example.bar,"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.scans.size(), 1U);
    ASSERT_EQ(out.scans[0].packages.size(), 2U);
    EXPECT_EQ(out.scans[0].packages[0], "com.example.foo");
    EXPECT_EQ(out.scans[0].packages[1], "com.example.bar");
}

TEST(SpringXmlTest, ParseComponentScan_PlaceholderPreservedAsLiteral)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
    <context:component-scan base-package="${app.scan.root}"/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    ASSERT_EQ(out.scans.size(), 1U);
    ASSERT_EQ(out.scans[0].packages.size(), 1U);
    EXPECT_EQ(out.scans[0].packages[0], "${app.scan.root}");
}

TEST(SpringXmlTest, ParseSpringXml_IgnoresAopAndTxNamespaces)
{
    const auto doc = parse_or_fail(R"(
<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:aop="http://www.springframework.org/schema/aop"
       xmlns:tx="http://www.springframework.org/schema/tx">
    <bean id="x" class="com.example.Foo"/>
    <aop:config><aop:aspect ref="x"/></aop:config>
    <tx:annotation-driven/>
</beans>
)");
    const ParsedSpringXml out = parse_spring_xml(doc);
    EXPECT_EQ(out.beans.size(), 1U);
    EXPECT_EQ(out.imports.size(), 0U);
    EXPECT_EQ(out.scans.size(), 0U);
}

// --- fqcn_without_nested ------------------------------------------------

TEST(SpringXmlTest, FqcnWithoutNested_NoDollar_Unchanged)
{
    EXPECT_EQ(fqcn_without_nested("com.example.Foo"), "com.example.Foo");
    EXPECT_EQ(fqcn_without_nested("Foo"), "Foo");
    EXPECT_EQ(fqcn_without_nested(""), "");
}

TEST(SpringXmlTest, FqcnWithoutNested_StripsAtFirstDollar)
{
    EXPECT_EQ(fqcn_without_nested("com.example.Outer$Inner"), "com.example.Outer");
    EXPECT_EQ(fqcn_without_nested("com.example.Outer$Inner$Deep"), "com.example.Outer");
}
