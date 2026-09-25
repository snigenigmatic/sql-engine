#include <gtest/gtest.h>
#include "execution/evaluator.h"
#include <string>

namespace sql
{

    namespace
    {
        const Value T(true);
        const Value F(false);
        const Value N = Value(DataType::BOOLEAN); // unknown

        // "T", "F" or "N" for a boolean result
        std::string Tri(const Value &v)
        {
            if (v.IsNull())
                return "N";
            return v.GetAsBool() ? "T" : "F";
        }
    } // namespace

    TEST(EvaluatorTest, AndTruthTable)
    {
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, T, T)), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, T, F)), "F");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, T, N)), "N");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, F, N)), "F");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, N, F)), "F");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::AND, N, N)), "N");
    }

    TEST(EvaluatorTest, OrTruthTable)
    {
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, F, F)), "F");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, T, F)), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, T, N)), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, N, T)), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, F, N)), "N");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::OR, N, N)), "N");
    }

    TEST(EvaluatorTest, NotTruthTable)
    {
        EXPECT_EQ(Tri(EvaluateUnaryOp(TokenType::NOT, T)), "F");
        EXPECT_EQ(Tri(EvaluateUnaryOp(TokenType::NOT, F)), "T");
        EXPECT_EQ(Tri(EvaluateUnaryOp(TokenType::NOT, N)), "N");
    }

    TEST(EvaluatorTest, ComparisonsWithNullAreUnknown)
    {
        for (TokenType op : {TokenType::EQ, TokenType::NEQ, TokenType::LT, TokenType::GT, TokenType::LEQ, TokenType::GEQ})
        {
            EXPECT_EQ(Tri(EvaluateBinaryOp(op, Value(1), Value(DataType::INTEGER))), "N");
            EXPECT_EQ(Tri(EvaluateBinaryOp(op, Value(), Value())), "N"); // NULL = NULL is unknown
            EXPECT_EQ(Tri(EvaluateBinaryOp(op, Value(DataType::VARCHAR), Value("a"))), "N");
        }
    }

    TEST(EvaluatorTest, ArithmeticWithNullIsNull)
    {
        EXPECT_TRUE(EvaluateBinaryOp(TokenType::PLUS, Value(1), Value()).IsNull());
        EXPECT_TRUE(EvaluateBinaryOp(TokenType::SLASH, Value(), Value(0)).IsNull()); // no division error
    }

    TEST(EvaluatorTest, NumericPromotion)
    {
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::EQ, Value(5), Value(5.0))), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::GT, Value(2.5), Value(2))), "T");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::LT, Value(2), Value(2.5))), "T");
        Value sum = EvaluateBinaryOp(TokenType::PLUS, Value(1), Value(0.5));
        EXPECT_EQ(sum.GetType(), DataType::FLOAT);
        EXPECT_DOUBLE_EQ(sum.GetAsFloat(), 1.5);
        Value quotient = EvaluateBinaryOp(TokenType::SLASH, Value(7), Value(2));
        EXPECT_EQ(quotient.GetType(), DataType::INTEGER);
        EXPECT_EQ(quotient.GetAsInt(), 3);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::SLASH, Value(1), Value(0)), std::runtime_error);
    }

    TEST(EvaluatorTest, UnrelatedTypes)
    {
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::EQ, Value(1), Value("1"))), "F");
        EXPECT_EQ(Tri(EvaluateBinaryOp(TokenType::NEQ, Value(1), Value("1"))), "T");
        EXPECT_THROW(EvaluateBinaryOp(TokenType::LT, Value(1), Value("1")), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::PLUS, Value("a"), Value(1)), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::AND, Value(1), T), std::runtime_error);
    }

    TEST(EvaluatorTest, WhereKeepsOnlyTrue)
    {
        EXPECT_TRUE(IsTrue(T));
        EXPECT_FALSE(IsTrue(F));
        EXPECT_FALSE(IsTrue(N));
        EXPECT_THROW(IsTrue(Value(1)), std::runtime_error);
    }

    TEST(EvaluatorTest, AndOrShortCircuit)
    {
        // FALSE AND <error> never evaluates the right side
        auto boom = std::make_unique<BinaryExpression>(std::make_unique<LiteralExpression>(Value(1)), TokenType::SLASH,
                                                       std::make_unique<LiteralExpression>(Value(0)));
        auto cmp = std::make_unique<BinaryExpression>(std::move(boom), TokenType::EQ,
                                                      std::make_unique<LiteralExpression>(Value(1)));
        BinaryExpression expr(std::make_unique<LiteralExpression>(F), TokenType::AND, std::move(cmp));
        auto no_columns = [](const ColumnExpression &) -> Value
        { throw std::runtime_error("no columns"); };
        EXPECT_EQ(Tri(EvaluateExpression(&expr, no_columns)), "F");
    }

} // namespace sql
