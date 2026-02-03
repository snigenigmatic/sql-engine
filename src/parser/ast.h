#pragma once

#include "lexer/token.h"
#include "common/value.h"
#include <vector>
#include <string>
#include <memory>
#include <optional>

namespace sql
{

    // --- Expressions ---

    enum class ExpressionType
    {
        LITERAL,
        COLUMN_REF,
        BINARY_OP
    };

    struct Expression
    {
        virtual ~Expression() = default;
        virtual ExpressionType GetType() const = 0;
    };

    struct LiteralExpression : public Expression
    {
        Value value;

        explicit LiteralExpression(Value v) : value(std::move(v)) {}
        ExpressionType GetType() const override { return ExpressionType::LITERAL; }
    };

    struct ColumnExpression : public Expression
    {
        std::string name;

        explicit ColumnExpression(std::string n) : name(std::move(n)) {}
        ExpressionType GetType() const override { return ExpressionType::COLUMN_REF; }
    };

    struct BinaryExpression : public Expression
    {
        std::unique_ptr<Expression> left;
        std::unique_ptr<Expression> right;
        TokenType op; // PLUS, MINUS, EQ, GT, etc.

        BinaryExpression(std::unique_ptr<Expression> l, TokenType o, std::unique_ptr<Expression> r)
            : left(std::move(l)), right(std::move(r)), op(o) {}
        ExpressionType GetType() const override { return ExpressionType::BINARY_OP; }
    };

    // --- Statements ---

    enum class StatementType
    {
        SELECT,
        CREATE_TABLE,
        INSERT
    };

    struct Statement
    {
        virtual ~Statement() = default;
        virtual StatementType GetType() const = 0;
    };

    struct SelectStatement : public Statement
    {
        std::string table;
        std::vector<std::string> columns;
        bool select_star = false;
        std::unique_ptr<Expression> where;

        StatementType GetType() const override { return StatementType::SELECT; }
    };

} // namespace sql
