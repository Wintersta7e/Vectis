#include "modes/code/language.h"

#include <filesystem>

#include <gtest/gtest.h>

namespace {

using vectis::modes::code::detect_language;
using vectis::modes::code::Language;
using vectis::modes::code::language_name;

TEST(LanguageTest, DetectsKnownExtensions)
{
    EXPECT_EQ(detect_language("/tmp/foo.py"),     Language::Python);
    EXPECT_EQ(detect_language("/tmp/foo.pyi"),    Language::Python);
    EXPECT_EQ(detect_language("/tmp/foo.js"),     Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.jsx"),    Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.mjs"),    Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.cjs"),    Language::JavaScript);
    EXPECT_EQ(detect_language("/tmp/foo.ts"),     Language::TypeScript);
    EXPECT_EQ(detect_language("/tmp/foo.tsx"),    Language::TypeScript);
    EXPECT_EQ(detect_language("/tmp/foo.c"),      Language::C);
    EXPECT_EQ(detect_language("/tmp/foo.h"),      Language::C);
    EXPECT_EQ(detect_language("/tmp/foo.cpp"),    Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.cxx"),    Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.cc"),     Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.hpp"),    Language::Cpp);
    EXPECT_EQ(detect_language("/tmp/foo.rs"),     Language::Rust);
    EXPECT_EQ(detect_language("/tmp/foo.java"),   Language::Java);
    EXPECT_EQ(detect_language("/tmp/foo.cs"),     Language::CSharp);
    EXPECT_EQ(detect_language("/tmp/foo.go"),     Language::Go);
    EXPECT_EQ(detect_language("/tmp/foo.rb"),     Language::Ruby);
    EXPECT_EQ(detect_language("/tmp/foo.php"),    Language::Php);
    EXPECT_EQ(detect_language("/tmp/foo.phtml"),  Language::Php);
    EXPECT_EQ(detect_language("/tmp/foo.sql"),    Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.ddl"),    Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.pks"),    Language::Sql);
    EXPECT_EQ(detect_language("/tmp/foo.pkb"),    Language::Sql);
}

TEST(LanguageTest, DetectionIsCaseInsensitive)
{
    EXPECT_EQ(detect_language("/tmp/FOO.PY"),  Language::Python);
    EXPECT_EQ(detect_language("/tmp/FOO.JaVa"),Language::Java);
    EXPECT_EQ(detect_language("/tmp/Foo.Tsx"), Language::TypeScript);
}

TEST(LanguageTest, UnknownForUnrelatedOrMissingExtensions)
{
    EXPECT_EQ(detect_language("/tmp/README"),          Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/Makefile"),        Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/notes.md"),        Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/package.json"),    Language::Unknown);
    EXPECT_EQ(detect_language("/tmp/image.png"),       Language::Unknown);
    EXPECT_EQ(detect_language(std::filesystem::path{}), Language::Unknown);
}

TEST(LanguageTest, NameMatchesEnum)
{
    EXPECT_EQ(language_name(Language::Python),     "Python");
    EXPECT_EQ(language_name(Language::JavaScript), "JavaScript");
    EXPECT_EQ(language_name(Language::TypeScript), "TypeScript");
    EXPECT_EQ(language_name(Language::C),          "C");
    EXPECT_EQ(language_name(Language::Cpp),        "C++");
    EXPECT_EQ(language_name(Language::Rust),       "Rust");
    EXPECT_EQ(language_name(Language::Java),       "Java");
    EXPECT_EQ(language_name(Language::CSharp),     "C#");
    EXPECT_EQ(language_name(Language::Go),         "Go");
    EXPECT_EQ(language_name(Language::Ruby),       "Ruby");
    EXPECT_EQ(language_name(Language::Php),        "PHP");
    EXPECT_EQ(language_name(Language::Sql),        "SQL");
    EXPECT_EQ(language_name(Language::Unknown),    "Unknown");
}

} // namespace
