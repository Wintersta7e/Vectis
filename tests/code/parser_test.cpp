#include "code/parser.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;
using vectis::code::TreeSitterParser;

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

TEST(ParserTest, ExtractsEnumValuesAsMembers)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
namespace demo {
enum class Color {
    Red,
    Green,
    Blue,
};

enum Flavor {
    Sweet = 1,
    Salty = 2,
    Umami = 3,
};
}  // namespace demo
)";
    const auto result = parser->parse_file(Language::Cpp, source);

    bool saw_color = false;
    bool saw_flavor = false;
    for (const auto& sym : result.symbols) {
        if (sym.kind != SymbolKind::Enum) {
            continue;
        }
        if (sym.name == "Color") {
            saw_color = true;
            ASSERT_EQ(sym.members.size(), 3U);
            EXPECT_EQ(sym.members[0], "Red");
            EXPECT_EQ(sym.members[1], "Green");
            EXPECT_EQ(sym.members[2], "Blue");
        }
        if (sym.name == "Flavor") {
            saw_flavor = true;
            ASSERT_EQ(sym.members.size(), 3U);
            EXPECT_EQ(sym.members[0], "Sweet");
            EXPECT_EQ(sym.members[1], "Salty");
            EXPECT_EQ(sym.members[2], "Umami");
        }
    }
    EXPECT_TRUE(saw_color);
    EXPECT_TRUE(saw_flavor);
}

TEST(ParserTest, ExtractsStructFieldsAsMembers)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
struct Point {
    int x;
    int y;
};

struct User {
    std::string id;
    std::string name;
    int         age;

    // Methods on a struct should NOT appear as members.
    std::string greeting() const;
};
)";
    const auto result = parser->parse_file(Language::Cpp, source);

    bool saw_point = false;
    bool saw_user  = false;
    for (const auto& sym : result.symbols) {
        if (sym.kind != SymbolKind::Struct) {
            continue;
        }
        if (sym.name == "Point") {
            saw_point = true;
            ASSERT_EQ(sym.members.size(), 2U);
            EXPECT_EQ(sym.members[0], "x");
            EXPECT_EQ(sym.members[1], "y");
        }
        if (sym.name == "User") {
            saw_user = true;
            // Three data members; `greeting()` is filtered out because
            // it's a function_declarator inside the field_declaration.
            ASSERT_EQ(sym.members.size(), 3U);
            EXPECT_EQ(sym.members[0], "id");
            EXPECT_EQ(sym.members[1], "name");
            EXPECT_EQ(sym.members[2], "age");
        }
    }
    EXPECT_TRUE(saw_point);
    EXPECT_TRUE(saw_user);
}

TEST(ParserTest, ClassesDoNotReportMembers)
{
    // Classes have private state by default; exposing it would leak
    // implementation details. The public surface is already captured
    // via method symbols, so class `members` should remain empty.
    auto parser = make_parser();
    constexpr std::string_view source = R"(
class Widget {
public:
    Widget();
    void render();
private:
    int m_width;
    int m_height;
};
)";
    const auto result = parser->parse_file(Language::Cpp, source);
    for (const auto& sym : result.symbols) {
        if (sym.kind == SymbolKind::Class && sym.name == "Widget") {
            EXPECT_TRUE(sym.members.empty());
        }
    }
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

TEST(ParserTest, ExtractsCppIncludes)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
#include <vector>
#include <string>
#include "core/log.h"
#include "platform/file_io.h"

namespace demo {
int foo() { return 0; }
}
)";
    const auto imports = parser->extract_imports(Language::Cpp, source);

    // Angle-bracket includes (<vector>, <string>) are intentionally
    // skipped; only the two quoted includes should show up.
    ASSERT_EQ(imports.size(), 2U);
    EXPECT_EQ(imports[0].import_string, "core/log.h");
    EXPECT_EQ(imports[0].kind,          "include");
    EXPECT_EQ(imports[1].import_string, "platform/file_io.h");
}

TEST(ParserTest, ExtractsPythonImports)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
import os
import sys
from typing import Optional
from models.user import User
from .helpers import greet
)";
    const auto imports = parser->extract_imports(Language::Python, source);

    // All five imports should be captured — names are dotted_name
    // or relative_import; relative imports show up as "." prefix.
    ASSERT_GE(imports.size(), 3U);

    bool saw_os = false;
    bool saw_typing = false;
    bool saw_models_user = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "os") { saw_os = true; }
        if (imp.import_string == "typing") { saw_typing = true; }
        if (imp.import_string == "models.user") { saw_models_user = true; }
    }
    EXPECT_TRUE(saw_os);
    EXPECT_TRUE(saw_typing);
    EXPECT_TRUE(saw_models_user);
}

