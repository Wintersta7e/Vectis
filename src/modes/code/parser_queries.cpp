#include "modes/code/parser_queries.h"

#include <string_view>

#include "modes/code/language.h"

namespace vectis::modes::code {

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

} // namespace vectis::modes::code
