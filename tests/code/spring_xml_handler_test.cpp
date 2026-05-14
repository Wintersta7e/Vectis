#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/manifest_scanner.h"
#include "code/spring_xml_handler.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::FileEntry;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::manifest_scanner::Config;
using vectis::code::manifest_scanner::scan_manifests;
using vectis::code::spring::make_spring_xml_handler;

/// Temp directory + file-writing helpers, mirroring PomHandlerFixture.
class SpringXmlHandlerFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_root =
            std::filesystem::temp_directory_path() / (std::string{"vectis_spring_"} + test_name);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    void TearDown() override { std::filesystem::remove_all(m_root); }

    void write_file(const std::filesystem::path& relative, std::string_view body) const
    {
        const auto full = m_root / relative;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream stream(full);
        stream << body;
    }

    void run_handler(CodeIndex& index) const
    {
        Config config;
        config.root = m_root;
        std::unordered_set<std::string> visited;
        scan_manifests(config, index, visited, {make_spring_xml_handler()});
    }

    [[nodiscard]] std::vector<Dependency> deps_of(const CodeIndex& index,
                                                  const std::string& path) const
    {
        const auto id = index.file_id_for_path(path);
        return id == 0 ? std::vector<Dependency>{} : index.dependencies_of(id);
    }

    /// Seed a synthetic .java file row so match_java_dotted_candidates
    /// has something to resolve against (the Spring handler never
    /// registers .java files itself).
    static std::int64_t seed_java(CodeIndex& index, const std::string& relative)
    {
        FileEntry f;
        f.path_relative = relative;
        f.language = Language::Java;
        f.line_count = 1;
        return index.add_file(std::move(f));
    }

    std::filesystem::path m_root;
};

constexpr std::string_view k_namespaced_beans = R"(<?xml version="1.0" encoding="UTF-8"?>
<beans xmlns="http://www.springframework.org/schema/beans">
  <bean id="svc" class="com.example.svc.MyService"/>
</beans>)";

constexpr std::string_view k_dtd_beans = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE beans PUBLIC "-//SPRING//DTD BEAN 2.0//EN"
    "http://www.springframework.org/dtd/spring-beans-2.0.dtd">
<beans>
  <bean id="helper" class="org.example.external.Helper"/>
</beans>)";

TEST_F(SpringXmlHandlerFixture, NamespacedBeansFileIsRegisteredAndTagged)
{
    write_file("applicationContext.xml", k_namespaced_beans);

    CodeIndex index;
    run_handler(index);

    const auto id = index.file_id_for_path("applicationContext.xml");
    ASSERT_NE(id, 0);
    const auto files = index.snapshot_files();
    bool tagged = false;
    for (const auto& f : files) {
        if (f.id == id) {
            tagged = (f.language == Language::SpringXml);
        }
    }
    EXPECT_TRUE(tagged) << "registered Spring XML must carry Language::SpringXml";

    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_EQ(symbols[0].kind, SymbolKind::Manifest);
    EXPECT_EQ(symbols[0].name, "applicationContext");
    const bool has_kind = std::find(symbols[0].members.begin(), symbols[0].members.end(),
                                    "kind:spring-xml") != symbols[0].members.end();
    EXPECT_TRUE(has_kind);
}

