#include <string_view>

#include <gtest/gtest.h>

#include "code/complexity_analyzer.h"
#include "code/language.h"
#include "code/parser.h"
#include "code/symbol.h"

namespace {

using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::TreeSitterParser;

/// Helper: parse a one-function snippet and return the single
/// function's complexity from its Symbol record. Fails the test if
/// exactly one Function symbol isn't found.
int complexity_of_single_function(TreeSitterParser& parser, Language language,
                                  std::string_view source, std::string_view expected_name)
{
    const auto result = parser.parse_file(language, source);
    for (const Symbol& sym : result.symbols) {
        if ((sym.kind == SymbolKind::Function || sym.kind == SymbolKind::Method) &&
            sym.name == expected_name) {
            return sym.complexity;
        }
    }
    return -1;
}

TEST(ComplexityAnalyzerTest, Cpp_LinearFunction_ComplexityOne)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    constexpr std::string_view src = R"(
int add(int a, int b) {
    return a + b;
}
)";
    EXPECT_EQ(complexity_of_single_function(parser, Language::Cpp, src, "add"), 1);
}

TEST(ComplexityAnalyzerTest, Cpp_SingleIf_ComplexityTwo)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    constexpr std::string_view src = R"(
int sign(int x) {
    if (x < 0) {
        return -1;
    }
    return 1;
}
)";
    EXPECT_EQ(complexity_of_single_function(parser, Language::Cpp, src, "sign"), 2);
}

TEST(ComplexityAnalyzerTest, Cpp_NestedBranchesAndLoop)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    // 1 (base) + if + else-if (a second if_statement in tree-sitter-cpp)
    //         + for + case case case + ternary = 1 + 6-7
    constexpr std::string_view src = R"(
int classify(int x) {
    if (x == 0) {
        return 0;
    } else if (x < 0) {
        return -1;
    }
    for (int i = 0; i < x; ++i) {
        if (i % 2 == 0) {
            continue;
        }
    }
    return x > 100 ? 2 : 1;
}
)";
    const int c = complexity_of_single_function(parser, Language::Cpp, src, "classify");
    EXPECT_GT(c, 3) << "nested branches should give complexity > 3, got " << c;
}

TEST(ComplexityAnalyzerTest, Python_LinearFunction_ComplexityOne)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    constexpr std::string_view src = R"(
def greet(name):
    return "hello, " + name
)";
    EXPECT_EQ(complexity_of_single_function(parser, Language::Python, src, "greet"), 1);
}

TEST(ComplexityAnalyzerTest, Python_IfAndForLoop)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    // 1 + if + elif + for + if-inside-for = 5
    constexpr std::string_view src = R"(
def process(items):
    result = []
    if items:
        for x in items:
            if x > 0:
                result.append(x)
    elif items is None:
        return None
    return result
)";
    const int c = complexity_of_single_function(parser, Language::Python, src, "process");
    EXPECT_GE(c, 4);
}

TEST(ComplexityAnalyzerTest, NonFunctionSymbols_ComplexityZero)
{
    TreeSitterParser parser;
    parser.register_builtin_languages();
    constexpr std::string_view src = R"(
namespace demo {
class Widget {};
struct Point {};
enum Color { Red, Green, Blue };
}
)";
    const auto result = parser.parse_file(Language::Cpp, src);
    for (const Symbol& sym : result.symbols) {
        if (sym.kind != SymbolKind::Function && sym.kind != SymbolKind::Method) {
            EXPECT_EQ(sym.complexity, 0) << sym.name << " (" << static_cast<int>(sym.kind) << ")";
        }
    }
}

} // namespace
