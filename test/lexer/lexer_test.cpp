#include <gtest/gtest.h>
#include "lexer/lexer.h"
#include <vector>

namespace sql
{

    struct ExpectedToken
    {
        TokenType type;
        std::string value;
    };

    void TestTokens(const std::string &input, const std::vector<ExpectedToken> &expected)
    {
        Lexer lexer(input);
        for (const auto &exp : expected)
        {
            Token t = lexer.NextToken();
            EXPECT_EQ(t.type, exp.type) << "Wrong token type for " << t.value;
            EXPECT_EQ(t.value, exp.value) << "Wrong literal for " << t.type;
        }
        EXPECT_EQ(lexer.NextToken().type, TokenType::END_OF_FILE);
    }

    TEST(LexerTest, TestNextToken)
    {
        std::string input = "SELECT * FROM users WHERE id = 5;";
        std::vector<ExpectedToken> expected = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::STAR, "*"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::WHERE, "WHERE"},
            {TokenType::IDENTIFIER, "id"},
            {TokenType::EQ, "="},
            {TokenType::INTEGER_LITERAL, "5"},
            {TokenType::SEMICOLON, ";"}};
        TestTokens(input, expected);
    }

    TEST(LexerTest, TestDataTypes)
    {
        std::string input = "CREATE TABLE test (id INT, name VARCHAR, active BOOLEAN);";
        std::vector<ExpectedToken> expected = {
            {TokenType::CREATE, "CREATE"},
            {TokenType::TABLE, "TABLE"},
            {TokenType::IDENTIFIER, "test"},
            {TokenType::LPAREN, "("},
            {TokenType::IDENTIFIER, "id"},
            {TokenType::INTEGER, "INT"}, // 'INT' maps to INTEGER token
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "name"},
            {TokenType::VARCHAR, "VARCHAR"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "active"},
            {TokenType::BOOLEAN, "BOOLEAN"},
            {TokenType::RPAREN, ")"},
            {TokenType::SEMICOLON, ";"}};
        TestTokens(input, expected);
    }

    TEST(LexerTest, TestLiterals)
    {
        std::string input = "INSERT INTO t VALUES (1, 'hello', 3.14, TRUE, FALSE);";
        std::vector<ExpectedToken> expected = {
            {TokenType::INSERT, "INSERT"},
            {TokenType::INTO, "INTO"},
            {TokenType::IDENTIFIER, "t"},
            {TokenType::VALUES, "VALUES"},
            {TokenType::LPAREN, "("},
            {TokenType::INTEGER_LITERAL, "1"},
            {TokenType::COMMA, ","},
            {TokenType::STRING_LITERAL, "hello"},
            {TokenType::COMMA, ","},
            {TokenType::FLOAT_LITERAL, "3.14"},
            {TokenType::COMMA, ","},
            {TokenType::TRUE, "TRUE"},
            {TokenType::COMMA, ","},
            {TokenType::FALSE, "FALSE"},
            {TokenType::RPAREN, ")"},
            {TokenType::SEMICOLON, ";"}};
        TestTokens(input, expected);
    }

    TEST(LexerTest, TestOperators)
    {
        std::string input = ">= <= != < > + - * /";
        std::vector<ExpectedToken> expected = {
            {TokenType::GEQ, ">="},
            {TokenType::LEQ, "<="},
            {TokenType::NEQ, "!="},
            {TokenType::LT, "<"},
            {TokenType::GT, ">"},
            {TokenType::PLUS, "+"},
            {TokenType::MINUS, "-"},
            {TokenType::STAR, "*"},
            {TokenType::SLASH, "/"}};
        TestTokens(input, expected);
    }

} // namespace sql
