#include "code/parser_queries.h"

#include <string_view>

#include "code/language.h"

namespace vectis::code {

namespace {

constexpr std::string_view k_query_python = R"(
(function_definition name: (identifier) @name) @function
(class_definition    name: (identifier) @name) @class
)";

constexpr std::string_view k_query_javascript = R"(
(function_declaration name: (identifier) @name) @function
(class_declaration    name: (identifier) @name) @class
(method_definition    name: (property_identifier) @name) @method
)";

constexpr std::string_view k_query_typescript = R"(
(function_declaration   name: (identifier) @name)        @function
(class_declaration      name: (type_identifier) @name)   @class
(method_definition      name: (property_identifier) @name) @method
(interface_declaration  name: (type_identifier) @name)   @interface
(type_alias_declaration name: (type_identifier) @name)   @type
)";

constexpr std::string_view k_query_c = R"(
(function_definition
  declarator: (function_declarator
    declarator: (identifier) @name)) @function
(struct_specifier name: (type_identifier) @name) @struct
)";

constexpr std::string_view k_query_cpp = R"(
; Free function: `void foo() { ... }`
(function_definition
  declarator: (function_declarator
    declarator: (identifier) @name)) @function

; In-class inline method: `void foo() { ... }` inside a class body
(function_definition
  declarator: (function_declarator
    declarator: (field_identifier) @name)) @method

; Out-of-line method definition: `void Class::foo() { ... }`
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (identifier) @name))) @method

; Out-of-line method with nested qualifiers: `void ns::Class::foo()`
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (qualified_identifier
        name: (identifier) @name)))) @method

; Pointer-return method: `int* Class::foo()`
(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator
      declarator: (qualified_identifier
        name: (identifier) @name)))) @method

; Reference-return method: `int& Class::foo()`
(function_definition
  declarator: (reference_declarator
    (function_declarator
      declarator: (qualified_identifier
        name: (identifier) @name)))) @method

; In-class method DECLARATION (no body): `void foo();`
(field_declaration
  declarator: (function_declarator
    declarator: (field_identifier) @name)) @method

; Free function DECLARATION at namespace scope (no body):
; `Result<void> init(...);` inside `namespace foo { ... }`. Without
; these, headers that only declare free functions report "no symbols
; extracted" (log.h, parser_queries.h, file_io.h, process.h, widgets.h,
; file_dialog.h). Note: tree-sitter-cpp exposes function_declarator as
; a positional child of declaration, NOT via a `declarator:` field —
; so we match without field paths.
(declaration
  (function_declarator
    declarator: (identifier) @name)) @function

; Pointer-return free function declaration: `int* foo();`
(declaration
  (pointer_declarator
    (function_declarator
      declarator: (identifier) @name))) @function

; Reference-return free function declaration: `int& foo();`
(declaration
  (reference_declarator
    (function_declarator
      declarator: (identifier) @name))) @function

; Class / struct / namespace declarations
(class_specifier      name: (type_identifier) @name) @class
(struct_specifier     name: (type_identifier) @name) @struct
(namespace_definition name: (namespace_identifier) @name) @namespace

; Type aliases and enums
(alias_declaration    name: (type_identifier) @name) @type
(enum_specifier       name: (type_identifier) @name) @enum
)";

constexpr std::string_view k_query_rust = R"(
(function_item name: (identifier) @name) @function
(struct_item   name: (type_identifier) @name) @struct
(enum_item     name: (type_identifier) @name) @enum
(trait_item    name: (type_identifier) @name) @interface
)";

constexpr std::string_view k_query_java = R"(
(method_declaration    name: (identifier) @name) @method
(class_declaration     name: (identifier) @name) @class
(interface_declaration name: (identifier) @name) @interface
(enum_declaration      name: (identifier) @name) @enum
)";

constexpr std::string_view k_query_csharp = R"(
(class_declaration     name: (identifier) @name) @class
(interface_declaration name: (identifier) @name) @interface
(struct_declaration    name: (identifier) @name) @struct
(enum_declaration      name: (identifier) @name) @enum
(method_declaration    name: (identifier) @name) @method
(namespace_declaration name: (identifier) @name) @namespace
)";

constexpr std::string_view k_query_go = R"(
(function_declaration name: (identifier) @name) @function
(method_declaration   name: (field_identifier) @name) @method
(type_declaration
  (type_spec name: (type_identifier) @name)) @type
)";

