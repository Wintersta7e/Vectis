#include "modes/code/parser.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "modes/code/language.h"
#include "modes/code/symbol.h"

namespace {

using vectis::modes::code::Language;
using vectis::modes::code::Symbol;
using vectis::modes::code::SymbolKind;
using vectis::modes::code::TreeSitterParser;

/// Helper: does the result contain a symbol with the given name + kind?
bool has_symbol(const std::vector<Symbol>& symbols, std::string_view name, SymbolKind kind)
{
    return std::any_of(symbols.begin(), symbols.end(), [&](const Symbol& s) {
        return s.name == name && s.kind == kind;
    });
}

/// Build a parser with the full builtin set registered once per test.
std::unique_ptr<TreeSitterParser> make_parser()
{
    auto p = std::make_unique<TreeSitterParser>();
    p->register_builtin_languages();
    return p;
}

TEST(ParserTest, SupportsAllBuiltinLanguages)
{
    auto parser = make_parser();
    EXPECT_TRUE(parser->supports(Language::Python));
    EXPECT_TRUE(parser->supports(Language::JavaScript));
    EXPECT_TRUE(parser->supports(Language::TypeScript));
    EXPECT_TRUE(parser->supports(Language::C));
    EXPECT_TRUE(parser->supports(Language::Cpp));
    EXPECT_TRUE(parser->supports(Language::Rust));
    EXPECT_TRUE(parser->supports(Language::Java));
    EXPECT_FALSE(parser->supports(Language::Unknown));
}

TEST(ParserTest, ExtractsPythonFunctionsAndClasses)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
def alpha():
    return 1

def beta(x, y):
    return x + y

class Gamma:
    def method_one(self):
        return 42
)";
    const auto result = parser->parse_file(Language::Python, source);
    EXPECT_TRUE(has_symbol(result.symbols, "alpha", SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "beta",  SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "Gamma", SymbolKind::Class));
}

TEST(ParserTest, ExtractsTypeScriptFunctionsClassesAndInterfaces)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
function doThing(x: number): number {
    return x * 2;
}

export class UserService {
    findById(id: string): User | null {
        return null;
    }
}

interface User {
    id: string;
    name: string;
}

type UserId = string;
)";
    const auto result = parser->parse_file(Language::TypeScript, source);
    EXPECT_TRUE(has_symbol(result.symbols, "doThing",     SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "UserService", SymbolKind::Class));
    EXPECT_TRUE(has_symbol(result.symbols, "findById",    SymbolKind::Method));
    EXPECT_TRUE(has_symbol(result.symbols, "User",        SymbolKind::Interface));
    EXPECT_TRUE(has_symbol(result.symbols, "UserId",      SymbolKind::Type));
}

TEST(ParserTest, ExtractsCppClassesAndFunctions)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace demo {

struct Point {
    int x;
    int y;
};

class Widget {
public:
    void render();
};

int free_function(int a, int b) {
    return a + b;
}

} // namespace demo
)";
    const auto result = parser->parse_file(Language::Cpp, source);
    EXPECT_TRUE(has_symbol(result.symbols, "Point",         SymbolKind::Struct));
    EXPECT_TRUE(has_symbol(result.symbols, "Widget",        SymbolKind::Class));
    EXPECT_TRUE(has_symbol(result.symbols, "free_function", SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "demo",          SymbolKind::Namespace));
}

TEST(ParserTest, ExtractsRustFunctionsAndTraits)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
pub fn hello() -> &'static str {
    "hello"
}

pub struct Config {
    pub port: u16,
}

pub trait Greet {
    fn greet(&self) -> String;
}

pub enum Direction {
    North,
    South,
}
)";
    const auto result = parser->parse_file(Language::Rust, source);
    EXPECT_TRUE(has_symbol(result.symbols, "hello",     SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "Config",    SymbolKind::Struct));
    EXPECT_TRUE(has_symbol(result.symbols, "Greet",     SymbolKind::Interface));
    EXPECT_TRUE(has_symbol(result.symbols, "Direction", SymbolKind::Enum));
}

TEST(ParserTest, LineNumbersAre1Based)
{
    auto parser = make_parser();
    // Line 1 is blank, line 2 contains `def foo():`
    constexpr std::string_view source = "\ndef foo():\n    pass\n";
    const auto result = parser->parse_file(Language::Python, source);
    ASSERT_EQ(result.symbols.size(), 1U);
    EXPECT_EQ(result.symbols[0].name, "foo");
    EXPECT_EQ(result.symbols[0].line_start, 2);
}

TEST(ParserTest, EmptyContentReturnsNoSymbols)
{
    auto parser = make_parser();
    const auto result = parser->parse_file(Language::Python, "");
    EXPECT_TRUE(result.symbols.empty());
}

TEST(ParserTest, UnknownLanguageReturnsNoSymbols)
{
    auto parser = make_parser();
    const auto result = parser->parse_file(Language::Unknown, "def foo(): pass");
    EXPECT_TRUE(result.symbols.empty());
}

} // namespace