TEST_F(SpringXmlHandlerFixture, DtdStyleBeansFileIsRegistered)
{
    write_file("inner-ctx.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    EXPECT_NE(index.file_id_for_path("inner-ctx.xml"), 0)
        << "DTD-style <beans> with a <bean> child must be detected without a namespace";
}

TEST_F(SpringXmlHandlerFixture, NonSpringXmlIsNotRegistered)
{
    write_file("config.xml", R"(<?xml version="1.0"?><config><setting>x</setting></config>)");

    CodeIndex index;
    run_handler(index);

    EXPECT_EQ(index.file_id_for_path("config.xml"), 0)
        << "XML with no <beans substring must be skipped by the pre-filter";
}

TEST_F(SpringXmlHandlerFixture, NonSpringNamespaceBeansRootIsNotRegistered)
{
    write_file("other.xml",
               R"(<?xml version="1.0"?><beans xmlns="http://example.com/other"><thing/></beans>)");

    CodeIndex index;
    run_handler(index);

    EXPECT_EQ(index.file_id_for_path("other.xml"), 0)
        << "a <beans> root in a foreign namespace with no <bean> child is not Spring XML";
}

TEST_F(SpringXmlHandlerFixture, BeanClassResolvesToInRepoJavaFile)
{
    write_file("applicationContext.xml", k_namespaced_beans); // bean: com.example.svc.MyService

    CodeIndex index;
    const std::int64_t java_id = seed_java(index, "src/main/java/com/example/svc/MyService.java");
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-bean" && d.import_string == "com.example.svc.MyService") {
            EXPECT_EQ(d.target_file_id, java_id) << "unique FQCN match must resolve internally";
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, BeanClassUnresolvedEmitsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <bean class="org.nowhere.Absent"/>
</beans>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-bean" && d.import_string == "org.nowhere.Absent") {
            EXPECT_EQ(d.target_file_id, 0);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, BeanClassMultipleSuffixMatchesEmitsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <bean class="com.x.Foo"/>
</beans>)");

    CodeIndex index;
    seed_java(index, "src/main/java/com/x/Foo.java");
    seed_java(index, "src/test/java/com/x/Foo.java");
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-bean" && d.import_string == "com.x.Foo") {
            EXPECT_EQ(d.target_file_id, 0) << "two suffix matches => not unique => external";
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, BeanNestedClassStripsForMatchButKeepsFullFqcn)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <bean class="com.x.Outer$Inner"/>
</beans>)");

    CodeIndex index;
    const std::int64_t outer_id = seed_java(index, "src/main/java/com/x/Outer.java");
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-bean") {
            EXPECT_EQ(d.import_string, "com.x.Outer$Inner")
                << "edge keeps the full nested-class FQCN for the agent";
            EXPECT_EQ(d.target_file_id, outer_id)
                << "resolution strips $Inner so it matches Outer.java";
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ComponentScanCommaSeparatedSplitsToMultipleEdges)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans"
       xmlns:context="http://www.springframework.org/schema/context">
  <context:component-scan base-package="com.example.svc, com.example.repo"/>
</beans>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    std::vector<std::string> scan_pkgs;
    for (const auto& d : deps) {
        if (d.kind == "spring-component-scan") {
            EXPECT_EQ(d.target_file_id, 0) << "component-scan edges are always external";
            scan_pkgs.push_back(d.import_string);
        }
    }
    ASSERT_EQ(scan_pkgs.size(), 2U);
    EXPECT_NE(std::find(scan_pkgs.begin(), scan_pkgs.end(), "com.example.svc"), scan_pkgs.end());
    EXPECT_NE(std::find(scan_pkgs.begin(), scan_pkgs.end(), "com.example.repo"), scan_pkgs.end());
}

TEST_F(SpringXmlHandlerFixture, ImportClasspathResolvesToRegisteredSpringXml)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="classpath:inner.xml"/>
</beans>)");
    write_file("src/main/resources/inner.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    const auto inner_id = index.file_id_for_path("src/main/resources/inner.xml");
    ASSERT_NE(inner_id, 0);
    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.import_string, "classpath:inner.xml") << "raw resource value preserved";
            EXPECT_EQ(d.target_file_id, inner_id);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportClasspathAmbiguousMatchEmitsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="classpath:inner.xml"/>
</beans>)");
    write_file("src/main/resources/inner.xml", k_dtd_beans);
    write_file("src/test/resources/inner.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, 0) << "two suffix matches => ambiguous => external";
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportClasspathStarPatternIsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="classpath*:META-INF/spring/*.xml"/>
</beans>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, 0);
            EXPECT_EQ(d.import_string, "classpath*:META-INF/spring/*.xml");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportClasspathMidComponentSuffixEmitsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="classpath:context.xml"/>
</beans>)");
    // "my-context.xml" ends with "context.xml" but not on a '/' boundary --
    // a mid-component suffix match must not count as a hit.
    write_file("src/main/resources/my-context.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, 0)
                << "suffix match mid-component (my-context.xml) must not resolve";
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportRelativePathResolves)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="sub/inner.xml"/>
</beans>)");
    write_file("sub/inner.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    const auto inner_id = index.file_id_for_path("sub/inner.xml");
    ASSERT_NE(inner_id, 0);
    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, inner_id);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportLeadingSlashResolvesFromRoot)
{
    write_file("nested/applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="/shared/inner.xml"/>
</beans>)");
    write_file("shared/inner.xml", k_dtd_beans);

    CodeIndex index;
    run_handler(index);

    const auto inner_id = index.file_id_for_path("shared/inner.xml");
    ASSERT_NE(inner_id, 0);
    const auto deps = deps_of(index, "nested/applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, inner_id);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SpringXmlHandlerFixture, ImportPlaceholderIsExternal)
{
    write_file("applicationContext.xml",
               R"(<beans xmlns="http://www.springframework.org/schema/beans">
  <import resource="${config.dir}/inner.xml"/>
</beans>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "applicationContext.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "spring-import") {
            EXPECT_EQ(d.target_file_id, 0);
            EXPECT_EQ(d.import_string, "${config.dir}/inner.xml");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

} // namespace
