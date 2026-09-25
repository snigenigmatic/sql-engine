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
        BINARY_OP,
        UNARY_OP, // NOT, unary minus
        IS_NULL,  // IS NULL / IS NOT NULL
        LIKE,     // [NOT] LIKE
        IN_LIST,  // [NOT] IN (...)
        BETWEEN   // [NOT] BETWEEN ... AND ...
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

    struct UnaryExpression : public Expression
    {
        TokenType op;
        std::unique_ptr<Expression> operand;
        UnaryExpression(TokenType o, std::unique_ptr<Expression> e) : op(o), operand(std::move(e)) {}
        ExpressionType GetType() const override { return ExpressionType::UNARY_OP; }
    };

    struct IsNullExpression : public Expression
    {
        std::unique_ptr<Expression> operand;
        bool negated; // IS NOT NULL
        IsNullExpression(std::unique_ptr<Expression> e, bool n) : operand(std::move(e)), negated(n) {}
        ExpressionType GetType() const override { return ExpressionType::IS_NULL; }
    };

    struct LikeExpression : public Expression
    {
        std::unique_ptr<Expression> value;
        std::unique_ptr<Expression> pattern; // % matches any run, _ any one character
        bool negated;
        LikeExpression(std::unique_ptr<Expression> v, std::unique_ptr<Expression> p, bool n)
            : value(std::move(v)), pattern(std::move(p)), negated(n) {}
        ExpressionType GetType() const override { return ExpressionType::LIKE; }
    };

    struct InListExpression : public Expression
    {
        std::unique_ptr<Expression> operand;
        std::vector<std::unique_ptr<Expression>> list;
        bool negated;
        InListExpression(std::unique_ptr<Expression> o, std::vector<std::unique_ptr<Expression>> l, bool n)
            : operand(std::move(o)), list(std::move(l)), negated(n) {}
        ExpressionType GetType() const override { return ExpressionType::IN_LIST; }
    };

    struct BetweenExpression : public Expression
    {
        std::unique_ptr<Expression> operand;
        std::unique_ptr<Expression> low;
        std::unique_ptr<Expression> high;
        bool negated;
        BetweenExpression(std::unique_ptr<Expression> o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> h,
                          bool n)
            : operand(std::move(o)), low(std::move(l)), high(std::move(h)), negated(n) {}
        ExpressionType GetType() const override { return ExpressionType::BETWEEN; }
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
        UPDATE_STMT,
        EXPLAIN_STMT,
        TRANSACTION_STMT
    };

    struct Statement
    {
        virtual ~Statement() = default;
        virtual StatementType GetType() const = 0;
    };

    // One entry of a SELECT list: an expression and its optional alias
    struct SelectItem
    {
        std::unique_ptr<Expression> expr;
        std::string alias; // empty if none
    };

    // ORDER BY entry: an expression, an output alias, or a 1-based position
    struct OrderItem
    {
        std::unique_ptr<Expression> expr;
        bool descending = false;
    };

    struct SelectStatement : public Statement
    {
        std::string table;
        std::optional<std::string> join_table;
        std::optional<std::string> join_left_column;
        std::optional<std::string> join_right_column;
        std::vector<SelectItem> items;    // the SELECT list (empty for SELECT *)
        std::vector<std::string> columns; // column names, when every item is a bare column
        bool select_star = false;
        bool distinct = false;
        std::vector<OrderItem> order_by;
        std::optional<int64_t> limit;
        int64_t offset = 0;

        // True when some item is more than a bare column reference
        bool HasComputedItems() const
        {
            for (const auto &item : items)
            {
                if (item.expr->GetType() != ExpressionType::COLUMN_REF)
                    return true;
            }
            return false;
        }
        std::unique_ptr<Expression> where;
        StatementType GetType() const override { return StatementType::SELECT; }
    };

    struct ColumnDef
    {
        std::string name;
        TokenType type_token; // INTEGER, VARCHAR, FLOAT, BOOLEAN
        int length = 0;       // For VARCHAR(n)
        bool not_null = false;
        bool primary_key = false;
        bool unique = false;
        std::optional<Value> default_value;
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
        std::vector<std::string> columns;                           // empty = every column, in order
        std::vector<std::vector<std::unique_ptr<Expression>>> rows; // VALUES (...), (...)
        StatementType GetType() const override { return StatementType::INSERT; }
    };

    struct CreateIndexStatement : public Statement
    {
        std::string index_name;
        std::string table;
        std::string column;
        bool unique = false;
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

    // BEGIN / COMMIT / ROLLBACK [TRANSACTION]
    struct TransactionStatement : public Statement
    {
        enum class Kind
        {
            BEGIN,
            COMMIT,
            ROLLBACK
        };
        Kind kind;
        explicit TransactionStatement(Kind k) : kind(k) {}
        StatementType GetType() const override { return StatementType::TRANSACTION_STMT; }
    };

    struct ExplainStatement : public Statement
    {
        std::unique_ptr<SelectStatement> select;
        StatementType GetType() const override { return StatementType::EXPLAIN_STMT; }
    };

    std::string ExpressionTypeToString(ExpressionType type);
    std::string StatementTypeToString(StatementType type);
    std::string DumpExpression(const Expression *expr, int indent = 0);

    // SQL text for an expression (used to name computed result columns)
    std::string ExpressionToSQL(const Expression *expr);
    std::string DumpStatement(const Statement *stmt);

} // namespace sql
