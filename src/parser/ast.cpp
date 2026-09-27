#include "parser/ast.h"
#include <sstream>
#include <stdexcept>

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
        case ExpressionType::AGGREGATE:
            return "AGGREGATE";
        case ExpressionType::SUBQUERY:
            return "SUBQUERY";
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
        case StatementType::ANALYZE_STMT:
            return "ANALYZE";
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
        case ExpressionType::AGGREGATE:
        {
            const auto *aggregate = static_cast<const AggregateExpression *>(expr);
            out << Indent(indent) << "Aggregate(" << AggregateFunctionName(aggregate->function)
                << (aggregate->distinct ? " DISTINCT" : "") << (aggregate->argument ? ")\n" : ", *)");
            if (aggregate->argument)
                out << DumpExpression(aggregate->argument.get(), indent + 1);
            break;
        }
        case ExpressionType::SUBQUERY:
        {
            const auto *subquery = static_cast<const SubqueryExpression *>(expr);
            out << Indent(indent) << "Subquery(" << ExpressionToSQL(expr) << ")";
            if (subquery->operand)
                out << "\n" << DumpExpression(subquery->operand.get(), indent + 1);
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
            const bool simple =
                t == ExpressionType::LITERAL || t == ExpressionType::COLUMN_REF || t == ExpressionType::AGGREGATE ||
                (t == ExpressionType::SUBQUERY &&
                 static_cast<const SubqueryExpression *>(expr)->kind == SubqueryExpression::Kind::SCALAR);
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
        case ExpressionType::AGGREGATE:
        {
            const auto *aggregate = static_cast<const AggregateExpression *>(expr);
            return std::string(AggregateFunctionName(aggregate->function)) + "(" +
                   (aggregate->distinct ? "DISTINCT " : "") +
                   (aggregate->argument ? ExpressionToSQL(aggregate->argument.get()) : "*") + ")";
        }
        case ExpressionType::SUBQUERY:
        {
            const auto *subquery = static_cast<const SubqueryExpression *>(expr);
            const std::string body = "(" + SelectToSQL(*subquery->select) + ")";
            switch (subquery->kind)
            {
            case SubqueryExpression::Kind::SCALAR:
                return body;
            case SubqueryExpression::Kind::EXISTS:
                return (subquery->negated ? "NOT EXISTS " : "EXISTS ") + body;
            case SubqueryExpression::Kind::IN:
                return OperandSQL(subquery->operand.get()) + (subquery->negated ? " NOT IN " : " IN ") + body;
            }
            break;
        }
        }
        return "?";
    }

    SubqueryExpression::SubqueryExpression(Kind k, bool n, std::unique_ptr<Expression> o,
                                           std::unique_ptr<SelectStatement> s)
        : kind(k), negated(n), operand(std::move(o)), select(std::move(s)) {}

    SubqueryExpression::~SubqueryExpression() = default;

    std::string SelectToSQL(const SelectStatement &select)
    {
        std::string sql = select.distinct ? "SELECT DISTINCT " : "SELECT ";
        if (select.select_star)
            sql += "*";
        for (size_t i = 0; i < select.items.size(); ++i)
        {
            sql += (i ? ", " : "") + ExpressionToSQL(select.items[i].expr.get());
            if (!select.items[i].alias.empty())
                sql += " AS " + select.items[i].alias;
        }
        sql += " FROM " + select.table + (select.table_alias.empty() ? "" : " " + select.table_alias);
        for (const auto &join : select.joins)
        {
            sql += join.type == JoinType::LEFT ? " LEFT JOIN " : join.type == JoinType::CROSS ? " CROSS JOIN " : " JOIN ";
            sql += join.right.table + (join.right.alias.empty() ? "" : " " + join.right.alias);
            if (join.on)
                sql += " ON " + ExpressionToSQL(join.on.get());
        }
        if (select.where)
            sql += " WHERE " + ExpressionToSQL(select.where.get());
        for (size_t i = 0; i < select.group_by.size(); ++i)
            sql += (i ? ", " : " GROUP BY ") + ExpressionToSQL(select.group_by[i].get());
        if (select.having)
            sql += " HAVING " + ExpressionToSQL(select.having.get());
        for (size_t i = 0; i < select.order_by.size(); ++i)
            sql += (i ? ", " : " ORDER BY ") + ExpressionToSQL(select.order_by[i].expr.get()) +
                   (select.order_by[i].descending ? " DESC" : "");
        if (select.limit)
            sql += " LIMIT " + std::to_string(*select.limit);
        if (select.offset > 0)
            sql += " OFFSET " + std::to_string(select.offset);
        return sql;
    }

    std::unique_ptr<SelectStatement> CloneSelect(
        const SelectStatement &select, const std::function<std::unique_ptr<Expression>(const Expression *)> &replace)
    {
        auto copy = [&](const std::unique_ptr<Expression> &e)
        { return RewriteExpression(e.get(), replace); };
        auto clone = std::make_unique<SelectStatement>();
        clone->table = select.table;
        clone->table_alias = select.table_alias;
        for (const auto &join : select.joins)
        {
            JoinClause j;
            j.type = join.type;
            j.right = join.right;
            j.on = copy(join.on);
            clone->joins.push_back(std::move(j));
        }
        for (const auto &item : select.items)
            clone->items.push_back(SelectItem{copy(item.expr), item.alias});
        clone->columns = select.columns;
        clone->select_star = select.select_star;
        clone->distinct = select.distinct;
        clone->where = copy(select.where);
        for (const auto &key : select.group_by)
            clone->group_by.push_back(copy(key));
        clone->having = copy(select.having);
        for (const auto &order : select.order_by)
            clone->order_by.push_back(OrderItem{copy(order.expr), order.descending});
        clone->limit = select.limit;
        clone->offset = select.offset;
        return clone;
    }

    std::vector<const Expression *> SelectExpressions(const SelectStatement &select)
    {
        std::vector<const Expression *> exprs;
        for (const auto &item : select.items)
            exprs.push_back(item.expr.get());
        for (const auto &join : select.joins)
        {
            if (join.on)
                exprs.push_back(join.on.get());
        }
        if (select.where)
            exprs.push_back(select.where.get());
        for (const auto &key : select.group_by)
            exprs.push_back(key.get());
        if (select.having)
            exprs.push_back(select.having.get());
        for (const auto &order : select.order_by)
            exprs.push_back(order.expr.get());
        return exprs;
    }

    const char *AggregateFunctionName(AggregateFunction function)
    {
        switch (function)
        {
        case AggregateFunction::COUNT:
            return "COUNT";
        case AggregateFunction::SUM:
            return "SUM";
        case AggregateFunction::AVG:
            return "AVG";
        case AggregateFunction::MIN:
            return "MIN";
        case AggregateFunction::MAX:
            return "MAX";
        }
        return "?";
    }

    // The direct subexpressions of an expression, in order
    std::vector<const Expression *> ExpressionChildren(const Expression *expr)
    {
        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
        case ExpressionType::COLUMN_REF:
            return {};
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            return {bin->left.get(), bin->right.get()};
        }
        case ExpressionType::UNARY_OP:
            return {static_cast<const UnaryExpression *>(expr)->operand.get()};
        case ExpressionType::IS_NULL:
            return {static_cast<const IsNullExpression *>(expr)->operand.get()};
        case ExpressionType::LIKE:
        {
            const auto *like = static_cast<const LikeExpression *>(expr);
            return {like->value.get(), like->pattern.get()};
        }
        case ExpressionType::IN_LIST:
        {
            const auto *in = static_cast<const InListExpression *>(expr);
            std::vector<const Expression *> children{in->operand.get()};
            for (const auto &item : in->list)
                children.push_back(item.get());
            return children;
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(expr);
            return {between->operand.get(), between->low.get(), between->high.get()};
        }
        case ExpressionType::AGGREGATE:
        {
            const auto *aggregate = static_cast<const AggregateExpression *>(expr);
            if (aggregate->argument)
                return {aggregate->argument.get()};
            return {};
        }
        case ExpressionType::SUBQUERY:
        {
            const auto *subquery = static_cast<const SubqueryExpression *>(expr);
            if (subquery->operand)
                return {subquery->operand.get()};
            return {};
        }
        }
        return {};
    }

    const char *JoinTypeName(JoinType type)
    {
        switch (type)
        {
        case JoinType::INNER:
            return "INNER";
        case JoinType::LEFT:
            return "LEFT";
        case JoinType::CROSS:
            return "CROSS";
        }
        return "?";
    }

    bool ContainsAggregate(const Expression *expr)
    {
        if (expr == nullptr)
            return false;
        if (expr->GetType() == ExpressionType::AGGREGATE)
            return true;
        for (const Expression *child : ExpressionChildren(expr))
        {
            if (ContainsAggregate(child))
                return true;
        }
        return false;
    }

    bool ExpressionsEqual(const Expression *a, const Expression *b,
                          const std::function<bool(const std::string &, const std::string &)> &same_column)
    {
        if (a == nullptr || b == nullptr)
            return a == b;
        if (a->GetType() != b->GetType())
            return false;
        switch (a->GetType())
        {
        case ExpressionType::LITERAL:
        {
            const Value &x = static_cast<const LiteralExpression *>(a)->value;
            const Value &y = static_cast<const LiteralExpression *>(b)->value;
            if (x.IsNull() || y.IsNull())
                return x.IsNull() && y.IsNull();
            return x.GetType() == y.GetType() && x == y;
        }
        case ExpressionType::COLUMN_REF:
            return same_column(static_cast<const ColumnExpression *>(a)->name,
                               static_cast<const ColumnExpression *>(b)->name);
        case ExpressionType::BINARY_OP:
            if (static_cast<const BinaryExpression *>(a)->op != static_cast<const BinaryExpression *>(b)->op)
                return false;
            break;
        case ExpressionType::UNARY_OP:
            if (static_cast<const UnaryExpression *>(a)->op != static_cast<const UnaryExpression *>(b)->op)
                return false;
            break;
        case ExpressionType::IS_NULL:
            if (static_cast<const IsNullExpression *>(a)->negated != static_cast<const IsNullExpression *>(b)->negated)
                return false;
            break;
        case ExpressionType::LIKE:
            if (static_cast<const LikeExpression *>(a)->negated != static_cast<const LikeExpression *>(b)->negated)
                return false;
            break;
        case ExpressionType::IN_LIST:
            if (static_cast<const InListExpression *>(a)->negated != static_cast<const InListExpression *>(b)->negated)
                return false;
            break;
        case ExpressionType::BETWEEN:
            if (static_cast<const BetweenExpression *>(a)->negated != static_cast<const BetweenExpression *>(b)->negated)
                return false;
            break;
        case ExpressionType::AGGREGATE:
        {
            const auto *x = static_cast<const AggregateExpression *>(a);
            const auto *y = static_cast<const AggregateExpression *>(b);
            if (x->function != y->function || x->distinct != y->distinct)
                return false;
            break;
        }
        case ExpressionType::SUBQUERY:
            // Comparing query bodies is not worth it: only a node equals itself
            return a == b;
        }
        const std::vector<const Expression *> left = ExpressionChildren(a);
        const std::vector<const Expression *> right = ExpressionChildren(b);
        if (left.size() != right.size())
            return false;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (!ExpressionsEqual(left[i], right[i], same_column))
                return false;
        }
        return true;
    }

    std::unique_ptr<Expression> RewriteExpression(
        const Expression *expr, const std::function<std::unique_ptr<Expression>(const Expression *)> &replace)
    {
        if (expr == nullptr)
            return nullptr;
        if (auto replaced = replace(expr))
            return replaced;
        auto copy = [&](const std::unique_ptr<Expression> &e)
        { return RewriteExpression(e.get(), replace); };

        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
            return std::make_unique<LiteralExpression>(static_cast<const LiteralExpression *>(expr)->value);
        case ExpressionType::COLUMN_REF:
            return std::make_unique<ColumnExpression>(static_cast<const ColumnExpression *>(expr)->name);
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            return std::make_unique<BinaryExpression>(copy(bin->left), bin->op, copy(bin->right));
        }
        case ExpressionType::UNARY_OP:
        {
            const auto *unary = static_cast<const UnaryExpression *>(expr);
            return std::make_unique<UnaryExpression>(unary->op, copy(unary->operand));
        }
        case ExpressionType::IS_NULL:
        {
            const auto *is_null = static_cast<const IsNullExpression *>(expr);
            return std::make_unique<IsNullExpression>(copy(is_null->operand), is_null->negated);
        }
        case ExpressionType::LIKE:
        {
            const auto *like = static_cast<const LikeExpression *>(expr);
            return std::make_unique<LikeExpression>(copy(like->value), copy(like->pattern), like->negated);
        }
        case ExpressionType::IN_LIST:
        {
            const auto *in = static_cast<const InListExpression *>(expr);
            std::vector<std::unique_ptr<Expression>> list;
            for (const auto &item : in->list)
                list.push_back(copy(item));
            return std::make_unique<InListExpression>(copy(in->operand), std::move(list), in->negated);
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(expr);
            return std::make_unique<BetweenExpression>(copy(between->operand), copy(between->low),
                                                       copy(between->high), between->negated);
        }
        case ExpressionType::AGGREGATE:
        {
            const auto *aggregate = static_cast<const AggregateExpression *>(expr);
            return std::make_unique<AggregateExpression>(aggregate->function, copy(aggregate->argument),
                                                         aggregate->distinct);
        }
        case ExpressionType::SUBQUERY:
        {
            const auto *subquery = static_cast<const SubqueryExpression *>(expr);
            return std::make_unique<SubqueryExpression>(subquery->kind, subquery->negated, copy(subquery->operand),
                                                        CloneSelect(*subquery->select, replace));
        }
        }
        throw std::logic_error("Unknown expression type");
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
            for (const auto &join : select->joins)
            {
                out << ", " << JoinTypeName(join.type) << " join=" << join.right.table
                    << (join.right.alias.empty() ? "" : " AS " + join.right.alias);
                if (join.on)
                    out << ", on=" << ExpressionToSQL(join.on.get());
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
