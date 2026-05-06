#include <filesystem>

#include <gtest/gtest.h>

#include "code/language.h"

namespace {

using vectis::code::detect_language;
using vectis::code::Language;
using vectis::code::language_name;
using vectis::code::refine_language;

TEST(LanguageTest, DetectsKnownExtensions)
{
    EXPECT_EQ(detect_language("/tmp/foo.py"), Language::Python);
    EXPECT_EQ(detect_language("/tmp/foo.pyi"), Language::Python);
    EXPECT_EQ(detect_language("/tmp/foo.js"), Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.jsx"), Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.mjs"), Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.cjs"), Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.ts"), Language::TypeScript);
    EXPECT_EQ(detect_language("/tmp/foo.tsx"), Language::TypeScript);
    EXPECT_EQ(detect_language("/tmp/foo.c"), Language::C);
    // .h is C++ — matches GitHub Linguist and most C++ codebases.
    EXPECT_EQ(detect_language("/tmp/foo.h"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.cpp"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.cxx"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.cc"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.hpp"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.hh"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.hxx"), Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.rs"), Language::Rust);
    EXPECT_EQ(detect_language("/tmp/foo.java"), Language::Java);
    EXPECT_EQ(detect_language("/tmp/foo.cs"), Language::CSharp);
    EXPECT_EQ(detect_language("/tmp/foo.go"), Language::Go);
    EXPECT_EQ(detect_language("/tmp/foo.rb"), Language::Ruby);
    EXPECT_EQ(detect_language("/tmp/foo.php"), Language::Php);
    EXPECT_EQ(detect_language("/tmp/foo.phtml"), Language::Php);
    EXPECT_EQ(detect_language("/tmp/foo.sql"), Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.ddl"), Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.pks"), Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.pkb"), Language::Sql);
}

TEST(LanguageTest, DetectionIsCaseInsensitive)
{
    EXPECT_EQ(detect_language("/tmp/FOO.PY"), Language::Python);
    EXPECT_EQ(detect_language("/tmp/FOO.JaVa"), Language::Java);
    EXPECT_EQ(detect_language("/tmp/Foo.Tsx"), Language::TypeScript);
}

TEST(LanguageTest, UnknownForUnrelatedOrMissingExtensions)
{
    EXPECT_EQ(detect_language("/tmp/README"), Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/Makefile"), Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/notes.md"), Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/package.json"), Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/image.png"), Language::Unknown);
    EXPECT_EQ(detect_language(std::filesystem::path{}), Language::Unknown);
}

TEST(LanguageTest, NameMatchesEnum)
{
    EXPECT_EQ(language_name(Language::Python), "Python");
    EXPECT_EQ(language_name(Language::JavaScript), "JavaScript");
    EXPECT_EQ(language_name(Language::TypeScript), "TypeScript");
    EXPECT_EQ(language_name(Language::C), "C");
    EXPECT_EQ(language_name(Language::Cpp), "C++");
    EXPECT_EQ(language_name(Language::Rust), "Rust");
    EXPECT_EQ(language_name(Language::Java), "Java");
    EXPECT_EQ(language_name(Language::CSharp), "C#");
    EXPECT_EQ(language_name(Language::Go), "Go");
    EXPECT_EQ(language_name(Language::Ruby), "Ruby");
    EXPECT_EQ(language_name(Language::Php), "PHP");
    EXPECT_EQ(language_name(Language::Sql), "SQL");
    EXPECT_EQ(language_name(Language::Unknown), "Unknown");
}

TEST(LanguageTest, RefineKeepsCppForRealHeaders)
{
    constexpr std::string_view header = "#pragma once\n"
                                        "#include <vector>\n"
                                        "namespace foo {\n"
                                        "class Bar { public: void baz(); };\n"
                                        "}\n";
    EXPECT_EQ(refine_language(Language::Cpp, ".h", header), Language::Cpp);

    constexpr std::string_view c_header = "#ifndef FOO_H\n#define FOO_H\nint foo(void);\n#endif\n";
    EXPECT_EQ(refine_language(Language::Cpp, ".h", c_header), Language::Cpp);
}

TEST(LanguageTest, RefineReclassifiesJsAliasHeaderAsJavaScript)
{
    // Mimics the legacy "alias.h" file from the FEEDBACK-2026-05-06
    // case study: an HTML help system using .h to host JS aliases.
    constexpr std::string_view js_alias = "// Help-system JS aliases.\n"
                                          "var navAliases = {};\n"
                                          "function regAlias(name, target) {\n"
                                          "  navAliases[name] = target;\n"
                                          "}\n";
    EXPECT_EQ(refine_language(Language::Cpp, ".h", js_alias), Language::JavaScript);
}

TEST(LanguageTest, RefineLeavesUnambiguousExtensionsAlone)
{
    // Refinement only touches .h. .cpp / .js stay put no matter what.
    EXPECT_EQ(refine_language(Language::Cpp, ".cpp", "function noop() {}"), Language::Cpp);
    EXPECT_EQ(refine_language(Language::JavaScript, ".js", "#include <vector>"),
              Language::JavaScript);
}

TEST(LanguageTest, RefineKeepsCppForAmbiguousContent)
{
    // No C/C++ markers and no JS markers — keep the original guess so
    // the file is still indexed (rather than dropped to Unknown).
    constexpr std::string_view ambiguous = "// just a comment\n";
    EXPECT_EQ(refine_language(Language::Cpp, ".h", ambiguous), Language::Cpp);
}

} // namespace
