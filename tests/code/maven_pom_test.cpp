#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "code/maven_pom.h"
#include "code/xml_reader.h"
#include "core/result.h"

namespace {

using vectis::code::maven::Coordinate;
using vectis::code::maven::Dependency;
using vectis::code::maven::parse_pom;
using vectis::code::maven::ParsedPom;
using vectis::code::maven::substitute_properties;
using vectis::code::xml::Document;
using vectis::code::xml::parse;

ParsedPom parse_pom_or_die(std::string_view xml_content)
{
    auto doc = parse(xml_content);
    if (!doc) {
        ADD_FAILURE() << "XML parse failed: " << doc.error().message;
        return ParsedPom{};
    }
    return parse_pom(doc->root());
}

// ----- Coordinate ----------------------------------------------------

TEST(MavenCoordinateTest, GavConcatenatesGroupArtifactVersion)
{
    const Coordinate c{"com.example", "lib", "1.0"};
    EXPECT_EQ(c.gav(), "com.example:lib:1.0");
}

TEST(MavenCoordinateTest, EmptyCoordinateRendersWithColonsForVisibility)
{
    // gav() is for agent display; an empty coordinate shouldn't pretend
    // to be a real coordinate, but it also shouldn't crash. The format
    // pins the shape so the handler can recognise unresolved coords by
    // their `${` prefix or by emptiness.
    const Coordinate empty;
    EXPECT_TRUE(empty.empty());
}

// ----- parse_pom -----------------------------------------------------

TEST(ParsePomTest, ParsesRootGroupArtifactVersion)
{
    constexpr std::string_view input = R"(<project>
  <groupId>com.example</groupId>
  <artifactId>app</artifactId>
  <version>1.0.0</version>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_EQ(pom.coord, (Coordinate{"com.example", "app", "1.0.0"}));
}

TEST(ParsePomTest, ChildInheritsGroupIdFromParent)
{
    constexpr std::string_view input = R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>parent</artifactId>
    <version>1.0.0</version>
  </parent>
  <artifactId>child</artifactId>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_EQ(pom.coord.group_id, "com.example") << "groupId inherits from <parent>";
    EXPECT_EQ(pom.coord.artifact_id, "child");
    // Version also inherits if the child omits it.
    EXPECT_EQ(pom.coord.version, "1.0.0");
}

TEST(ParsePomTest, ChildOverridesGroupIdWhenDeclaredExplicitly)
{
    constexpr std::string_view input = R"(<project>
  <parent>
    <groupId>com.example.parent</groupId>
    <artifactId>parent</artifactId>
    <version>1.0.0</version>
  </parent>
  <groupId>com.example.child</groupId>
  <artifactId>child</artifactId>
  <version>2.0.0</version>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_EQ(pom.coord.group_id, "com.example.child");
    EXPECT_EQ(pom.coord.version, "2.0.0");
}

TEST(ParsePomTest, PackagingDefaultsToJar)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_EQ(pom.packaging, "jar");
}

TEST(ParsePomTest, PackagingExplicit)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <packaging>pom</packaging>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_EQ(pom.packaging, "pom");
}

TEST(ParsePomTest, ExtractsProperties)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <properties>
    <junit.version>5.10.0</junit.version>
    <slf4j.version>2.0.7</slf4j.version>
  </properties>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.properties.size(), 2U);
    EXPECT_EQ(pom.properties.at("junit.version"), "5.10.0");
    EXPECT_EQ(pom.properties.at("slf4j.version"), "2.0.7");
}

TEST(ParsePomTest, ExtractsModules)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>parent</artifactId>
  <modules>
    <module>app</module>
    <module>lib</module>
  </modules>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.modules.size(), 2U);
    EXPECT_EQ(pom.modules[0], "app");
    EXPECT_EQ(pom.modules[1], "lib");
}

TEST(ParsePomTest, ParentRelativePathDefaultsToDotDotPom)
{
    constexpr std::string_view input = R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>parent</artifactId>
    <version>1.0.0</version>
  </parent>
  <artifactId>child</artifactId>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_TRUE(pom.parent_relative_path.has_value());
    EXPECT_EQ(*pom.parent_relative_path, "../pom.xml")
        << "missing <relativePath> must default to ../pom.xml per Maven semantics";
}

TEST(ParsePomTest, ParentRelativePathHonoursExplicitValue)
{
    constexpr std::string_view input = R"(<project>
  <parent>
    <groupId>com.example</groupId>
    <artifactId>parent</artifactId>
    <version>1.0.0</version>
    <relativePath>../../shared/pom.xml</relativePath>
  </parent>
  <artifactId>child</artifactId>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_TRUE(pom.parent_relative_path.has_value());
    EXPECT_EQ(*pom.parent_relative_path, "../../shared/pom.xml");
}

TEST(ParsePomTest, NoParentMeansNoParentRelativePath)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
</project>)";
    const auto pom = parse_pom_or_die(input);
    EXPECT_FALSE(pom.parent.has_value());
    EXPECT_FALSE(pom.parent_relative_path.has_value());
}

TEST(ParsePomTest, ExtractsTopLevelDependencies)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <dependencies>
    <dependency>
      <groupId>com.example</groupId>
      <artifactId>lib</artifactId>
      <version>1.0.0</version>
    </dependency>
    <dependency>
      <groupId>org.junit</groupId>
      <artifactId>junit-jupiter</artifactId>
      <version>5.10.0</version>
    </dependency>
  </dependencies>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.dependencies.size(), 2U);
    EXPECT_EQ(pom.dependencies[0].location, Dependency::Location::TopLevel);
    EXPECT_FALSE(pom.dependencies[0].is_bom);
    EXPECT_EQ(pom.dependencies[0].coord.artifact_id, "lib");
    EXPECT_EQ(pom.dependencies[1].coord.artifact_id, "junit-jupiter");
}

