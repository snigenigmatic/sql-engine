#pragma once

#include "common/value.h"
#include "lexer/token.h"
#include "parser/ast.h"
#include <functional>

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
    Value EvaluateUnaryOp(TokenType op, const Value &operand);

    // Resolves a column reference to its value in the current row
    using ColumnResolver = std::function<Value(const ColumnExpression &)>;

    // Evaluate an expression tree. AND / OR skip their right operand when
    // the left one already decides the result.
    Value EvaluateExpression(const Expression *expr, const ColumnResolver &resolve_column);

    // WHERE semantics: only TRUE qualifies; FALSE and NULL do not.
    // Throws if the value is not a boolean.
    bool IsTrue(const Value &value);

} // namespace sql
