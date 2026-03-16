#include <gtest/gtest.h>
#include "parser/parser.h"

namespace sql
{

    TEST(ParserTest, ParseSelectStar)
    {
        std::string sql = "SELECT * FROM users;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::SELECT);

        auto select = static_cast<SelectStatement *>(stmt.get());
        EXPECT_EQ(select->table, "users");
        EXPECT_TRUE(select->select_star);
        EXPECT_TRUE(select->columns.empty());
        EXPECT_EQ(select->where, nullptr);
    }

    TEST(ParserTest, ParseSelectColumns)
    {
        std::string sql = "SELECT id, name, age FROM users;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        auto select = static_cast<SelectStatement *>(stmt.get());

        EXPECT_EQ(select->table, "users");
        EXPECT_FALSE(select->select_star);
        ASSERT_EQ(select->columns.size(), 3);
        EXPECT_EQ(select->columns[0], "id");
        EXPECT_EQ(select->columns[1], "name");
        EXPECT_EQ(select->columns[2], "age");
    }

    TEST(ParserTest, ParseWhereClause)
    {
        std::string sql = "SELECT * FROM users WHERE id = 1;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        auto select = static_cast<SelectStatement *>(stmt.get());

        ASSERT_NE(select->where, nullptr);
        EXPECT_EQ(select->where->GetType(), ExpressionType::BINARY_OP);

        auto binOp = static_cast<BinaryExpression *>(select->where.get());
        EXPECT_EQ(binOp->op, TokenType::EQ);
        EXPECT_EQ(binOp->left->GetType(), ExpressionType::COLUMN_REF);
        EXPECT_EQ(binOp->right->GetType(), ExpressionType::LITERAL);
    }

    TEST(ParserTest, ParseComplexWhere)
    {
        std::string sql = "SELECT * FROM t WHERE a > 5 AND (b = 'test' OR c <= 10);";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        auto select = static_cast<SelectStatement *>(stmt.get());

        // Top level should be AND
        auto top = static_cast<BinaryExpression *>(select->where.get());
        EXPECT_EQ(top->op, TokenType::AND);

        // Left: a > 5
        auto left = static_cast<BinaryExpression *>(top->left.get());
        EXPECT_EQ(left->op, TokenType::GT);

        // Right: OR expression
        auto right = static_cast<BinaryExpression *>(top->right.get());
        EXPECT_EQ(right->op, TokenType::OR);
    }

    TEST(ParserTest, ParseCreateTable)
    {
        std::string sql = "CREATE TABLE employees (id INTEGER, name VARCHAR(100), salary FLOAT, active BOOLEAN);";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::CREATE_TABLE);

        auto create = static_cast<CreateTableStatement *>(stmt.get());
        EXPECT_EQ(create->table, "employees");
        ASSERT_EQ(create->columns.size(), 4);
        EXPECT_EQ(create->columns[0].name, "id");
        EXPECT_EQ(create->columns[0].type_token, TokenType::INTEGER);
        EXPECT_EQ(create->columns[1].name, "name");
        EXPECT_EQ(create->columns[1].type_token, TokenType::VARCHAR);
        EXPECT_EQ(create->columns[1].length, 100);
        EXPECT_EQ(create->columns[2].name, "salary");
        EXPECT_EQ(create->columns[2].type_token, TokenType::FLOAT);
        EXPECT_EQ(create->columns[3].name, "active");
        EXPECT_EQ(create->columns[3].type_token, TokenType::BOOLEAN);
    }

    TEST(ParserTest, ParseInsert)
    {
        std::string sql = "INSERT INTO users VALUES (1, 'Alice', 25);";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::INSERT);

        auto insert = static_cast<InsertStatement *>(stmt.get());
        EXPECT_EQ(insert->table, "users");
        ASSERT_EQ(insert->rows.size(), 1);
        ASSERT_EQ(insert->rows[0].size(), 3);
    }

    TEST(ParserTest, ParseInsertMultipleRows)
    {
        std::string sql = "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::INSERT);
        auto insert = static_cast<InsertStatement *>(stmt.get());

        ASSERT_EQ(insert->rows.size(), 2);
        ASSERT_EQ(insert->rows[0].size(), 2);
        ASSERT_EQ(insert->rows[1].size(), 2);
    }

    TEST(ParserTest, ParseDelete)
    {
        std::string sql = "DELETE FROM users;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::DELETE);

        auto del = static_cast<DeleteStatement *>(stmt.get());
        EXPECT_EQ(del->table, "users");
        EXPECT_EQ(del->where, nullptr);
    }

    TEST(ParserTest, ParseDeleteWhere)
    {
        std::string sql = "DELETE FROM users WHERE id = 5;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::DELETE);
        auto del = static_cast<DeleteStatement *>(stmt.get());

        EXPECT_EQ(del->table, "users");
        ASSERT_NE(del->where, nullptr);
        auto cond = static_cast<BinaryExpression *>(del->where.get());
        EXPECT_EQ(cond->op, TokenType::EQ);
    }

    TEST(ParserTest, ParseUpdate)
    {
        std::string sql = "UPDATE users SET age = 30 WHERE id = 1;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::UPDATE);

        auto update = static_cast<UpdateStatement *>(stmt.get());
        EXPECT_EQ(update->table, "users");
        ASSERT_EQ(update->assignments.size(), 1);
        EXPECT_EQ(update->assignments[0].first, "age");
        ASSERT_NE(update->where, nullptr);
    }

    TEST(ParserTest, ParseUpdateMultipleAssignments)
    {
        std::string sql = "UPDATE users SET age = 30, name = 'Bob' WHERE id = 1;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::UPDATE);
        auto update = static_cast<UpdateStatement *>(stmt.get());

        ASSERT_EQ(update->assignments.size(), 2);
        EXPECT_EQ(update->assignments[0].first, "age");
        EXPECT_EQ(update->assignments[1].first, "name");
    }

    TEST(ParserTest, ParseDropTable)
    {
        std::string sql = "DROP TABLE users;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::DROP_TABLE);

        auto drop = static_cast<DropTableStatement *>(stmt.get());
        EXPECT_EQ(drop->table, "users");
    }

    TEST(ParserTest, ParseNegativeLiterals)
    {
        std::string sql = "SELECT * FROM t WHERE age > -25 AND balance >= -1.5;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::SELECT);

        auto select = static_cast<SelectStatement *>(stmt.get());
        ASSERT_NE(select->where, nullptr);

        // Top level AND
        auto top = static_cast<BinaryExpression *>(select->where.get());
        EXPECT_EQ(top->op, TokenType::AND);

        // Left side: age > -25
        auto left = static_cast<BinaryExpression *>(top->left.get());
        EXPECT_EQ(left->op, TokenType::GT);
        EXPECT_EQ(left->right->GetType(), ExpressionType::LITERAL);

        // Right side: balance >= -1.5
        auto right = static_cast<BinaryExpression *>(top->right.get());
        EXPECT_EQ(right->op, TokenType::GEQ);
        EXPECT_EQ(right->right->GetType(), ExpressionType::LITERAL);
    }

} // namespace sql
