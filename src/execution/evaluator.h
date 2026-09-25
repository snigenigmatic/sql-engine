#pragma once

#include "common/value.h"
#include "lexer/token.h"
#include "parser/ast.h"
#include <functional>
#include <optional>

namespace sql
{

    // SQL expression semantics, shared by every operator that evaluates
    // expressions (Filter, and the WHERE / SET clauses of DML).
    //
    // NULL means "unknown": comparisons and arithmetic with a NULL operand
    // yield NULL, and AND / OR / NOT use three-valued logic
    // (FALSE AND NULL = FALSE, TRUE OR NULL = TRUE, NOT NULL = NULL).
    // INTEGER and FLOAT operands are compared and combined numerically.

    Value EvaluateBinaryOp(TokenType op, const Value &left, const Value &right);
    Value EvaluateUnaryOp(TokenType op, const Value &operand); // NOT, unary minus

    // value LIKE pattern: % matches any run of characters, _ exactly one;
    // case-sensitive; NULL if either side is NULL
    Value EvaluateLike(const Value &value, const Value &pattern);

    // Resolves a column reference to its value in the current row
    using ColumnResolver = std::function<Value(const ColumnExpression &)>;

    // Evaluate an expression tree. AND / OR skip their right operand when
    // the left one already decides the result.
    Value EvaluateExpression(const Expression *expr, const ColumnResolver &resolve_column);

    // Convert a number to the other numeric type without changing its value.
    // Returns nullopt when no value of the target type equals it (2.5 as an
    // INTEGER). NULLs, non-numeric values and values already of the target
    // type are returned unchanged.
    std::optional<Value> ConvertNumber(const Value &value, DataType target);

    // Total order used by ORDER BY: NULL first, numbers by value (INTEGER
    // and FLOAT together), other values by value within a type, and values
    // of different types by type. Never throws.
    int CompareForSort(const Value &a, const Value &b);

    // Appends a byte encoding of the value to *key such that two values get
    // the same encoding exactly when GROUP BY / DISTINCT treat them as the
    // same: every NULL is alike, numbers compare by value (1 = 1.0,
    // 0.0 = -0.0), and other values by type and value.
    void AppendGroupKey(const Value &value, std::string *key);

    // WHERE semantics: only TRUE qualifies; FALSE and NULL do not.
    // Throws if the value is not a boolean.
    bool IsTrue(const Value &value);

} // namespace sql
