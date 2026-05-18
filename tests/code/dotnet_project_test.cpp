#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "code/dotnet_project.h"
#include "code/xml_reader.h"

namespace {

using vectis::code::dotnet::CsprojData;
using vectis::code::dotnet::is_csharp_family_extension;
using vectis::code::dotnet::is_csharp_family_guid;
using vectis::code::dotnet::MsbuildContext;
using vectis::code::dotnet::parse_csproj;
using vectis::code::dotnet::parse_packages_props;
using vectis::code::dotnet::parse_sln_text;
using vectis::code::dotnet::parse_slnx;
using vectis::code::dotnet::PropertyMap;
using vectis::code::dotnet::SolutionProjectEntry;
using vectis::code::dotnet::substitute_msbuild_builtins;
using vectis::code::xml::parse;

CsprojData parse_csproj_or_die(std::string_view xml_content)
{
    auto doc = parse(xml_content);
    if (!doc) {
        ADD_FAILURE() << "XML parse failed: " << doc.error().message;
        return {};
    }
    return parse_csproj(doc->root());
}

PropertyMap parse_packages_or_die(std::string_view xml_content)
{
    auto doc = parse(xml_content);
    if (!doc) {
        ADD_FAILURE() << "XML parse failed: " << doc.error().message;
        return {};
    }
    return parse_packages_props(doc->root());
}

std::vector<SolutionProjectEntry> parse_slnx_or_die(std::string_view xml_content)
{
    auto doc = parse(xml_content);
    if (!doc) {
        ADD_FAILURE() << "XML parse failed: " << doc.error().message;
        return {};
    }
    return parse_slnx(doc->root());
}

// ----- csproj --------------------------------------------------------

TEST(ParseCsprojTest, ExtractsProjectReferences)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <ProjectReference Include="..\Lib\Lib.csproj" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.project_references.size(), 1U);
    EXPECT_EQ(data.project_references[0].include_path, "..\\Lib\\Lib.csproj");
}

TEST(ParseCsprojTest, ExtractsPackageReferenceWithInlineVersion)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Newtonsoft.Json" Version="13.0.3" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.package_references.size(), 1U);
    EXPECT_EQ(data.package_references[0].name, "Newtonsoft.Json");
    EXPECT_EQ(data.package_references[0].version, "13.0.3");
}

TEST(ParseCsprojTest, PackageReferenceWithoutVersionLeavesVersionEmpty)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Newtonsoft.Json" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.package_references.size(), 1U);
    EXPECT_EQ(data.package_references[0].name, "Newtonsoft.Json");
    EXPECT_TRUE(data.package_references[0].version.empty())
        << "no Version attribute = empty; handler resolves via CPM later";
}

TEST(ParseCsprojTest, ExtractsImports)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(RepoRoot)src\Common.props" />
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.imports.size(), 1U);
    EXPECT_EQ(data.imports[0].project_path, "$(RepoRoot)src\\Common.props");
}

TEST(ParseCsprojTest, FiltersPackageReferenceRemoveEntries)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Keep.Me" Version="1.0" />
    <PackageReference Remove="Drop.Me" />
    <PackageReference Update="Update.Me" Version="2.0" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.package_references.size(), 1U)
        << "only Include= rows emit; Remove/Update are filtered out";
    EXPECT_EQ(data.package_references[0].name, "Keep.Me");
}

TEST(ParseCsprojTest, FiltersProjectReferenceRemoveEntries)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <ProjectReference Include="..\Keep\Keep.csproj" />
    <ProjectReference Remove="..\Drop\Drop.csproj" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.project_references.size(), 1U);
    EXPECT_EQ(data.project_references[0].include_path, "..\\Keep\\Keep.csproj");
}

TEST(ParseCsprojTest, SubstitutesPropertyGroupValueIntoVersionAttribute)
{
    // Same-file <PropertyGroup> resolution against the csproj's own
    // properties applies to the Version attribute of PackageReference.
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <NewtonsoftVersion>13.0.3</NewtonsoftVersion>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Newtonsoft.Json" Version="$(NewtonsoftVersion)" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.package_references.size(), 1U);
    EXPECT_EQ(data.package_references[0].version, "13.0.3")
        << "$(NewtonsoftVersion) must resolve via the csproj's own <PropertyGroup>";
}

TEST(ParseCsprojTest, UnresolvedPlaceholderPassesThroughLiteral)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <ItemGroup>
    <PackageReference Include="Foo" Version="$(UnknownVar)" />
  </ItemGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    ASSERT_EQ(data.package_references.size(), 1U);
    EXPECT_EQ(data.package_references[0].version, "$(UnknownVar)")
        << "unknown $() placeholder must round-trip verbatim for agent visibility";
}