TEST(ParserTest, ExtractsTypeScriptImports)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
import { User } from "./types";
import { UserService } from "./services/user-service";
import * as path from "path";
)";
    const auto imports = parser->extract_imports(Language::TypeScript, source);

    ASSERT_EQ(imports.size(), 3U);
    EXPECT_EQ(imports[0].import_string, "./types");
    EXPECT_EQ(imports[1].import_string, "./services/user-service");
    EXPECT_EQ(imports[2].import_string, "path");
    for (const auto& imp : imports) {
        EXPECT_EQ(imp.kind, "import");
    }
}

TEST(ParserTest, ExtractsRustUseDeclarations)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
use std::collections::HashMap;
use crate::foo::bar;
mod helpers;
)";
    const auto imports = parser->extract_imports(Language::Rust, source);
    ASSERT_GE(imports.size(), 2U);
    // The Rust query should pick up HashMap and bar from the
    // scoped_identifier captures, plus helpers from the mod_item.
    bool saw_helpers = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "helpers") { saw_helpers = true; }
    }
    EXPECT_TRUE(saw_helpers);
}

TEST(ParserTest, ExtractsJavaImports)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
import java.util.List;
import java.util.Map;
import com.example.foo.Bar;
class Main {}
)";
    const auto imports = parser->extract_imports(Language::Java, source);
    ASSERT_EQ(imports.size(), 3U);
    EXPECT_EQ(imports[0].import_string, "java.util.List");
    EXPECT_EQ(imports[2].import_string, "com.example.foo.Bar");
    for (const auto& imp : imports) {
        EXPECT_EQ(imp.kind, "import");
    }
}

TEST(ParserTest, ExtractsCSharpUsings)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
using System;
using System.Collections.Generic;
using SampleApp.Models;

namespace SampleApp {}
)";
    const auto imports = parser->extract_imports(Language::CSharp, source);
    ASSERT_GE(imports.size(), 3U);
    bool saw_system = false;
    bool saw_collections = false;
    bool saw_models = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "System")                    { saw_system = true; }
        if (imp.import_string == "System.Collections.Generic"){ saw_collections = true; }
        if (imp.import_string == "SampleApp.Models")          { saw_models = true; }
        EXPECT_EQ(imp.kind, "use");
    }
    EXPECT_TRUE(saw_system);
    EXPECT_TRUE(saw_collections);
    EXPECT_TRUE(saw_models);
}

TEST(ParserTest, ExtractsGoImports)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
package main

import (
    "fmt"
    "example.com/sample/user"
)

import "os"
)";
    const auto imports = parser->extract_imports(Language::Go, source);
    ASSERT_GE(imports.size(), 3U);
    bool saw_fmt = false;
    bool saw_user = false;
    bool saw_os = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "fmt")                      { saw_fmt = true; }
        if (imp.import_string == "example.com/sample/user") { saw_user = true; }
        if (imp.import_string == "os")                       { saw_os = true; }
    }
    EXPECT_TRUE(saw_fmt);
    EXPECT_TRUE(saw_user);
    EXPECT_TRUE(saw_os);
}

TEST(ParserTest, ExtractsRubyRequires)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(
require 'json'
require_relative 'lib/greeter'
)";
    const auto imports = parser->extract_imports(Language::Ruby, source);
    ASSERT_EQ(imports.size(), 2U);
    bool saw_json = false;
    bool saw_lib = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "json")         { saw_json = true; }
        if (imp.import_string == "lib/greeter")  { saw_lib = true; }
        EXPECT_EQ(imp.kind, "require");
    }
    EXPECT_TRUE(saw_json);
    EXPECT_TRUE(saw_lib);
}

