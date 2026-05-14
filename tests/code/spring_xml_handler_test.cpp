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

} // namespace
