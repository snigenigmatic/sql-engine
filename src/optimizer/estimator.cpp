#include "optimizer/estimator.h"
#include <algorithm>

namespace sql
{

    namespace
    {
        double Clamp(double p) { return std::min(1.0, std::max(0.0, p)); }

        std::optional<double> Number(const Value &value)
        {
            if (value.IsNull())
                return std::nullopt;
            if (value.GetType() == DataType::INTEGER)
                return static_cast<double>(value.GetAsInt());
            if (value.GetType() == DataType::FLOAT)
                return value.GetAsFloat();
            return std::nullopt;
        }

        // a > b becomes b < a, and so on
        TokenType Mirror(TokenType op)
        {
            switch (op)
            {
            case TokenType::LT:
                return TokenType::GT;
            case TokenType::GT:
                return TokenType::LT;
            case TokenType::LEQ:
                return TokenType::GEQ;
            case TokenType::GEQ:
                return TokenType::LEQ;
            default:
                return op;
            }
        }
    } // namespace

    double Estimator::Rows(size_t relation) const
    {
        return static_cast<double>(scope_.Relations().at(relation).schema->GetTupleCount());
    }

    const ColumnStats *Estimator::Stats(const Scope::Resolved &column) const
    {
        const TableStats *stats = catalog_ ? catalog_->GetStats(scope_.Relations().at(column.relation).table) : nullptr;
        if (stats == nullptr || column.column >= stats->columns.size() || stats->rows == 0)
            return nullptr;
        return &stats->columns[column.column];
    }

    double Estimator::Distinct(const Scope::Resolved &column) const
    {
        const double rows = std::max(1.0, Rows(column.relation));
        if (const ColumnStats *stats = Stats(column))
            return std::min(rows, std::max(1.0, static_cast<double>(stats->distinct)));
        // Unknown: assume a key, so an equi-join keeps the larger input's size
        return rows;
    }

    bool Estimator::Unique(const Scope::Resolved &column) const
    {
        if (catalog_ == nullptr)
            return false;
        for (const IndexInfo *index : catalog_->GetTableIndexes(scope_.Relations().at(column.relation).table))
            if (index->unique && index->column_index == static_cast<int>(column.column))
                return true;
        return false;
    }

    double Estimator::NullFraction(const Scope::Resolved &column) const
    {
        if (const ColumnStats *stats = Stats(column))
        {
            const TableStats *table = catalog_->GetStats(scope_.Relations().at(column.relation).table);
            return static_cast<double>(stats->nulls) / static_cast<double>(table->rows);
        }
        return DEFAULT_IS_NULL;
    }

    std::optional<double> Estimator::RangeFraction(const Scope::Resolved &column, const std::optional<Value> &low,
                                                   const std::optional<Value> &high) const
    {
        const ColumnStats *stats = Stats(column);
        if (stats == nullptr || !stats->min || !stats->max)
            return std::nullopt;
        const std::optional<double> min = Number(*stats->min), max = Number(*stats->max);
        if (!min || !max)
            return std::nullopt;
        double from = *min, to = *max;
        if (low)
        {
            const std::optional<double> l = Number(*low);
            if (!l)
                return std::nullopt;
            from = std::max(from, *l);
        }
        if (high)
        {
            const std::optional<double> h = Number(*high);
            if (!h)
                return std::nullopt;
            to = std::min(to, *h);
        }
        const double non_null = 1.0 - NullFraction(column);
        if (to < from)
            return 0.01;
        if (*max == *min)
            return non_null;
        return std::max(0.01, non_null * (to - from) / (*max - *min));
    }

    std::optional<Scope::Resolved> Estimator::Column(const Expression *expr) const
    {
        if (expr == nullptr || expr->GetType() != ExpressionType::COLUMN_REF)
            return std::nullopt;
        const std::string &name = static_cast<const ColumnExpression *>(expr)->name;
        if (!scope_.CanResolve(name))
            return std::nullopt;
        return scope_.Resolve(name);
    }

