#include "execution/filter.h"
#include <stdexcept>

namespace sql
{

    void Filter::Open()
    {
        if (child_)
        {
            child_->Open();
        }
    }

    bool Filter::Next(Tuple *tuple)
    {
        if (!child_)
        {
            return false;
        }

        // Keep fetching tuples until we find one that satisfies the predicate
        while (child_->Next(tuple))
        {
            if (predicate_ == nullptr || EvaluatePredicate(*tuple))
            {
                return true;
            }
        }
        return false;
    }

    void Filter::Close()
    {
        if (child_)
        {
            child_->Close();
        }
    }

    Value Filter::Evaluate(const Expression *expr, const Tuple &tuple) const
    {
        if (!expr)
        {
            throw std::runtime_error("Null expression");
        }

        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
        {
            const auto *lit = static_cast<const LiteralExpression *>(expr);
            return lit->value;
        }
        case ExpressionType::COLUMN_REF:
        {
            const auto *col = static_cast<const ColumnExpression *>(expr);
            int idx = table_->GetColumnIndex(col->name);
            if (idx < 0)
            {
                throw std::runtime_error("Unknown column: " + col->name);
            }
            return tuple.GetValue(static_cast<size_t>(idx));
        }
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            Value left = Evaluate(bin->left.get(), tuple);
            Value right = Evaluate(bin->right.get(), tuple);

            switch (bin->op)
            {
            // Comparison operators
            case TokenType::EQ:
                return Value(left == right);
            case TokenType::NEQ:
                return Value(left != right);
            case TokenType::LT:
                return Value(left < right);
            case TokenType::GT:
                return Value(left > right);
            case TokenType::LEQ:
                return Value(left <= right);
            case TokenType::GEQ:
                return Value(left >= right);

            // Logical operators
            case TokenType::AND:
                return Value(left.GetAsBool() && right.GetAsBool());
            case TokenType::OR:
                return Value(left.GetAsBool() || right.GetAsBool());

            // Arithmetic operators
            case TokenType::PLUS:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                {
                    return Value(left.GetAsInt() + right.GetAsInt());
                }
                else
                {
                    return Value(left.GetAsFloat() + right.GetAsFloat());
                }
            case TokenType::MINUS:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                {
                    return Value(left.GetAsInt() - right.GetAsInt());
                }
                else
                {
                    return Value(left.GetAsFloat() - right.GetAsFloat());
                }
            case TokenType::STAR:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                {
                    return Value(left.GetAsInt() * right.GetAsInt());
                }
                else
                {
                    return Value(left.GetAsFloat() * right.GetAsFloat());
                }
            case TokenType::SLASH:
                if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
                {
                    if (right.GetAsInt() == 0)
                    {
                        throw std::runtime_error("Division by zero");
                    }
                    return Value(left.GetAsInt() / right.GetAsInt());
                }
                else
                {
                    if (right.GetAsFloat() == 0.0)
                    {
                        throw std::runtime_error("Division by zero");
                    }
                    return Value(left.GetAsFloat() / right.GetAsFloat());
                }

            default:
                throw std::runtime_error("Unknown binary operator");
            }
        }
        default:
            throw std::runtime_error("Unknown expression type");
        }
    }

    bool Filter::EvaluatePredicate(const Tuple &tuple) const
    {
        if (!predicate_)
        {
            return true; // No predicate means all tuples pass
        }
        Value result = Evaluate(predicate_, tuple);
        return result.GetAsBool();
    }

} // namespace sql