constexpr std::string_view k_query_ruby = R"(
(method           name: (identifier) @name) @method
(singleton_method name: (identifier) @name) @method
(class            name: (constant) @name) @class
(module           name: (constant) @name) @namespace
)";

constexpr std::string_view k_query_php = R"(
(function_definition   name: (name) @name) @function
(method_declaration    name: (name) @name) @method
(class_declaration     name: (name) @name) @class
(interface_declaration name: (name) @name) @interface
(trait_declaration     name: (name) @name) @interface
(enum_declaration      name: (name) @name) @enum
)";

// SQL — m-novikov's PostgreSQL-flavoured grammar exposes unnamed
// `_identifier` children under create_* statements. The underscore rule
// is hidden, so the tree actually holds either `identifier` or
// `dotted_name` as the child — we accept both via a capture alternation.
constexpr std::string_view k_query_sql = R"(
(create_table_statement [(identifier) (dotted_name)] @name) @struct
(create_view_statement  [(identifier) (dotted_name)] @name) @type
(create_function_statement [(identifier) (dotted_name)] @name) @function
)";

// -----------------------------------------------------------------------------
// Import queries — one per supported language. Captures `@path` on the
// identifier / string holding the imported module name, and a kind
// capture (`@include`, `@import`, `@use`, `@require`, `@mod`) on the
// enclosing node so the parser can tag each raw import.
//
// Languages not yet wired (Go, Java, C#, Ruby, PHP, SQL) return an
// empty view — the parser treats empty as "no imports for this
// language" and gracefully skips extraction.
// -----------------------------------------------------------------------------

// C / C++ — quoted #include "..." only. Angle-bracket includes
// (#include <vector>) are intentionally skipped: they're system /
// library headers we won't resolve to files inside the project.
constexpr std::string_view k_imports_c_cpp = R"(
(preproc_include
  path: (string_literal) @path) @include
)";

// Python — three statement forms:
//   import foo
//   import foo.bar
//   from foo import bar
//   from .relative import bar
constexpr std::string_view k_imports_python = R"(
(import_statement (dotted_name) @path) @import
(import_from_statement module_name: (dotted_name)    @path) @import
(import_from_statement module_name: (relative_import) @path) @import
)";

// TypeScript / JavaScript — both ESM `import x from '...'` and CJS
// `require('...')`. The source path sits inside a `string >
// string_fragment` pair in either case.
constexpr std::string_view k_imports_ts_js = R"(
(import_statement
  source: (string (string_fragment) @path)) @import

; CommonJS: `require('./foo')` / `require('lodash')`. The resolver's
; relative-path branch turns `./foo` into a file lookup; bare module
; names fall through and are recorded as external. Tagging as @require
; lets the resolver distinguish CJS from ESM if it ever needs to.
(call_expression
  function: (identifier) @_func
  arguments: (arguments (string (string_fragment) @path))
  (#eq? @_func "require")) @require
)";

// Rust — `use foo::bar;` and `mod foo;`.
constexpr std::string_view k_imports_rust = R"(
(use_declaration argument: (scoped_identifier) @path) @use
(use_declaration argument: (identifier) @path) @use
(mod_item name: (identifier) @path) @mod
)";

// Java — `import foo.bar.Baz;` / `import foo.Baz;`. Static / wildcard
// forms use a different tree shape and are skipped for now.
constexpr std::string_view k_imports_java = R"(
(import_declaration (scoped_identifier) @path) @import
(import_declaration (identifier)        @path) @import
)";

// C# — `using System;` / `using System.Text;`. Aliased (`using Foo = ...`)
// and static (`using static Foo.Bar`) usings have distinct node shapes we
// don't cover in this starter.
constexpr std::string_view k_imports_csharp = R"(
(using_directive (qualified_name) @path) @use
(using_directive (identifier)     @path) @use
)";

// Go — both the single-line `import "x"` and the grouped `import ( ... )`
// forms produce `import_spec` nodes with a `path:` field.
constexpr std::string_view k_imports_go = R"(
(import_spec path: (interpreted_string_literal) @path) @import
)";

