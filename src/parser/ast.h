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
        TokenType op;
        BinaryExpression(std::unique_ptr<Expression> l, TokenType o, std::unique_ptr<Expression> r)
            : left(std::move(l)), right(std::move(r)), op(o) {}
        ExpressionType GetType() const override { return ExpressionType::BINARY_OP; }
    };

    // --- Statements ---

    enum class StatementType
    {
        SELECT,
        CREATE_TABLE,
        DROP_TABLE,
        INSERT,
        CREATE_INDEX,
        DELETE_STMT,
        UPDATE_STMT
    };

    struct Statement
    {
        virtual ~Statement() = default;
        virtual StatementType GetType() const = 0;
    };

    struct SelectStatement : public Statement
    {
        std::string table;
        std::optional<std::string> join_table;
        std::optional<std::string> join_left_column;
        std::optional<std::string> join_right_column;
        std::vector<std::string> columns;
        bool select_star = false;
        std::unique_ptr<Expression> where;
        StatementType GetType() const override { return StatementType::SELECT; }
    };

    struct ColumnDef
    {
        std::string name;
        TokenType type_token; // INTEGER, VARCHAR, FLOAT, BOOLEAN
        int length = 0;       // For VARCHAR(n)
    };

    struct CreateTableStatement : public Statement
    {
        std::string table;
        std::vector<ColumnDef> columns;
        StatementType GetType() const override { return StatementType::CREATE_TABLE; }
    };

    struct DropTableStatement : public Statement
    {
        std::string table;
        StatementType GetType() const override { return StatementType::DROP_TABLE; }
    };

    struct InsertStatement : public Statement
    {
        std::string table;
        std::vector<std::vector<std::unique_ptr<Expression>>> rows; // VALUES (...), (...)
        StatementType GetType() const override { return StatementType::INSERT; }
    };

    struct CreateIndexStatement : public Statement
    {
        std::string index_name;
        std::string table;
        std::string column;
        StatementType GetType() const override { return StatementType::CREATE_INDEX; }
    };

    struct DeleteStatement : public Statement
    {
        std::string table;
        std::unique_ptr<Expression> where;
        StatementType GetType() const override { return StatementType::DELETE_STMT; }
    };

    struct UpdateStatement : public Statement
    {
        std::string table;
        std::vector<std::pair<std::string, std::unique_ptr<Expression>>> assignments; // SET col = expr
        std::unique_ptr<Expression> where;
        StatementType GetType() const override { return StatementType::UPDATE_STMT; }
    };

    std::string ExpressionTypeToString(ExpressionType type);
    std::string StatementTypeToString(StatementType type);
    std::string DumpExpression(const Expression *expr, int indent = 0);
    std::string DumpStatement(const Statement *stmt);

} // namespace sql