    double Estimator::Comparison(TokenType op, const Scope::Resolved &column, const Value &literal) const
    {
        if (literal.IsNull())
            return 0.0; // never true
        const bool analyzed = Stats(column) != nullptr;
        // A unique column holds each value at most once, analyzed or not
        const double equal = analyzed      ? (1.0 - NullFraction(column)) / Distinct(column)
                              : Unique(column) ? 1.0 / std::max(1.0, Rows(column.relation))
                                               : DEFAULT_EQUALITY;
        switch (op)
        {
        case TokenType::EQ:
            return equal;
        case TokenType::NEQ:
            return Clamp((analyzed ? 1.0 - NullFraction(column) : 1.0) - equal);
        case TokenType::LT:
        case TokenType::LEQ:
            return RangeFraction(column, std::nullopt, literal).value_or(DEFAULT_RANGE);
        case TokenType::GT:
        case TokenType::GEQ:
            return RangeFraction(column, literal, std::nullopt).value_or(DEFAULT_RANGE);
        default:
            return DEFAULT_OTHER;
        }
    }

    double Estimator::Selectivity(const Expression *condition) const
    {
        if (condition == nullptr)
            return 1.0;
        switch (condition->GetType())
        {
        case ExpressionType::LITERAL:
        {
            const Value &v = static_cast<const LiteralExpression *>(condition)->value;
            if (!v.IsNull() && v.GetType() == DataType::BOOLEAN)
                return v.GetAsBool() ? 1.0 : 0.0;
            return DEFAULT_OTHER;
        }
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(condition);
            if (bin->op == TokenType::AND)
                return Selectivity(bin->left.get()) * Selectivity(bin->right.get());
            if (bin->op == TokenType::OR)
            {
                const double p = Selectivity(bin->left.get()), q = Selectivity(bin->right.get());
                return Clamp(p + q - p * q);
            }
            const std::optional<Scope::Resolved> left = Column(bin->left.get());
            const std::optional<Scope::Resolved> right = Column(bin->right.get());
            if (left && bin->right->GetType() == ExpressionType::LITERAL)
                return Comparison(bin->op, *left, static_cast<const LiteralExpression *>(bin->right.get())->value);
            if (right && bin->left->GetType() == ExpressionType::LITERAL)
                return Comparison(Mirror(bin->op), *right,
                                  static_cast<const LiteralExpression *>(bin->left.get())->value);
            if (left && right && bin->op == TokenType::EQ)
                return 1.0 / std::max(Distinct(*left), Distinct(*right));
            return DEFAULT_OTHER;
        }
        case ExpressionType::UNARY_OP:
        {
            const auto *unary = static_cast<const UnaryExpression *>(condition);
            if (unary->op == TokenType::NOT)
                return Clamp(1.0 - Selectivity(unary->operand.get()));
            return DEFAULT_OTHER;
        }
        case ExpressionType::IS_NULL:
        {
            const auto *is_null = static_cast<const IsNullExpression *>(condition);
            const std::optional<Scope::Resolved> column = Column(is_null->operand.get());
            const double p = column ? NullFraction(*column) : DEFAULT_IS_NULL;
            return is_null->negated ? Clamp(1.0 - p) : p;
        }
        case ExpressionType::LIKE:
            return static_cast<const LikeExpression *>(condition)->negated ? 1.0 - DEFAULT_LIKE : DEFAULT_LIKE;
        case ExpressionType::IN_LIST:
        {
            const auto *in = static_cast<const InListExpression *>(condition);
            const std::optional<Scope::Resolved> column = Column(in->operand.get());
            double p = DEFAULT_OTHER;
            if (column)
            {
                p = 0.0;
                for (const auto &item : in->list)
                {
                    if (item->GetType() == ExpressionType::LITERAL)
                        p += Comparison(TokenType::EQ, *column, static_cast<const LiteralExpression *>(item.get())->value);
                    else
                        p += DEFAULT_EQUALITY;
                }
                p = Clamp(p);
            }
            return in->negated ? Clamp(1.0 - p) : p;
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(condition);
            const std::optional<Scope::Resolved> column = Column(between->operand.get());
            double p = DEFAULT_RANGE;
            if (column && between->low->GetType() == ExpressionType::LITERAL &&
                between->high->GetType() == ExpressionType::LITERAL)
                p = RangeFraction(*column, static_cast<const LiteralExpression *>(between->low.get())->value,
                                  static_cast<const LiteralExpression *>(between->high.get())->value)
                        .value_or(DEFAULT_RANGE);
            return between->negated ? Clamp(1.0 - p) : p;
        }
        default:
            return DEFAULT_OTHER;
        }
    }

    double Estimator::JoinRows(double left_rows, double right_rows, const Scope::Resolved &left,
                               const Scope::Resolved &right) const
    {
        return left_rows * right_rows / std::max(Distinct(left), Distinct(right));
    }

} // namespace sql
