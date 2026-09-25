#include "execution/evaluator.h"
#include <climits>
#include <cmath>
#include <cstdint>
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
                int32_t out = 0;
                bool overflow = false;
                switch (op)
                {
                case TokenType::PLUS:
                    overflow = __builtin_add_overflow(l, r, &out);
                    break;
                case TokenType::MINUS:
                    overflow = __builtin_sub_overflow(l, r, &out);
                    break;
                case TokenType::STAR:
                    overflow = __builtin_mul_overflow(l, r, &out);
                    break;
                case TokenType::SLASH:
                    if (r == 0)
                        throw std::runtime_error("Division by zero");
                    overflow = l == INT32_MIN && r == -1;
                    if (!overflow)
                        out = l / r;
                    break;
                default:
                    throw std::runtime_error("Unknown arithmetic operator");
                }
                if (overflow)
                    throw std::runtime_error("Integer overflow");
                return Value(out);
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
        if (op == TokenType::MINUS)
        {
            if (operand.IsNull())
                return operand;
            if (operand.GetType() == DataType::INTEGER)
            {
                if (operand.GetAsInt() == INT32_MIN)
                    throw std::runtime_error("Integer overflow");
                return Value(-operand.GetAsInt());
            }
            if (operand.GetType() == DataType::FLOAT)
                return Value(-operand.GetAsFloat());
            throw std::runtime_error("Unary minus requires a number, got " + operand.ToString());
        }
        throw std::runtime_error("Unknown unary operator: " + TokenToString(op));
    }

    Value EvaluateLike(const Value &value, const Value &pattern)
    {
        if (value.IsNull() || pattern.IsNull())
            return NULL_BOOL;
        if (value.GetType() != DataType::VARCHAR || pattern.GetType() != DataType::VARCHAR)
            throw std::runtime_error("LIKE requires strings");
        const std::string s = value.GetAsString();
        const std::string p = pattern.GetAsString();

        // Greedy wildcard match with backtracking to the last %
        size_t si = 0, pi = 0;
        size_t star = std::string::npos, star_s = 0;
        while (si < s.size())
        {
            if (pi < p.size() && (p[pi] == '_' || p[pi] == s[si]))
            {
                ++si;
                ++pi;
            }
            else if (pi < p.size() && p[pi] == '%')
            {
                star = pi++;
                star_s = si;
            }
            else if (star != std::string::npos)
            {
                pi = star + 1;
                si = ++star_s;
            }
            else
            {
                return Value(false);
            }
        }
        while (pi < p.size() && p[pi] == '%')
            ++pi;
        return Value(pi == p.size());
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
        case ExpressionType::LIKE:
        {
            const auto *like = static_cast<const LikeExpression *>(expr);
            Value matched = EvaluateLike(EvaluateExpression(like->value.get(), resolve_column),
                                         EvaluateExpression(like->pattern.get(), resolve_column));
            return like->negated ? EvaluateUnaryOp(TokenType::NOT, matched) : matched;
        }
        case ExpressionType::IN_LIST:
        {
            // TRUE if any item equals the operand; otherwise NULL if the
            // operand or any item is NULL (it might have matched); else FALSE
            const auto *in = static_cast<const InListExpression *>(expr);
            const Value operand = EvaluateExpression(in->operand.get(), resolve_column);
            Value result(false);
            for (const auto &item : in->list)
            {
                const Value equal = EvaluateBinaryOp(TokenType::EQ, operand,
                                                     EvaluateExpression(item.get(), resolve_column));
                if (IsTrue(equal))
                {
                    result = Value(true);
                    break;
                }
                if (equal.IsNull())
                    result = NULL_BOOL;
            }
            return in->negated ? EvaluateUnaryOp(TokenType::NOT, result) : result;
        }
        case ExpressionType::BETWEEN:
        {
            const auto *between = static_cast<const BetweenExpression *>(expr);
            const Value operand = EvaluateExpression(between->operand.get(), resolve_column);
            const Value inside = EvaluateBinaryOp(
                TokenType::AND,
                EvaluateBinaryOp(TokenType::GEQ, operand, EvaluateExpression(between->low.get(), resolve_column)),
                EvaluateBinaryOp(TokenType::LEQ, operand, EvaluateExpression(between->high.get(), resolve_column)));
            return between->negated ? EvaluateUnaryOp(TokenType::NOT, inside) : inside;
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

    std::optional<Value> ConvertNumber(const Value &value, DataType target)
    {
        if (value.IsNull() || value.GetType() == target || !IsNumeric(value) ||
            (target != DataType::INTEGER && target != DataType::FLOAT))
            return value;
        if (target == DataType::FLOAT)
            return Value(static_cast<double>(value.GetAsInt()));
        const double d = value.GetAsFloat();
        if (!(d >= INT32_MIN && d <= INT32_MAX) || std::floor(d) != d)
            return std::nullopt;
        return Value(static_cast<int32_t>(d));
    }

    int CompareForSort(const Value &a, const Value &b)
    {
        if (a.IsNull() || b.IsNull())
            return a.IsNull() == b.IsNull() ? 0 : (a.IsNull() ? -1 : 1);
        if (IsNumeric(a) && IsNumeric(b))
            return Compare(a, b);
        if (a.GetType() != b.GetType())
            return static_cast<int>(a.GetType()) < static_cast<int>(b.GetType()) ? -1 : 1;
        return a < b ? -1 : (b < a ? 1 : 0);
    }

    bool IsTrue(const Value &value)
    {
        if (value.IsNull())
            return false;
        return AsBool(value, "WHERE");
    }

} // namespace sql
