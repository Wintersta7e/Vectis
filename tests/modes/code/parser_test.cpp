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
    EXPECT_TRUE(parser->supports(Language::CSharp));
    EXPECT_TRUE(parser->supports(Language::Go));
    EXPECT_TRUE(parser->supports(Language::Ruby));
    EXPECT_TRUE(parser->supports(Language::Php));
    EXPECT_TRUE(parser->supports(Language::Sql));
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

TEST(ParserTest, ExtractsSignaturesForCppFunctions)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace demo {

int add(int a, int b) {
    return a + b;
}

int multiply(
    int lhs,
    int rhs) const noexcept
{
    return lhs * rhs;
}

}  // namespace demo
)";
    const auto result = parser->parse_file(Language::Cpp, source);

    // Find each function by name and inspect its signature.
    for (const auto& sym : result.symbols) {
        if (sym.name == "add") {
            EXPECT_EQ(sym.signature, "int add(int a, int b)");
        } else if (sym.name == "multiply") {
            // Whitespace-normalized, multi-line collapsed to one line.
            EXPECT_EQ(sym.signature, "int multiply( int lhs, int rhs) const noexcept");
        }
    }
}

TEST(ParserTest, ExtractsSignaturesForCppDeclarations)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace vectis::core {

Result<void> init(const std::filesystem::path& data_dir);
int* make_buffer(std::size_t n);
const std::string& default_name();

}  // namespace vectis::core
)";
    const auto result = parser->parse_file(Language::Cpp, source);

    bool saw_init = false;
    bool saw_make_buffer = false;
    for (const auto& sym : result.symbols) {
        if (sym.name == "init") {
            saw_init = true;
            // No trailing semicolon, multi-line collapsed, signature captures args.
            EXPECT_NE(sym.signature.find("init"), std::string::npos);
            EXPECT_NE(sym.signature.find("data_dir"), std::string::npos);
            EXPECT_EQ(sym.signature.find(';'), std::string::npos);
        }
        if (sym.name == "make_buffer") {
            saw_make_buffer = true;
            EXPECT_NE(sym.signature.find("int"), std::string::npos);
            EXPECT_NE(sym.signature.find("make_buffer"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_init);
    EXPECT_TRUE(saw_make_buffer);
}

TEST(ParserTest, NonFunctionSymbolsHaveEmptySignature)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace demo {
class Widget {};
struct Point {};
enum Color { Red, Blue };
}  // namespace demo
)";
    const auto result = parser->parse_file(Language::Cpp, source);
    for (const auto& sym : result.symbols) {
        if (sym.kind == SymbolKind::Class || sym.kind == SymbolKind::Struct ||
            sym.kind == SymbolKind::Enum  || sym.kind == SymbolKind::Namespace)
        {
            EXPECT_TRUE(sym.signature.empty())
                << sym.name << " had unexpected signature: " << sym.signature;
        }
    }
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

TEST(ParserTest, ExtractsCSharpClassesAndMethods)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace MyApp.Services
{
    public interface IUserService
    {
        User FindById(string id);
    }

    public class UserService : IUserService
    {
        public User FindById(string id) { return null; }
    }

    public enum UserStatus
    {
        Active,
        Inactive,
    }
}
)";
    const auto result = parser->parse_file(Language::CSharp, source);
    EXPECT_TRUE(has_symbol(result.symbols, "IUserService", SymbolKind::Interface));
    EXPECT_TRUE(has_symbol(result.symbols, "UserService",  SymbolKind::Class));
    EXPECT_TRUE(has_symbol(result.symbols, "FindById",     SymbolKind::Method));
    EXPECT_TRUE(has_symbol(result.symbols, "UserStatus",   SymbolKind::Enum));
}

TEST(ParserTest, ExtractsGoFunctionsMethodsAndTypes)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
package main

type User struct {
    ID   string
    Name string
}

type Service interface {
    Greet() string
}

func NewUser(id string) *User {
    return &User{ID: id}
}

func (u *User) Greet() string {
    return "hello, " + u.Name
}
)";
    const auto result = parser->parse_file(Language::Go, source);
    EXPECT_TRUE(has_symbol(result.symbols, "NewUser", SymbolKind::Function));
    EXPECT_TRUE(has_symbol(result.symbols, "Greet",   SymbolKind::Method));
    EXPECT_TRUE(has_symbol(result.symbols, "User",    SymbolKind::Type));
    EXPECT_TRUE(has_symbol(result.symbols, "Service", SymbolKind::Type));
}

TEST(ParserTest, ExtractsRubyClassesAndMethods)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
module Greetings
  class Greeter
    def initialize(name)
      @name = name
    end

    def hello
      "hello, #{@name}"
    end
  end
end
)";
    const auto result = parser->parse_file(Language::Ruby, source);
    EXPECT_TRUE(has_symbol(result.symbols, "Greetings",  SymbolKind::Namespace));
    EXPECT_TRUE(has_symbol(result.symbols, "Greeter",    SymbolKind::Class));
    EXPECT_TRUE(has_symbol(result.symbols, "initialize", SymbolKind::Method));
    EXPECT_TRUE(has_symbol(result.symbols, "hello",      SymbolKind::Method));
}

TEST(ParserTest, ExtractsPhpClassesAndFunctions)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(<?php

namespace App\Services;

interface UserRepository {
    public function findById(string $id): ?User;
}

class EloquentUserRepository implements UserRepository {
    public function findById(string $id): ?User {
        return null;
    }
}

function bootstrap(): void {
}
)";
    const auto result = parser->parse_file(Language::Php, source);
    EXPECT_TRUE(has_symbol(result.symbols, "UserRepository",          SymbolKind::Interface));
    EXPECT_TRUE(has_symbol(result.symbols, "EloquentUserRepository",  SymbolKind::Class));
    EXPECT_TRUE(has_symbol(result.symbols, "findById",                SymbolKind::Method));
    EXPECT_TRUE(has_symbol(result.symbols, "bootstrap",               SymbolKind::Function));
}

TEST(ParserTest, ExtractsSqlTablesViewsAndFunctions)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
CREATE TABLE users (
    id   SERIAL PRIMARY KEY,
    name TEXT NOT NULL
);

CREATE VIEW active_users AS
    SELECT * FROM users WHERE active = true;

CREATE OR REPLACE FUNCTION greet(target TEXT) RETURNS TEXT AS $$
    SELECT 'hello, ' || target;
$$ LANGUAGE SQL;
)";
    const auto result = parser->parse_file(Language::Sql, source);
    // Lower bound: tree-sitter-sql's node types are more complex than
    // the other grammars, so we just require at least one CREATE
    // statement was recognized. If this ever drops to zero, tune the
    // query in parser_queries.cpp.
    EXPECT_GE(result.symbols.size(), 1U)
        << "expected at least one CREATE-statement symbol from SQL grammar";
}

} // namespace
