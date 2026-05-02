#include "code/complexity_analyzer.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <tree_sitter/api.h>

#include "code/language.h"

namespace vectis::code {

namespace {

/// True if `type` is a decision-point node in the given language.
/// The list per language is hand-picked to match standard cyclomatic
/// complexity definition: if / else-if / while / for / case / &&  /
/// || / ternary.
[[nodiscard]] bool is_decision_point(std::string_view type, Language language) noexcept
{
    switch (language) {
    case Language::Cpp:
    case Language::C: {
        return type == "if_statement" || type == "while_statement" || type == "for_statement" ||
               type == "for_range_loop" || type == "do_statement" || type == "case_statement" ||
               type == "conditional_expression" || type == "&&" || type == "||";
    }
    case Language::Python: {
        return type == "if_statement" || type == "while_statement" || type == "for_statement" ||
               type == "elif_clause" || type == "conditional_expression" ||
               type == "boolean_operator" || type == "try_statement" || type == "except_clause";
    }
    case Language::JavaScript:
    case Language::TypeScript: {
        return type == "if_statement" || type == "while_statement" || type == "for_statement" ||
               type == "for_in_statement" || type == "do_statement" || type == "switch_case" ||
               type == "ternary_expression" || type == "&&" || type == "||";
    }
    case Language::Rust: {
        return type == "if_expression" || type == "while_expression" || type == "for_expression" ||
               type == "loop_expression" || type == "match_arm" || type == "&&" || type == "||";
    }
    case Language::Java:
    case Language::CSharp: {
        return type == "if_statement" || type == "while_statement" || type == "for_statement" ||
               type == "enhanced_for_statement" || type == "for_each_statement" ||
               type == "do_statement" || type == "case_statement" || type == "ternary_expression" ||
               type == "conditional_expression" || type == "catch_clause";
    }
    case Language::Go: {
        return type == "if_statement" || type == "for_statement" ||
               type == "type_switch_statement" || type == "expression_switch_statement" ||
               type == "expression_case" || type == "select_statement";
    }
    case Language::Ruby: {
        return type == "if" || type == "elsif" || type == "unless" || type == "while" ||
               type == "until" || type == "for" || type == "when" || type == "conditional" ||
               type == "rescue";
    }
    case Language::Php: {
        return type == "if_statement" || type == "while_statement" || type == "for_statement" ||
               type == "foreach_statement" || type == "do_statement" || type == "case_statement" ||
               type == "conditional_expression" || type == "match_expression";
    }
    case Language::Sql:
    case Language::Unknown:
    default:
        return false;
    }
}

/// Iterative DFS counter — tree-sitter nodes can get deep on large
/// functions, so we avoid recursion to keep the stack under control.
[[nodiscard]] int count_decision_points_iterative(TSNode root, Language language) noexcept
{
    if (ts_node_is_null(root)) {
        return 0;
    }

    int count = 0;

    // Manual stack of nodes to visit.
    constexpr std::size_t k_stack_capacity = 256;
    std::array<TSNode, k_stack_capacity> stack{};
    std::size_t top = 0;
    stack[top++] = root;

    while (top > 0) {
        const TSNode node = stack[--top];
        if (ts_node_is_null(node)) {
            continue;
        }

        const std::string_view type{ts_node_type(node)};
        if (is_decision_point(type, language)) {
            ++count;
        }

        const std::uint32_t child_count = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < child_count && top < k_stack_capacity - 1; ++i) {
            stack[top++] = ts_node_named_child(node, i);
        }
        // If we overflowed the fixed stack (very deep tree), we
        // under-count rather than crash. Acceptable for Step 4.
    }

    return count;
}

} // namespace

int compute_cyclomatic_complexity(TSNode function_node, Language language) noexcept
{
    if (ts_node_is_null(function_node)) {
        return 1;
    }
    return 1 + count_decision_points_iterative(function_node, language);
}

} // namespace vectis::code
