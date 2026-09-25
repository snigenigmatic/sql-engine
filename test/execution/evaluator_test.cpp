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

    TEST(EvaluatorTest, LikePatterns)
    {
        auto like = [](const std::string &s, const std::string &p) { return Tri(EvaluateLike(Value(s), Value(p))); };
        EXPECT_EQ(like("apple", "ap%"), "T");
        EXPECT_EQ(like("apple", "%le"), "T");
        EXPECT_EQ(like("apple", "%pp%"), "T");
        EXPECT_EQ(like("apple", "a_ple"), "T");
        EXPECT_EQ(like("apple", "a_le"), "F");
        EXPECT_EQ(like("apple", "apple"), "T");
        EXPECT_EQ(like("apple", "Apple"), "F"); // case-sensitive
        EXPECT_EQ(like("", "%"), "T");
        EXPECT_EQ(like("", "_"), "F");
        EXPECT_EQ(like("a.b", "a.b"), "T");  // no regex meaning
        EXPECT_EQ(like("axb", "a.b"), "F");
        EXPECT_EQ(like("mississippi", "%iss%ppi"), "T"); // backtracking
        EXPECT_EQ(like("abc", "%%%c"), "T");
        EXPECT_EQ(Tri(EvaluateLike(Value(DataType::VARCHAR), Value("%"))), "N");
        EXPECT_THROW(EvaluateLike(Value(1), Value("1")), std::runtime_error);
    }

    TEST(EvaluatorTest, UnaryMinusAndOverflow)
    {
        EXPECT_EQ(EvaluateUnaryOp(TokenType::MINUS, Value(5)).GetAsInt(), -5);
        EXPECT_DOUBLE_EQ(EvaluateUnaryOp(TokenType::MINUS, Value(2.5)).GetAsFloat(), -2.5);
        EXPECT_TRUE(EvaluateUnaryOp(TokenType::MINUS, Value(DataType::INTEGER)).IsNull());
        EXPECT_THROW(EvaluateUnaryOp(TokenType::MINUS, Value(INT32_MIN)), std::runtime_error);
        EXPECT_THROW(EvaluateUnaryOp(TokenType::MINUS, Value("x")), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::PLUS, Value(INT32_MAX), Value(1)), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::MINUS, Value(INT32_MIN), Value(1)), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::STAR, Value(1 << 20), Value(1 << 20)), std::runtime_error);
        EXPECT_THROW(EvaluateBinaryOp(TokenType::SLASH, Value(INT32_MIN), Value(-1)), std::runtime_error);
    }

    TEST(EvaluatorTest, InAndBetweenThreeValued)
    {
        auto in_list = [](Value operand, std::vector<Value> items, bool negated)
        {
            std::vector<std::unique_ptr<Expression>> list;
            for (auto &v : items)
                list.push_back(std::make_unique<LiteralExpression>(v));
            InListExpression expr(std::make_unique<LiteralExpression>(operand), std::move(list), negated);
            return Tri(EvaluateExpression(&expr, [](const ColumnExpression &) -> Value
                                          { throw std::runtime_error("no columns"); }));
        };
        EXPECT_EQ(in_list(Value(2), {Value(1), Value(2)}, false), "T");
        EXPECT_EQ(in_list(Value(3), {Value(1), Value(2)}, false), "F");
        EXPECT_EQ(in_list(Value(3), {Value(1), Value()}, false), "N");  // might have matched
        EXPECT_EQ(in_list(Value(1), {Value(1), Value()}, false), "T");
        EXPECT_EQ(in_list(Value(3), {Value(1), Value()}, true), "N");   // NOT IN with a NULL
        EXPECT_EQ(in_list(Value(), {Value(1)}, false), "N");
        EXPECT_EQ(in_list(Value(2), {Value(2.0)}, false), "T");          // numeric equality

        auto between = [](Value v, Value lo, Value hi, bool negated)
        {
            BetweenExpression expr(std::make_unique<LiteralExpression>(v), std::make_unique<LiteralExpression>(lo),
                                   std::make_unique<LiteralExpression>(hi), negated);
            return Tri(EvaluateExpression(&expr, [](const ColumnExpression &) -> Value
                                          { throw std::runtime_error("no columns"); }));
        };
        EXPECT_EQ(between(Value(5), Value(1), Value(5), false), "T"); // inclusive
        EXPECT_EQ(between(Value(6), Value(1), Value(5), false), "F");
        EXPECT_EQ(between(Value(6), Value(1), Value(5), true), "T");
        EXPECT_EQ(between(Value(0), Value(), Value(5), false), "N");
        EXPECT_EQ(between(Value(9), Value(), Value(5), false), "F");  // NULL AND FALSE
    }

    TEST(EvaluatorTest, SortOrderIsTotal)
    {
        EXPECT_LT(CompareForSort(Value(DataType::INTEGER), Value(-100)), 0); // NULL first
        EXPECT_EQ(CompareForSort(Value(), Value(DataType::VARCHAR)), 0);
        EXPECT_LT(CompareForSort(Value(2), Value(2.5)), 0);                // numeric across types
        EXPECT_EQ(CompareForSort(Value(2), Value(2.0)), 0);
        EXPECT_GT(CompareForSort(Value("b"), Value("a")), 0);
        EXPECT_NE(CompareForSort(Value(1), Value("1")), 0);                // different types never throw
        EXPECT_EQ(CompareForSort(Value(1), Value("1")), -CompareForSort(Value("1"), Value(1)));
    }

    TEST(EvaluatorTest, AggregatesCannotBeEvaluatedDirectly)
    {
        AggregateExpression count(AggregateFunction::COUNT, nullptr, false);
        try
        {
            EvaluateExpression(&count, [](const ColumnExpression &) { return Value(1); });
            FAIL() << "expected an error";
        }
        catch (const std::runtime_error &e)
        {
            EXPECT_STREQ(e.what(), "Misuse of aggregate function COUNT()");
        }
    }

    TEST(EvaluatorTest, GroupKeysFollowEquality)
    {
        auto key = [](const Value &v)
        {
            std::string k;
            AppendGroupKey(v, &k);
            return k;
        };
        EXPECT_EQ(key(Value(1)), key(Value(1.0)));
        EXPECT_NE(key(Value(1)), key(Value(1.5)));
        EXPECT_EQ(key(Value(0.0)), key(Value(-0.0)));
        EXPECT_EQ(key(Value()), key(Value(DataType::VARCHAR))); // every NULL alike
        EXPECT_NE(key(Value()), key(Value(0)));
        EXPECT_NE(key(Value(1)), key(Value("1")));
        EXPECT_NE(key(Value(true)), key(Value(1)));
        EXPECT_EQ(key(Value("ab")), key(Value("ab")));
        // Keys of several values concatenate without ambiguity
        EXPECT_NE(key(Value("a")) + key(Value("bc")), key(Value("ab")) + key(Value("c")));
    }

} // namespace sql
