#include "parser/ast.h"
#include <sstream>

namespace sql
{

    namespace
    {
        std::string Indent(int indent)
        {
            return std::string(static_cast<size_t>(indent * 2), ' ');
        }
    } // namespace

    std::string ExpressionTypeToString(ExpressionType type)
    {
        switch (type)
        {
        case ExpressionType::LITERAL:
            return "LITERAL";
        case ExpressionType::COLUMN_REF:
            return "COLUMN_REF";
        case ExpressionType::BINARY_OP:
            return "BINARY_OP";
        case ExpressionType::UNARY_OP:
            return "UNARY_OP";
        case ExpressionType::IS_NULL:
            return "IS_NULL";
        case ExpressionType::LIKE:
            return "LIKE";
        case ExpressionType::IN_LIST:
            return "IN_LIST";
        case ExpressionType::BETWEEN:
            return "BETWEEN";
        default:
            return "UNKNOWN_EXPRESSION";
        }
    }

    std::string StatementTypeToString(StatementType type)
    {
        switch (type)
        {
        case StatementType::SELECT:
            return "SELECT";
        case StatementType::CREATE_TABLE:
            return "CREATE_TABLE";
        case StatementType::DROP_TABLE:
            return "DROP_TABLE";
        case StatementType::INSERT:
            return "INSERT";
        case StatementType::CREATE_INDEX:
            return "CREATE_INDEX";
        case StatementType::DELETE_STMT:
            return "DELETE";
        case StatementType::UPDATE_STMT:
            return "UPDATE";
        case StatementType::EXPLAIN_STMT:
            return "EXPLAIN";
        case StatementType::TRANSACTION_STMT:
            return "TRANSACTION";
        default:
            return "UNKNOWN_STATEMENT";
        }
    }