TEST(ParsePomTest, ExtractsManagedDependencies)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <dependencyManagement>
    <dependencies>
      <dependency>
        <groupId>org.junit</groupId>
        <artifactId>junit-jupiter</artifactId>
        <version>5.10.0</version>
      </dependency>
    </dependencies>
  </dependencyManagement>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.dependencies.size(), 1U);
    EXPECT_EQ(pom.dependencies[0].location, Dependency::Location::Managed);
    EXPECT_FALSE(pom.dependencies[0].is_bom);
}

TEST(ParsePomTest, BomMarkersInsideDependencyManagementSetIsBom)
{
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
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
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.dependencies.size(), 1U);
    EXPECT_TRUE(pom.dependencies[0].is_bom) << "<type>pom</type> + <scope>import</scope> markers "
                                               "must set is_bom regardless of location";
    EXPECT_EQ(pom.dependencies[0].location, Dependency::Location::Managed);
}

TEST(ParsePomTest, BomMarkersAtTopLevelStillSetIsBom)
{
    // Rare but legal: BOM markers under top-level <dependencies>. Per
    // the Phase 1 spec, kind is decided by markers, not location.
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <dependencies>
    <dependency>
      <groupId>org.junit</groupId>
      <artifactId>junit-bom</artifactId>
      <version>5.10.0</version>
      <type>pom</type>
      <scope>import</scope>
    </dependency>
  </dependencies>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.dependencies.size(), 1U);
    EXPECT_TRUE(pom.dependencies[0].is_bom);
    EXPECT_EQ(pom.dependencies[0].location, Dependency::Location::TopLevel);
}

TEST(ParsePomTest, DependenciesInsidePluginsAreIgnored)
{
    // <plugin><dependencies><dependency>...</dependency></dependencies></plugin>
    // is part of plugin configuration, not the project's dep graph.
    constexpr std::string_view input = R"(<project>
  <artifactId>app</artifactId>
  <build>
    <plugins>
      <plugin>
        <groupId>org.apache.maven.plugins</groupId>
        <artifactId>maven-compiler-plugin</artifactId>
        <dependencies>
          <dependency>
            <groupId>org.example</groupId>
            <artifactId>plugin-helper</artifactId>
            <version>1.0</version>
          </dependency>
        </dependencies>
      </plugin>
    </plugins>
  </build>
  <dependencies>
    <dependency>
      <groupId>com.example</groupId>
      <artifactId>real-dep</artifactId>
      <version>1.0</version>
    </dependency>
  </dependencies>
</project>)";
    const auto pom = parse_pom_or_die(input);
    ASSERT_EQ(pom.dependencies.size(), 1U) << "<plugin> dependencies must be filtered out";
    EXPECT_EQ(pom.dependencies[0].coord.artifact_id, "real-dep");
}

// ----- substitute_properties -----------------------------------------

TEST(SubstitutePropertiesTest, ResolvesProjectVersion)
{
    const Coordinate own{"com.example", "app", "1.0.0"};
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("${project.version}", own, empty, empty), "1.0.0");
}

TEST(SubstitutePropertiesTest, ResolvesProjectGroupId)
{
    const Coordinate own{"com.example", "app", "1.0.0"};
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("${project.groupId}", own, empty, empty), "com.example");
}

TEST(SubstitutePropertiesTest, ResolvesFromOwnProperties)
{
    const Coordinate own;
    const std::map<std::string, std::string> own_props{{"junit.version", "5.10.0"}};
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("${junit.version}", own, own_props, empty), "5.10.0");
}

TEST(SubstitutePropertiesTest, FallsBackToParentProperties)
{
    const Coordinate own;
    const std::map<std::string, std::string> empty;
    const std::map<std::string, std::string> parent_props{{"slf4j.version", "2.0.7"}};
    EXPECT_EQ(substitute_properties("${slf4j.version}", own, empty, parent_props), "2.0.7");
}

TEST(SubstitutePropertiesTest, OwnPropertyShadowsParent)
{
    const Coordinate own;
    const std::map<std::string, std::string> own_props{{"v", "OWN"}};
    const std::map<std::string, std::string> parent_props{{"v", "PARENT"}};
    EXPECT_EQ(substitute_properties("${v}", own, own_props, parent_props), "OWN");
}

TEST(SubstitutePropertiesTest, LeavesUnknownPlaceholderAsLiteral)
{
    const Coordinate own;
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("${missing.thing}", own, empty, empty), "${missing.thing}");
}

TEST(SubstitutePropertiesTest, ResolvesMultiplePlaceholdersInSameString)
{
    const Coordinate own{"com.example", "app", "1.0.0"};
    const std::map<std::string, std::string> own_props{{"v", "9"}};
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("${project.groupId}:lib:${v}-${project.version}", own,
                                    own_props, empty),
              "com.example:lib:9-1.0.0");
}

TEST(SubstitutePropertiesTest, LiteralStringWithoutPlaceholdersPassesThrough)
{
    const Coordinate own;
    const std::map<std::string, std::string> empty;
    EXPECT_EQ(substitute_properties("plain-1.0.0", own, empty, empty), "plain-1.0.0");
}

} // namespace
