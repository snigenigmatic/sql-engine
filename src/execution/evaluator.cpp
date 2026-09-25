#include "execution/evaluator.h"
#include <stdexcept>

namespace sql
{

    namespace
    {
        const Value NULL_BOOL = Value(DataType::BOOLEAN);

        bool IsNumeric(const Value &v)
        {
            return v.GetType() == DataType::INTEGER || v.GetType() == DataType::FLOAT;
        }

        double AsDouble(const Value &v)
        {
            return v.GetType() == DataType::INTEGER ? static_cast<double>(v.GetAsInt()) : v.GetAsFloat();
        }

        // -1 / 0 / 1; operands are non-NULL
        int Compare(const Value &left, const Value &right)
        {
            if (IsNumeric(left) && IsNumeric(right) && left.GetType() != right.GetType())
            {
                const double l = AsDouble(left), r = AsDouble(right);
                return l < r ? -1 : (r < l ? 1 : 0);
            }
            if (left.GetType() != right.GetType())
                throw std::runtime_error("Cannot compare " + left.ToString() + " with " + right.ToString() +
                                         ": different types");
            return left < right ? -1 : (right < left ? 1 : 0);
        }

        bool AsBool(const Value &v, const char *op)
        {
            if (v.GetType() != DataType::BOOLEAN)
                throw std::runtime_error(std::string(op) + " requires boolean operands, got " + v.ToString());
            return v.GetAsBool();
        }

        Value Arithmetic(TokenType op, const Value &left, const Value &right)
        {
            if (!IsNumeric(left) || !IsNumeric(right))
                throw std::runtime_error("Arithmetic requires numeric operands");
            if (left.GetType() == DataType::INTEGER && right.GetType() == DataType::INTEGER)
            {
                const int32_t l = left.GetAsInt(), r = right.GetAsInt();
                switch (op)
                {
                case TokenType::PLUS:
                    return Value(l + r);
                case TokenType::MINUS:
                    return Value(l - r);
                case TokenType::STAR:
                    return Value(l * r);
                case TokenType::SLASH:
                    if (r == 0)
                        throw std::runtime_error("Division by zero");
                    return Value(l / r);
                default:
                    break;
                }
            }
            else
            {
                const double l = AsDouble(left), r = AsDouble(right);
                switch (op)
                {
                case TokenType::PLUS:
                    return Value(l + r);
                case TokenType::MINUS:
                    return Value(l - r);
                case TokenType::STAR:
                    return Value(l * r);
                case TokenType::SLASH:
                    if (r == 0.0)
                        throw std::runtime_error("Division by zero");
                    return Value(l / r);
                default:
                    break;
                }
            }
            throw std::runtime_error("Unknown arithmetic operator");
        }
    } // namespace

    Value EvaluateBinaryOp(TokenType op, const Value &left, const Value &right)
    {
        switch (op)
        {
        case TokenType::AND:
        {
            // FALSE wins, then NULL
            if ((!left.IsNull() && !AsBool(left, "AND")) || (!right.IsNull() && !AsBool(right, "AND")))
                return Value(false);
            if (left.IsNull() || right.IsNull())
                return NULL_BOOL;
            return Value(true);
        }
        case TokenType::OR:
        {
            // TRUE wins, then NULL
            if ((!left.IsNull() && AsBool(left, "OR")) || (!right.IsNull() && AsBool(right, "OR")))
                return Value(true);
            if (left.IsNull() || right.IsNull())
                return NULL_BOOL;
            return Value(false);
        }
        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::LT:
        case TokenType::GT:
        case TokenType::LEQ:
        case TokenType::GEQ:
        {
            if (left.IsNull() || right.IsNull())
                return NULL_BOOL;
            // Values of unrelated types are never equal
            if (op == TokenType::EQ || op == TokenType::NEQ)
            {
                const bool comparable = left.GetType() == right.GetType() || (IsNumeric(left) && IsNumeric(right));
                const bool equal = comparable && Compare(left, right) == 0;
                return Value(op == TokenType::EQ ? equal : !equal);
            }
            const int c = Compare(left, right);
            switch (op)
            {
            case TokenType::LT:
                return Value(c < 0);
            case TokenType::GT:
                return Value(c > 0);
            case TokenType::LEQ:
                return Value(c <= 0);
            default:
                return Value(c >= 0);
            }
        }
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
            if (left.IsNull() || right.IsNull())
                return Value(IsNumeric(left) && IsNumeric(right) && left.GetType() == right.GetType()
                                 ? left.GetType()
                                 : DataType::FLOAT);
            return Arithmetic(op, left, right);
        default:
            throw std::runtime_error("Unknown binary operator: " + TokenToString(op));
        }
    }

    Value EvaluateUnaryOp(TokenType op, const Value &operand)
    {
        if (op == TokenType::NOT)
        {
            if (operand.IsNull())
                return NULL_BOOL;
            return Value(!AsBool(operand, "NOT"));
        }
        throw std::runtime_error("Unknown unary operator: " + TokenToString(op));
    }

    Value EvaluateExpression(const Expression *expr, const ColumnResolver &resolve_column)
    {
        if (!expr)
            throw std::runtime_error("Null expression");

        switch (expr->GetType())
        {
        case ExpressionType::LITERAL:
            return static_cast<const LiteralExpression *>(expr)->value;
        case ExpressionType::COLUMN_REF:
            return resolve_column(*static_cast<const ColumnExpression *>(expr));
        case ExpressionType::UNARY_OP:
        {
            const auto *unary = static_cast<const UnaryExpression *>(expr);
            return EvaluateUnaryOp(unary->op, EvaluateExpression(unary->operand.get(), resolve_column));
        }
        case ExpressionType::IS_NULL:
        {
            const auto *is_null = static_cast<const IsNullExpression *>(expr);
            const bool null = EvaluateExpression(is_null->operand.get(), resolve_column).IsNull();
            return Value(is_null->negated ? !null : null);
        }
        case ExpressionType::BINARY_OP:
        {
            const auto *bin = static_cast<const BinaryExpression *>(expr);
            Value left = EvaluateExpression(bin->left.get(), resolve_column);
            // Short-circuit when the left side decides AND / OR
            if (bin->op == TokenType::AND && !left.IsNull() && !AsBool(left, "AND"))
                return Value(false);
            if (bin->op == TokenType::OR && !left.IsNull() && AsBool(left, "OR"))
                return Value(true);
            Value right = EvaluateExpression(bin->right.get(), resolve_column);
            return EvaluateBinaryOp(bin->op, left, right);
        }
        }
        throw std::runtime_error("Unknown expression type");
    }

    bool IsTrue(const Value &value)
    {
        if (value.IsNull())
            return false;
        return AsBool(value, "WHERE");
    }

} // namespace sql