TEST(ParseCsprojTest, HandlesMultiplePropertyGroupBlocks)
{
    constexpr std::string_view input = R"xml(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
  <PropertyGroup>
    <Foo>FOO_VALUE</Foo>
  </PropertyGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    EXPECT_EQ(data.properties.at("TargetFramework"), "net8.0");
    EXPECT_EQ(data.properties.at("Foo"), "FOO_VALUE");
}

TEST(ParseCsprojTest, CapturesRootSdkAttribute)
{
    // The root `<Project Sdk="...">` attribute is required to detect
    // SDK-only WPF / WinForms apps that carry no PackageReference.
    constexpr std::string_view input =
        R"xml(<Project Sdk="Microsoft.NET.Sdk.WindowsDesktop">
  <PropertyGroup>
    <UseWPF>true</UseWPF>
  </PropertyGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    EXPECT_EQ(data.properties.at("__SdkAttribute"), "Microsoft.NET.Sdk.WindowsDesktop");
    EXPECT_EQ(data.properties.at("UseWPF"), "true");
}

TEST(ParseCsprojTest, MissingSdkAttributeLeavesNoSyntheticKey)
{
    constexpr std::string_view input = R"xml(<Project>
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
</Project>)xml";
    const auto data = parse_csproj_or_die(input);
    EXPECT_EQ(data.properties.find("__SdkAttribute"), data.properties.end());
}

// ----- packages.props (CPM) ------------------------------------------

TEST(ParsePackagesPropsTest, ExtractsPackageVersionEntries)
{
    constexpr std::string_view input = R"xml(<Project>
  <ItemGroup>
    <PackageVersion Include="Newtonsoft.Json" Version="13.0.3" />
    <PackageVersion Include="MSTest" Version="3.8.3" />
  </ItemGroup>
</Project>)xml";
    const auto map = parse_packages_or_die(input);
    EXPECT_EQ(map.at("Newtonsoft.Json"), "13.0.3");
    EXPECT_EQ(map.at("MSTest"), "3.8.3");
}

TEST(ParsePackagesPropsTest, ResolvesPropertyGroupVarInVersionAttribute)
{
    // Real-world CPM files routinely declare <MSTestVersion>3.8.3</...>
    // followed by Version="$(MSTestVersion)" — must resolve.
    constexpr std::string_view input = R"xml(<Project>
  <PropertyGroup>
    <MSTestVersion>3.8.3</MSTestVersion>
  </PropertyGroup>
  <ItemGroup>
    <PackageVersion Include="MSTest" Version="$(MSTestVersion)" />
  </ItemGroup>
</Project>)xml";
    const auto map = parse_packages_or_die(input);
    EXPECT_EQ(map.at("MSTest"), "3.8.3");
}

TEST(ParsePackagesPropsTest, UnresolvedPlaceholderPassesThroughLiteral)
{
    constexpr std::string_view input = R"xml(<Project>
  <ItemGroup>
    <PackageVersion Include="Foo" Version="$(UnknownVar)" />
  </ItemGroup>
</Project>)xml";
    const auto map = parse_packages_or_die(input);
    EXPECT_EQ(map.at("Foo"), "$(UnknownVar)");
}

// ----- sln (text) ----------------------------------------------------

TEST(ParseSlnTextTest, ExtractsProjectLinesAndGuids)
{
    constexpr std::string_view input =
        R"sln(Microsoft Visual Studio Solution File, Format Version 12.00
Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = "App", "App\App.csproj", "{12345678-1234-1234-1234-123456789012}"
EndProject
Project("{F184B08F-C81C-45F6-A57F-5ABD9991F28F}") = "Lib", "Lib\Lib.vbproj", "{ABCDABCD-ABCD-ABCD-ABCD-ABCDABCDABCD}"
EndProject
Global
EndGlobal
)sln";
    const auto entries = parse_sln_text(input);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].name, "App");
    EXPECT_EQ(entries[0].path, "App\\App.csproj");
    EXPECT_EQ(entries[0].project_type_guid, "FAE04EC0-301F-11D3-BF4B-00C04F79EFBC");
    EXPECT_EQ(entries[1].name, "Lib");
    EXPECT_EQ(entries[1].project_type_guid, "F184B08F-C81C-45F6-A57F-5ABD9991F28F");
}

TEST(ParseSlnTextTest, SkipsLinesThatAreNotProjectDeclarations)
{
    constexpr std::string_view input = R"sln(Microsoft Visual Studio Solution File
Project("{FAE04EC0-301F-11D3-BF4B-00C04F79EFBC}") = "App", "App.csproj", "{abc}"
EndProject
GlobalSection(SolutionConfigurationPlatforms) = preSolution
EndGlobalSection
)sln";
    const auto entries = parse_sln_text(input);
    EXPECT_EQ(entries.size(), 1U);
}