TEST(ParserTest, ExtractsPhpRequiresAndUses)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(<?php
require_once 'src/UserService.php';
include 'config.php';
use App\Services\UserService;
)";
    const auto imports = parser->extract_imports(Language::Php, source);
    ASSERT_GE(imports.size(), 3U);
    bool saw_require = false;
    bool saw_include = false;
    bool saw_use     = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "src/UserService.php") {
            saw_require = true;
            EXPECT_EQ(imp.kind, "require");
        }
        if (imp.import_string == "config.php") {
            saw_include = true;
            EXPECT_EQ(imp.kind, "include");
        }
        if (imp.import_string == "App\\Services\\UserService") {
            saw_use = true;
            EXPECT_EQ(imp.kind, "use");
        }
    }
    EXPECT_TRUE(saw_require);
    EXPECT_TRUE(saw_include);
    EXPECT_TRUE(saw_use);
}

TEST(ParserTest, ExtractImports_EmptyForSql)
{
    auto parser = make_parser();
    // SQL import semantics (FK graph / sqlplus @includes) not yet spec'd —
    // the query stays empty and extraction yields no results.
    const auto imports = parser->extract_imports(
        Language::Sql, "CREATE TABLE users (id INT);");
    EXPECT_TRUE(imports.empty());
}

TEST(ParserTest, ExtractsJavaPackageDeclarations)
{
    auto parser = make_parser();
    constexpr std::string_view dotted = R"(
package com.example.foo;

public class Bar {}
)";
    const auto ns1 = parser->extract_namespaces(Language::Java, dotted);
    ASSERT_EQ(ns1.size(), 1U);
    EXPECT_EQ(ns1[0], "com.example.foo");

    constexpr std::string_view single = R"(
package flat;
public class Root {}
)";
    const auto ns2 = parser->extract_namespaces(Language::Java, single);
    ASSERT_EQ(ns2.size(), 1U);
    EXPECT_EQ(ns2[0], "flat");
}

TEST(ParserTest, ExtractsCSharpNamespaces_BothBlockAndFileScoped)
{
    auto parser = make_parser();
    constexpr std::string_view block_form = R"(
namespace SampleApp.Models
{
    public class User {}
}
)";
    const auto ns1 = parser->extract_namespaces(Language::CSharp, block_form);
    ASSERT_EQ(ns1.size(), 1U);
    EXPECT_EQ(ns1[0], "SampleApp.Models");

    constexpr std::string_view file_scoped = R"(
namespace SampleApp.Services;

public class UserService {}
)";
    const auto ns2 = parser->extract_namespaces(Language::CSharp, file_scoped);
    ASSERT_EQ(ns2.size(), 1U);
    EXPECT_EQ(ns2[0], "SampleApp.Services");
}

TEST(ParserTest, ExtractsPhpNamespaces)
{
    auto parser = make_parser();
    constexpr std::string_view source = R"(<?php
namespace App\Services;
class UserService {}
)";
    const auto ns = parser->extract_namespaces(Language::Php, source);
    ASSERT_EQ(ns.size(), 1U);
    EXPECT_EQ(ns[0], "App\\Services");
}

TEST(ParserTest, ExtractsCSharpUsings_HandlesStaticAndAliased)
{
    auto parser = make_parser();
    // `using static` and `using Alias = ...;` both still expose the
    // target namespace as a child of `using_directive` — our existing
    // import query captures both without any alias/static-specific
    // pattern. This test exists as a regression guard.
    constexpr std::string_view source = R"(
using System;
using static System.Math;
using Models = SampleApp.Models;
)";
    const auto imports = parser->extract_imports(Language::CSharp, source);
    ASSERT_GE(imports.size(), 3U);
    bool saw_system = false;
    bool saw_math   = false;
    bool saw_models = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "System")             saw_system = true;
        if (imp.import_string == "System.Math")        saw_math   = true;
        if (imp.import_string == "SampleApp.Models")   saw_models = true;
    }
    EXPECT_TRUE(saw_system);
    EXPECT_TRUE(saw_math);
    EXPECT_TRUE(saw_models);
}

TEST(ParserTest, ExtractsPhpRequireConcat)
{
    auto parser = make_parser();
    // `require __DIR__ . '/foo.php';` — the string literal lives on
    // the RHS of a binary_expression. Our nested query pulls it out.
    constexpr std::string_view source = R"(<?php
require_once __DIR__ . '/src/UserService.php';
)";
    const auto imports = parser->extract_imports(Language::Php, source);
    ASSERT_GE(imports.size(), 1U);
    bool saw = false;
    for (const auto& imp : imports) {
        if (imp.import_string == "/src/UserService.php") {
            saw = true;
            EXPECT_EQ(imp.kind, "require");
        }
    }
    EXPECT_TRUE(saw);
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
