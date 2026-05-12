#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/manifest_scanner.h"
#include "code/maven_pom_handler.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::manifest_scanner::Config;
using vectis::code::manifest_scanner::scan_manifests;
using vectis::code::maven::make_pom_handler;

/// Temp directory + file-writing helpers, mirroring ScannerFixture's
/// pattern. Cleaned up on test exit.
class PomHandlerFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        m_root = std::filesystem::temp_directory_path() / (std::string{"vectis_pom_"} + test_name);
        std::filesystem::remove_all(m_root);
        std::filesystem::create_directories(m_root);
    }

    void TearDown() override { std::filesystem::remove_all(m_root); }

    void write_pom(const std::filesystem::path& relative, std::string_view body) const
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
        scan_manifests(config, index, visited, {make_pom_handler()});
    }

    [[nodiscard]] std::vector<Dependency> deps_of(const CodeIndex& index,
                                                  const std::string& path) const
    {
        const auto id = index.file_id_for_path(path);
        return id == 0 ? std::vector<Dependency>{} : index.dependencies_of(id);
    }

    std::filesystem::path m_root;
};

TEST_F(PomHandlerFixture, ModulesEdgesPointToChildPoms)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>root</artifactId>
  <version>1.0</version>
  <packaging>pom</packaging>
  <modules>
    <module>app</module>
    <module>lib</module>
  </modules>
</project>)");
    write_pom("app/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>app</artifactId>
</project>)");
    write_pom("lib/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>lib</artifactId>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto root_deps = deps_of(index, "pom.xml");
    int module_edges = 0;
    for (const auto& d : root_deps) {
        if (d.kind == "maven-module") {
            ++module_edges;
            EXPECT_NE(d.target_file_id, 0) << "module edges must resolve internally";
        }
    }
    EXPECT_EQ(module_edges, 2);
}

TEST_F(PomHandlerFixture, ParentResolvesInternallyViaDefaultRelativePath)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>root</artifactId>
  <version>1.0</version>
  <packaging>pom</packaging>
</project>)");
    write_pom("app/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>app</artifactId>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto app_deps = deps_of(index, "app/pom.xml");
    bool found = false;
    for (const auto& d : app_deps) {
        if (d.kind == "maven-parent") {
            EXPECT_NE(d.target_file_id, 0)
                << "missing <relativePath> must default to ../pom.xml and resolve";
            EXPECT_EQ(d.import_string, "com.example:root:1.0");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(PomHandlerFixture, DependencyResolvesInternallyToSiblingByCoordinate)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>root</artifactId>
  <version>1.0</version>
  <packaging>pom</packaging>
  <modules>
    <module>app</module>
    <module>lib</module>
  </modules>
</project>)");
    write_pom("app/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>app</artifactId>
  <dependencies>
    <dependency>
      <groupId>com.example</groupId>
      <artifactId>lib</artifactId>
      <version>1.0</version>
    </dependency>
  </dependencies>
</project>)");
    write_pom("lib/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>lib</artifactId>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto app_deps = deps_of(index, "app/pom.xml");
    const auto lib_id = index.file_id_for_path("lib/pom.xml");
    ASSERT_NE(lib_id, 0);

    bool found = false;
    for (const auto& d : app_deps) {
        if (d.kind == "maven" && d.target_file_id == lib_id) {
            EXPECT_EQ(d.import_string, "com.example:lib:1.0");
            found = true;
        }
    }
    EXPECT_TRUE(found) << "<dependency> to in-repo coordinate must resolve internally";
}

TEST_F(PomHandlerFixture, ExternalDependencyKeepsGavInImportString)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>app</artifactId>
  <version>1.0</version>
  <dependencies>
    <dependency>
      <groupId>org.junit</groupId>
      <artifactId>junit-jupiter</artifactId>
      <version>5.10.0</version>
    </dependency>
  </dependencies>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "pom.xml");
    bool found = false;
    for (const auto& d : deps) {
        if (d.kind == "maven" && d.target_file_id == 0) {
            EXPECT_EQ(d.import_string, "org.junit:junit-jupiter:5.10.0");
            found = true;
        }
    }
    EXPECT_TRUE(found) << "foreign coordinate must surface as external maven edge";
}

TEST_F(PomHandlerFixture, DependencyManagementEntryEmitsManagedKind)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>app</artifactId>
  <version>1.0</version>
  <dependencyManagement>
    <dependencies>
      <dependency>
        <groupId>org.junit</groupId>
        <artifactId>junit-jupiter</artifactId>
        <version>5.10.0</version>
      </dependency>
    </dependencies>
  </dependencyManagement>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "pom.xml");
    bool managed = false;
    for (const auto& d : deps) {
        if (d.kind == "maven-managed") {
            managed = true;
        }
        EXPECT_NE(d.kind, "maven") << "<dependencyManagement> entries must NOT emit maven kind";
    }
    EXPECT_TRUE(managed);
}