// Ruby — only `require` / `require_relative` string literals. Interpolated
// / symbol / dynamic arguments are intentionally skipped. The method name
// capture is tagged `@_m` so it's visible to the `#match?` predicate; the
// outer `@require` capture fires after it and overrides the kind.
constexpr std::string_view k_imports_ruby = R"(
(call
  method: (identifier) @_m
  arguments: (argument_list (string (string_content) @path))
  (#match? @_m "^require(_relative)?$")) @require
)";

// PHP — `require` / `require_once` / `include` / `include_once` with a
// direct string-literal argument OR a `__DIR__ . '…'` concatenation
// (the right-hand string is captured). Plus `use Namespace\Sub;` clauses.
constexpr std::string_view k_imports_php = R"(
(require_once_expression (string (string_content) @path)) @require
(require_expression      (string (string_content) @path)) @require
(include_once_expression (string (string_content) @path)) @include
(include_expression      (string (string_content) @path)) @include

; `require __DIR__ . '/foo.php';` — the string literal sits on the RHS
; of a binary_expression. Match nested so the outer kind tag fires.
(require_once_expression
  (binary_expression right: (string (string_content) @path))) @require
(require_expression
  (binary_expression right: (string (string_content) @path))) @require
(include_once_expression
  (binary_expression right: (string (string_content) @path))) @include
(include_expression
  (binary_expression right: (string (string_content) @path))) @include

(namespace_use_clause    (qualified_name)         @path)  @use
(namespace_use_clause    (name)                   @path)  @use
)";

// -----------------------------------------------------------------------------
// Namespace-declaration queries — per-language. Capture a single `@name`
// on the node that holds the dotted namespace path. The scanner records
// these for each file and the resolver builds a namespace → files map so
// C# `using X.Y;` / PHP `use X\Y;` statements create internal edges to
// every file that declares the named namespace.
// -----------------------------------------------------------------------------

constexpr std::string_view k_namespaces_csharp = R"(
(namespace_declaration            (qualified_name) @name)
(namespace_declaration            (identifier)     @name)
(file_scoped_namespace_declaration (qualified_name) @name)
(file_scoped_namespace_declaration (identifier)     @name)
)";

constexpr std::string_view k_namespaces_php = R"(
(namespace_definition (namespace_name) @name)
)";

// Java treats `package com.example;` as the enclosing namespace for the
// compilation unit. Specific imports already resolve via the dotted
// path-to-file map (`match_java_dotted`), but `import com.example.*;`
// drops the asterisk at capture time and needs a fallback that maps
// the bare namespace back to every file declaring it — which is what
// the namespace index gives us.
constexpr std::string_view k_namespaces_java = R"(
(package_declaration (scoped_identifier) @name)
(package_declaration (identifier)        @name)
)";

} // namespace

std::string_view query_for(Language language) noexcept
{
    switch (language) {
        case Language::Python:     return k_query_python;
        case Language::JavaScript: return k_query_javascript;
        case Language::TypeScript: return k_query_typescript;
        case Language::C:          return k_query_c;
        case Language::Cpp:        return k_query_cpp;
        case Language::Rust:       return k_query_rust;
        case Language::Java:       return k_query_java;
        case Language::CSharp:     return k_query_csharp;
        case Language::Go:         return k_query_go;
        case Language::Ruby:       return k_query_ruby;
        case Language::Php:        return k_query_php;
        case Language::Sql:        return k_query_sql;
        case Language::Unknown:    return {};
    }
    return {};
}

std::string_view import_query_for(Language language) noexcept
{
    switch (language) {
        case Language::Python:     return k_imports_python;
        case Language::JavaScript:
        case Language::TypeScript: return k_imports_ts_js;
        case Language::C:
        case Language::Cpp:        return k_imports_c_cpp;
        case Language::Rust:       return k_imports_rust;
        case Language::Java:       return k_imports_java;
        case Language::CSharp:     return k_imports_csharp;
        case Language::Go:         return k_imports_go;
        case Language::Ruby:       return k_imports_ruby;
        case Language::Php:        return k_imports_php;
        // SQL import semantics (FK / table references / sqlplus @includes)
        // haven't been spec'd yet; keep empty until we settle the design.
        case Language::Sql:
        case Language::Unknown:    return {};
    }
    return {};
}

std::string_view namespace_query_for(Language language) noexcept
{
    switch (language) {
        case Language::CSharp: return k_namespaces_csharp;
        case Language::Php:    return k_namespaces_php;
        // Java uses the index only as a wildcard-import fallback
        // (`import com.foo.*;`); specific imports still resolve through
        // the path-based `match_java_dotted`.
        case Language::Java:   return k_namespaces_java;
        default:               return {};
    }
}

} // namespace vectis::code
