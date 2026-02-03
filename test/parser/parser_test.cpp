#include <gtest/gtest.h>
#include "parser/parser.h"

namespace sql {

TEST(ParserTest, ParseSelectStar) {
    std::string sql = "SELECT * FROM users;";
    Lexer lexer(sql);
    Parser parser(lexer);
    
    auto stmt = parser.ParseStatement();
    ASSERT_NE(stmt, nullptr);
    ASSERT_EQ(stmt->GetType(), StatementType::SELECT);
    
    auto select = static_cast<SelectStatement*>(stmt.get());
    EXPECT_EQ(select->table, "users");
    EXPECT_TRUE(select->select_star);
    EXPECT_TRUE(select->columns.empty());
    EXPECT_EQ(select->where, nullptr);
}

TEST(ParserTest, ParseSelectColumns) {
    std::string sql = "SELECT id, name, age FROM users;";
    Lexer lexer(sql);
    Parser parser(lexer);
    
    auto stmt = parser.ParseStatement();
    auto select = static_cast<SelectStatement*>(stmt.get());
    
    EXPECT_EQ(select->table, "users");
    EXPECT_FALSE(select->select_star);
    ASSERT_EQ(select->columns.size(), 3);
    EXPECT_EQ(select->columns[0], "id");
    EXPECT_EQ(select->columns[1], "name");
    EXPECT_EQ(select->columns[2], "age");
}

TEST(ParserTest, ParseWhereClause) {
    std::string sql = "SELECT * FROM users WHERE id = 1;";
    Lexer lexer(sql);
    Parser parser(lexer);
    
    auto stmt = parser.ParseStatement();
    auto select = static_cast<SelectStatement*>(stmt.get());
    
    ASSERT_NE(select->where, nullptr);
    EXPECT_EQ(select->where->GetType(), ExpressionType::BINARY_OP);
    
    auto binOp = static_cast<BinaryExpression*>(select->where.get());
    EXPECT_EQ(binOp->op, TokenType::EQ);
    EXPECT_EQ(binOp->left->GetType(), ExpressionType::COLUMN_REF);
    EXPECT_EQ(binOp->right->GetType(), ExpressionType::LITERAL);
}

TEST(ParserTest, ParseComplexWhere) {
    std::string sql = "SELECT * FROM t WHERE a > 5 AND (b = 'test' OR c <= 10);";
    Lexer lexer(sql);
    Parser parser(lexer);
    
    auto stmt = parser.ParseStatement();
    auto select = static_cast<SelectStatement*>(stmt.get());
    
    // Top level should be AND
    auto top = static_cast<BinaryExpression*>(select->where.get());
    EXPECT_EQ(top->op, TokenType::AND);
    
    // Left: a > 5
    auto left = static_cast<BinaryExpression*>(top->left.get());
    EXPECT_EQ(left->op, TokenType::GT);
    
    // Right: OR expression
    auto right = static_cast<BinaryExpression*>(top->right.get());
    EXPECT_EQ(right->op, TokenType::OR);
}

} // namespace sql
