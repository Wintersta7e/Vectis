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
(function_definition
  declarator: (function_declarator
    declarator: (identifier) @name)) @function
(function_definition
  declarator: (function_declarator
    declarator: (field_identifier) @name)) @method
(class_specifier      name: (type_identifier) @name) @class
(struct_specifier     name: (type_identifier) @name) @struct
(namespace_definition name: (namespace_identifier) @name) @namespace
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
        // Queries for the remaining languages are added in the next
        // commit; until then they fall through and the parser skips
        // them (register_one() treats an empty query as "unsupported").
        case Language::CSharp:     return {};
        case Language::Go:         return {};
        case Language::Ruby:       return {};
        case Language::Php:        return {};
        case Language::Sql:        return {};
        case Language::Unknown:    return {};
    }
    return {};
}

} // namespace vectis::modes::code