TEST(IsCsharpFamilyGuidTest, MatchesKnownProjectTypes)
{
    EXPECT_TRUE(is_csharp_family_guid("FAE04EC0-301F-11D3-BF4B-00C04F79EFBC")); // C#
    EXPECT_TRUE(is_csharp_family_guid("F2A71F9B-5D33-465A-A702-920D77279786")); // F#
    EXPECT_TRUE(is_csharp_family_guid("F184B08F-C81C-45F6-A57F-5ABD9991F28F")); // VB
    EXPECT_TRUE(is_csharp_family_guid("9A19103F-16F7-4668-BE54-9A1E7A4F7556")); // C# SDK
    // Case-insensitive
    EXPECT_TRUE(is_csharp_family_guid("fae04ec0-301f-11d3-bf4b-00c04f79efbc"));
}

TEST(IsCsharpFamilyGuidTest, RejectsFolderAndCppGuids)
{
    EXPECT_FALSE(is_csharp_family_guid("2150E333-8FDC-42A3-9474-1A3956D46DE8")); // solution folder
    EXPECT_FALSE(is_csharp_family_guid("8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942")); // C++ (vcxproj)
    EXPECT_FALSE(is_csharp_family_guid(""));
}

// ----- slnx (XML) ----------------------------------------------------

TEST(ParseSlnxTest, ExtractsTopLevelProjectElements)
{
    constexpr std::string_view input = R"xml(<Solution>
  <Project Path="src/App.csproj" />
  <Project Path="src/Lib.csproj" />
</Solution>)xml";
    const auto entries = parse_slnx_or_die(input);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].path, "src/App.csproj");
    EXPECT_EQ(entries[1].path, "src/Lib.csproj");
}

TEST(ParseSlnxTest, RecursesIntoFolderNesting)
{
    constexpr std::string_view input = R"xml(<Solution>
  <Project Path="src/App.csproj" />
  <Folder Name="/Tools/">
    <Project Path="src/tools/Bar.csproj" />
    <Folder Name="/Tools/Deep/">
      <Project Path="src/tools/deep/Baz.csproj" />
    </Folder>
  </Folder>
</Solution>)xml";
    const auto entries = parse_slnx_or_die(input);
    EXPECT_EQ(entries.size(), 3U) << "<Folder> nesting must not hide projects";
}

TEST(IsCsharpFamilyExtensionTest, AcceptsCsprojFsprojVbproj)
{
    EXPECT_TRUE(is_csharp_family_extension("App.csproj"));
    EXPECT_TRUE(is_csharp_family_extension("Lib.fsproj"));
    EXPECT_TRUE(is_csharp_family_extension("Old.vbproj"));
    // Case-insensitive
    EXPECT_TRUE(is_csharp_family_extension("App.CSPROJ"));
}

TEST(IsCsharpFamilyExtensionTest, RejectsVcxprojAndOthers)
{
    EXPECT_FALSE(is_csharp_family_extension("App.vcxproj"));
    EXPECT_FALSE(is_csharp_family_extension("App.proj"));
    EXPECT_FALSE(is_csharp_family_extension("not-a-project"));
}

// ----- MSBuild built-in substitution ---------------------------------

TEST(SubstituteMsbuildBuiltinsTest, ResolvesRepoRoot)
{
    MsbuildContext ctx;
    ctx.repo_root = "/p/proj/";
    ctx.this_file_dir = "/p/proj/src/App/";
    ctx.project_name = "App";
    EXPECT_EQ(substitute_msbuild_builtins("$(RepoRoot)src/Common.props", ctx),
              "/p/proj/src/Common.props");
}

TEST(SubstituteMsbuildBuiltinsTest, ResolvesProjectDirAndMSBuildThisFileDirectory)
{
    MsbuildContext ctx;
    ctx.repo_root = "/p/proj/";
    ctx.this_file_dir = "/p/proj/src/App/";
    ctx.project_name = "App";
    EXPECT_EQ(substitute_msbuild_builtins("$(ProjectDir)x.props", ctx), "/p/proj/src/App/x.props");
    EXPECT_EQ(substitute_msbuild_builtins("$(MSBuildThisFileDirectory)x.props", ctx),
              "/p/proj/src/App/x.props");
}

TEST(SubstituteMsbuildBuiltinsTest, ResolvesProjectName)
{
    MsbuildContext ctx;
    ctx.project_name = "MyLib";
    EXPECT_EQ(substitute_msbuild_builtins("$(MSBuildProjectName).g.cs", ctx), "MyLib.g.cs");
}

TEST(SubstituteMsbuildBuiltinsTest, UnknownPropertyPassesThroughLiteral)
{
    MsbuildContext ctx;
    EXPECT_EQ(substitute_msbuild_builtins("$(SomethingCustom)/file.props", ctx),
              "$(SomethingCustom)/file.props");
}

} // namespace