TEST_F(PomHandlerFixture, BomImportInsideDependencyManagementEmitsMavenBomNotManaged)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>app</artifactId>
  <version>1.0</version>
  <dependencyManagement>
    <dependencies>
      <dependency>
        <groupId>org.junit</groupId>
        <artifactId>junit-bom</artifactId>
        <version>5.10.0</version>
        <type>pom</type>
        <scope>import</scope>
      </dependency>
    </dependencies>
  </dependencyManagement>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "pom.xml");
    bool bom = false;
    for (const auto& d : deps) {
        if (d.kind == "maven-bom") {
            EXPECT_EQ(d.target_file_id, 0) << "BOM imports are always external";
            EXPECT_EQ(d.import_string, "org.junit:junit-bom:5.10.0");
            bom = true;
        }
        EXPECT_NE(d.kind, "maven-managed")
            << "BOM markers override <dependencyManagement> classification";
    }
    EXPECT_TRUE(bom);
}

TEST_F(PomHandlerFixture, OneHopParentPropertySubstitution)
{
    // Parent declares the version property; child references it
    // inside its own <dependency><version>. The handler must read
    // parent's <properties> for the substitution.
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>root</artifactId>
  <version>1.0</version>
  <packaging>pom</packaging>
  <properties>
    <junit.version>5.10.0</junit.version>
  </properties>
  <modules><module>app</module></modules>
</project>)");
    write_pom("app/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>root</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>app</artifactId>
  <dependencies>
    <dependency>
      <groupId>org.junit</groupId>
      <artifactId>junit-jupiter</artifactId>
      <version>${junit.version}</version>
    </dependency>
  </dependencies>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "app/pom.xml");
    bool resolved = false;
    for (const auto& d : deps) {
        if (d.kind == "maven") {
            EXPECT_EQ(d.import_string, "org.junit:junit-jupiter:5.10.0")
                << "one-hop parent property must substitute into child's <version>";
            resolved = true;
        }
    }
    EXPECT_TRUE(resolved);
}

TEST_F(PomHandlerFixture, GrandparentPropertiesAreNotRecursed)
{
    // Grandparent has the property; parent does NOT. Child references
    // it. Spec: one hop only, so this stays literal.
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>grand</artifactId>
  <version>1.0</version>
  <packaging>pom</packaging>
  <properties>
    <slf4j.version>2.0.7</slf4j.version>
  </properties>
  <modules><module>parent</module></modules>
</project>)");
    write_pom("parent/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>grand</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>parent</artifactId>
  <packaging>pom</packaging>
  <modules><module>../child</module></modules>
</project>)");
    write_pom("child/pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>parent</artifactId>
    <version>1.0</version>
    <relativePath>../parent/pom.xml</relativePath>
  </parent>
  <artifactId>child</artifactId>
  <dependencies>
    <dependency>
      <groupId>org.slf4j</groupId>
      <artifactId>slf4j-api</artifactId>
      <version>${slf4j.version}</version>
    </dependency>
  </dependencies>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto deps = deps_of(index, "child/pom.xml");
    bool found_literal = false;
    for (const auto& d : deps) {
        if (d.kind == "maven") {
            EXPECT_EQ(d.import_string, "org.slf4j:slf4j-api:${slf4j.version}")
                << "grandparent property must NOT be recursively resolved (Phase 1: one hop only)";
            found_literal = true;
        }
    }
    EXPECT_TRUE(found_literal);
}

TEST_F(PomHandlerFixture, MalformedPomIsSkippedWithoutFailingTheScan)
{
    write_pom("pom.xml",
              R"(<project>
  <groupId>com.example</groupId>
  <artifactId>root</artifactId>
  <version>1.0</version>
</project>)");
    write_pom("broken/pom.xml", "<this is not valid XML at all>>>");

    CodeIndex index;
    run_handler(index);

    // The good POM must still register; the broken one is silently
    // skipped (with a WARN log not asserted here).
    EXPECT_NE(index.file_id_for_path("pom.xml"), 0);
    EXPECT_EQ(index.file_id_for_path("broken/pom.xml"), 0);
}

TEST_F(PomHandlerFixture, AttachesManifestSymbolWithPackagingAndParentMembers)
{
    write_pom("pom.xml",
              R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>parent</artifactId>
    <version>1.0</version>
  </parent>
  <artifactId>child</artifactId>
  <packaging>war</packaging>
</project>)");

    CodeIndex index;
    run_handler(index);

    const auto id = index.file_id_for_path("pom.xml");
    ASSERT_NE(id, 0);
    const auto symbols = index.symbols_in_file(id);
    ASSERT_EQ(symbols.size(), 1U);
    EXPECT_EQ(symbols[0].kind, SymbolKind::Manifest);
    EXPECT_EQ(symbols[0].name, "com.example:child:1.0")
        << "groupId/version inherit from parent; name is post-inheritance gav()";
    const auto has_member = [&](const std::string& m) {
        return std::find(symbols[0].members.begin(), symbols[0].members.end(), m) !=
               symbols[0].members.end();
    };
    EXPECT_TRUE(has_member("packaging:war"));
    EXPECT_TRUE(has_member("parent:com.example:parent:1.0"));
}

} // namespace