    std::string DumpExpression(const Expression *expr, int indent)
    {
        if (expr == nullptr)
        {
            return Indent(indent) + "null";
        }

        std::ostringstream out;
        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
        {
            const auto *literal = static_cast<const LiteralExpression *>(expr);
            out << Indent(indent) << "Literal(" << literal->value.ToString() << ")";
            break;
        }
        case ExpressionType::COLUMN_REF:
        {
            const auto *column = static_cast<const ColumnExpression *>(expr);
            out << Indent(indent) << "Column(" << column->name << ")";
            break;
        }
        case ExpressionType::BINARY_OP:
        {
            const auto *binary = static_cast<const BinaryExpression *>(expr);
            out << Indent(indent) << "BinaryOp(" << TokenToString(binary->op) << ")\n";
            out << DumpExpression(binary->left.get(), indent + 1) << "\n";
            out << DumpExpression(binary->right.get(), indent + 1);
            break;
        }
        case ExpressionType::UNARY_OP:
        {
            const auto *unary = static_cast<const UnaryExpression *>(expr);
            out << Indent(indent) << "UnaryOp(" << TokenToString(unary->op) << ")\n";
            out << DumpExpression(unary->operand.get(), indent + 1);
            break;
        }
        case ExpressionType::IS_NULL:
        {
            const auto *is_null = static_cast<const IsNullExpression *>(expr);
            out << Indent(indent) << (is_null->negated ? "IsNotNull" : "IsNull") << "\n";
            out << DumpExpression(is_null->operand.get(), indent + 1);
            break;
        }
        case ExpressionType::LIKE:
        {
            const auto *like = static_cast<const LikeExpression *>(expr);
            out << Indent(indent) << (like->negated ? "NotLike" : "Like") << "\n";
            out << DumpExpression(like->value.get(), indent + 1) << "\n";
            out << DumpExpression(like->pattern.get(), indent + 1);
            break;
        }
        case ExpressionType::IN_LIST:
        {
            const auto *in = static_cast<const InListExpression *>(expr);
            out << Indent(indent) << (in->negated ? "NotIn" : "In") << "\n";
            out << DumpExpression(in->operand.get(), indent + 1);
            for (const auto &item : in->list)
                out << "\n" << DumpExpression(item.get(), indent + 2);
            break;
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(expr);
            out << Indent(indent) << (between->negated ? "NotBetween" : "Between") << "\n";
            out << DumpExpression(between->operand.get(), indent + 1) << "\n";
            out << DumpExpression(between->low.get(), indent + 1) << "\n";
            out << DumpExpression(between->high.get(), indent + 1);
            break;
        }
        default:
            out << Indent(indent) << "UnknownExpression";
            break;
        }
        return out.str();
    }

    namespace
    {
        const char *OperatorSQL(TokenType op)
        {
            switch (op)
            {
            case TokenType::PLUS:
                return "+";
            case TokenType::MINUS:
                return "-";
            case TokenType::STAR:
                return "*";
            case TokenType::SLASH:
                return "/";
            case TokenType::EQ:
                return "=";
            case TokenType::NEQ:
                return "<>";
            case TokenType::LT:
                return "<";
            case TokenType::GT:
                return ">";
            case TokenType::LEQ:
                return "<=";
            case TokenType::GEQ:
                return ">=";
            case TokenType::AND:
                return "AND";
            case TokenType::OR:
                return "OR";
            case TokenType::NOT:
                return "NOT";
            default:
                return "?";
            }
        }

        // Nested operators are parenthesized so the text stays unambiguous
        std::string OperandSQL(const Expression *expr)
        {
            const ExpressionType t = expr->GetType();
            const bool simple = t == ExpressionType::LITERAL || t == ExpressionType::COLUMN_REF;
            return simple ? ExpressionToSQL(expr) : "(" + ExpressionToSQL(expr) + ")";
        }
    } // namespace

    std::string ExpressionToSQL(const Expression *expr)
    {
        if (expr == nullptr)
            return "";
        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
        {
            const Value &v = static_cast<const LiteralExpression *>(expr)->value;
            if (v.IsNull())
                return "NULL";
            if (v.GetType() == DataType::VARCHAR)
            {
                std::string quoted = "'";
                for (char c : v.GetAsString())
                    quoted += (c == '\'' ? std::string("''") : std::string(1, c));
                return quoted + "'";
            }
            if (v.GetType() == DataType::BOOLEAN)
                return v.GetAsBool() ? "TRUE" : "FALSE";
            return v.ToString();
        }
        case ExpressionType::COLUMN_REF:
            return static_cast<const ColumnExpression *>(expr)->name;
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            return OperandSQL(bin->left.get()) + " " + OperatorSQL(bin->op) + " " + OperandSQL(bin->right.get());
        }
        case ExpressionType::UNARY_OP:
        {
            const auto *unary = static_cast<const UnaryExpression *>(expr);
            if (unary->op == TokenType::MINUS)
                return "-" + OperandSQL(unary->operand.get());
            return "NOT " + OperandSQL(unary->operand.get());
        }
        case ExpressionType::IS_NULL:
        {
            const auto *is_null = static_cast<const IsNullExpression *>(expr);
            return OperandSQL(is_null->operand.get()) + (is_null->negated ? " IS NOT NULL" : " IS NULL");
        }
        case ExpressionType::LIKE:
        {
            const auto *like = static_cast<const LikeExpression *>(expr);
            return OperandSQL(like->value.get()) + (like->negated ? " NOT LIKE " : " LIKE ") +
                   OperandSQL(like->pattern.get());
        }
        case ExpressionType::IN_LIST:
        {
            const auto *in = static_cast<const InListExpression *>(expr);
            std::string text = OperandSQL(in->operand.get()) + (in->negated ? " NOT IN (" : " IN (");
            for (size_t i = 0; i < in->list.size(); ++i)
                text += (i ? ", " : "") + ExpressionToSQL(in->list[i].get());
            return text + ")";
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(expr);
            return OperandSQL(between->operand.get()) + (between->negated ? " NOT BETWEEN " : " BETWEEN ") +
                   OperandSQL(between->low.get()) + " AND " + OperandSQL(between->high.get());
        }
        }
        return "?";
    }

    std::string DumpStatement(const Statement *stmt)
    {
        if (stmt == nullptr)
        {
            return "null";
        }

        std::ostringstream out;
        switch (stmt->GetType())
        {
        case StatementType::SELECT:
        {
            const auto *select = static_cast<const SelectStatement *>(stmt);
            out << "SelectStatement(table=" << select->table << ", columns=";
            if (select->select_star)
            {
                out << "*";
            }
            else
            {
                out << "[";
                for (size_t i = 0; i < select->columns.size(); ++i)
                {
                    if (i > 0)
                        out << ", ";
                    out << select->columns[i];
                }
                out << "]";
            }
            out << ")";
            if (select->join_table.has_value())
            {
                out << ", join=" << *select->join_table
                    << ", on=" << select->join_left_column.value_or("")
                    << " = " << select->join_right_column.value_or("");
            }
            out << "\n";
            out << "  where:\n";
            out << DumpExpression(select->where.get(), 2);
            break;
        }
        case StatementType::CREATE_TABLE:
        {
            const auto *create = static_cast<const CreateTableStatement *>(stmt);
            out << "CreateTableStatement(table=" << create->table << ")\n";
            out << "  columns:";
            for (const auto &column : create->columns)
            {
                out << "\n  - " << column.name << " " << TokenToString(column.type_token);
                if (column.type_token == TokenType::VARCHAR)
                {
                    out << "(" << column.length << ")";
                }
            }
            break;
        }
        case StatementType::DROP_TABLE:
        {
            const auto *drop = static_cast<const DropTableStatement *>(stmt);
            out << "DropTableStatement(table=" << drop->table << ")";
            break;
        }
        case StatementType::INSERT:
        {
            const auto *insert = static_cast<const InsertStatement *>(stmt);
            out << "InsertStatement(table=" << insert->table << ", rows=" << insert->rows.size() << ")";
            for (size_t r = 0; r < insert->rows.size(); ++r)
            {
                out << "\n  row[" << r << "]";
                for (size_t c = 0; c < insert->rows[r].size(); ++c)
                {
                    out << "\n" << DumpExpression(insert->rows[r][c].get(), 2);
                }
            }
            break;
        }
        case StatementType::CREATE_INDEX:
        {
            const auto *create_index = static_cast<const CreateIndexStatement *>(stmt);
            out << "CreateIndexStatement(index=" << create_index->index_name
                << ", table=" << create_index->table
                << ", column=" << create_index->column << ")";
            break;
        }
        case StatementType::DELETE_STMT:
        {
            const auto *del = static_cast<const DeleteStatement *>(stmt);
            out << "DeleteStatement(table=" << del->table << ")\n";
            out << "  where:\n";
            out << DumpExpression(del->where.get(), 2);
            break;
        }
        case StatementType::UPDATE_STMT:
        {
            const auto *update = static_cast<const UpdateStatement *>(stmt);
            out << "UpdateStatement(table=" << update->table << ")\n";
            out << "  assignments:";
            for (const auto &assignment : update->assignments)
            {
                out << "\n  - " << assignment.first << " =\n";
                out << DumpExpression(assignment.second.get(), 2);
            }
            out << "\n  where:\n";
            out << DumpExpression(update->where.get(), 2);
            break;
        }
        default:
            out << "UnknownStatement";
            break;
        }
        return out.str();
    }

} // namespace sql
