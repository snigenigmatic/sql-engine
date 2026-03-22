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
        default:
            out << Indent(indent) << "UnknownExpression";
            break;
        }
        return out.str();
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
            out << ")\n";
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
