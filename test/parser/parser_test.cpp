#include <gtest/gtest.h>
#include "parser/parser.h"
#include "optimizer/optimizer.h"
#include "catalog/catalog.h"
#include "common/schema.h"

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
        ASSERT_EQ(stmt->GetType(), StatementType::DELETE_STMT);

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
        ASSERT_EQ(stmt->GetType(), StatementType::DELETE_STMT);
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
        ASSERT_EQ(stmt->GetType(), StatementType::UPDATE_STMT);

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
        ASSERT_EQ(stmt->GetType(), StatementType::UPDATE_STMT);
        auto update = static_cast<UpdateStatement *>(stmt.get());

        ASSERT_EQ(update->assignments.size(), 2);
        EXPECT_EQ(update->assignments[0].first, "age");
        EXPECT_EQ(update->assignments[1].first, "name");
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

    TEST(ParserTest, DumpSelectAst)
    {
        std::string sql = "SELECT id, name FROM users WHERE id = 1 OR name = 'Alice';";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        auto dump = DumpStatement(stmt.get());

        EXPECT_NE(dump.find("SelectStatement(table=users"), std::string::npos);
        EXPECT_NE(dump.find("columns=[id, name]"), std::string::npos);
        EXPECT_NE(dump.find("BinaryOp(OR)"), std::string::npos);
        EXPECT_NE(dump.find("BinaryOp(EQ)"), std::string::npos);
        EXPECT_NE(dump.find("Column(id)"), std::string::npos);
        EXPECT_NE(dump.find("Literal(1)"), std::string::npos);
        EXPECT_NE(dump.find("Column(name)"), std::string::npos);
        EXPECT_NE(dump.find("Literal(Alice)"), std::string::npos);
    }

    TEST(ParserTest, DumpUpdateAst)
    {
        std::string sql = "UPDATE users SET age = 30, name = 'Bob' WHERE id = 1;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        auto dump = DumpStatement(stmt.get());

        EXPECT_NE(dump.find("UpdateStatement(table=users)"), std::string::npos);
        EXPECT_NE(dump.find("assignments:"), std::string::npos);
        EXPECT_NE(dump.find("- age ="), std::string::npos);
        EXPECT_NE(dump.find("- name ="), std::string::npos);
        EXPECT_NE(dump.find("Literal(30)"), std::string::npos);
        EXPECT_NE(dump.find("Literal(Bob)"), std::string::npos);
        EXPECT_NE(dump.find("where:"), std::string::npos);
        EXPECT_NE(dump.find("Column(id)"), std::string::npos);
        EXPECT_NE(dump.find("Literal(1)"), std::string::npos);
    }

    TEST(ParserTest, BuildLogicalPlanForSelect)
    {
        std::string sql = "SELECT id, name FROM users WHERE age > 30;";
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        Optimizer optimizer;
        auto plan = optimizer.BuildLogicalPlan(stmt.get());
        auto explain = optimizer.ExplainLogicalPlan(plan.get());

        EXPECT_NE(explain.find("Projection(columns=[id, name])"), std::string::npos);
        EXPECT_NE(explain.find("Filter"), std::string::npos);
        EXPECT_NE(explain.find("BinaryOp(GT)"), std::string::npos);
        EXPECT_NE(explain.find("SeqScan(table=users)"), std::string::npos);
    }

    TEST(ParserTest, BuildLogicalPlanForSelectStarWithoutWhere)
    {
        std::string sql = "SELECT * FROM users;";
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        Optimizer optimizer;
        auto plan = optimizer.BuildLogicalPlan(stmt.get());
        auto explain = optimizer.ExplainLogicalPlan(plan.get());

        EXPECT_NE(explain.find("Projection(columns=*)"), std::string::npos);
        EXPECT_EQ(explain.find("Filter"), std::string::npos);
        EXPECT_NE(explain.find("SeqScan(table=users)"), std::string::npos);
    }

    TEST(ParserTest, BuildPhysicalPlanUsesIndexWhenAvailable)
    {
        Catalog catalog;
        ASSERT_TRUE(catalog.CreateTable("users", Schema({
                                                 Column("id", DataType::INTEGER),
                                                 Column("name", DataType::VARCHAR, 50),
                                             })));
        ASSERT_TRUE(catalog.CreateIndex("idx_users_id", "users", "id"));

        std::string sql = "SELECT * FROM users WHERE id = 2;";
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        Optimizer optimizer;
        auto plan = optimizer.BuildPhysicalPlan(stmt.get(), &catalog);
        auto explain = optimizer.ExplainPhysicalPlan(plan.get());

        EXPECT_NE(explain.find("Projection(columns=*)"), std::string::npos);
        EXPECT_NE(explain.find("Filter"), std::string::npos);
        EXPECT_NE(explain.find("IndexScan(table=users, column=id, point=2)"), std::string::npos);
        EXPECT_EQ(explain.find("SeqScan(table=users)"), std::string::npos);
    }

    TEST(ParserTest, BuildPhysicalPlanFallsBackToSeqScanWithoutIndex)
    {
        Catalog catalog;
        ASSERT_TRUE(catalog.CreateTable("users", Schema({
                                                 Column("id", DataType::INTEGER),
                                                 Column("age", DataType::INTEGER),
                                             })));

        std::string sql = "SELECT id FROM users WHERE age > 30;";
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        Optimizer optimizer;
        auto plan = optimizer.BuildPhysicalPlan(stmt.get(), &catalog);
        auto explain = optimizer.ExplainPhysicalPlan(plan.get());

        EXPECT_NE(explain.find("Projection(columns=[id])"), std::string::npos);
        EXPECT_NE(explain.find("Filter"), std::string::npos);
        EXPECT_NE(explain.find("SeqScan(table=users)"), std::string::npos);
        EXPECT_EQ(explain.find("IndexScan("), std::string::npos);
    }

    TEST(ParserTest, BuildPhysicalPlanFallsBackToSeqScanOnTypeMismatch)
    {
        Catalog catalog;
        ASSERT_TRUE(catalog.CreateTable("users", Schema({
                                                 Column("id", DataType::INTEGER),
                                                 Column("name", DataType::VARCHAR, 50),
                                             })));
        ASSERT_TRUE(catalog.CreateIndex("idx_users_id", "users", "id"));

        std::string sql = "SELECT * FROM users WHERE id = '2';";
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();

        Optimizer optimizer;
        auto plan = optimizer.BuildPhysicalPlan(stmt.get(), &catalog);
        auto explain = optimizer.ExplainPhysicalPlan(plan.get());

        EXPECT_NE(explain.find("Projection(columns=*)"), std::string::npos);
        EXPECT_NE(explain.find("Filter"), std::string::npos);
        EXPECT_NE(explain.find("SeqScan(table=users)"), std::string::npos);
        EXPECT_EQ(explain.find("IndexScan("), std::string::npos);
    }

    TEST(ParserTest, ParseSelectWithJoin)
    {
        std::string sql = "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id;";
        Lexer lexer(sql);
        Parser parser(lexer);

        auto stmt = parser.ParseStatement();
        ASSERT_NE(stmt, nullptr);
        ASSERT_EQ(stmt->GetType(), StatementType::SELECT);
        auto select = static_cast<SelectStatement *>(stmt.get());

        EXPECT_EQ(select->table, "users");
        ASSERT_TRUE(select->join_table.has_value());
        EXPECT_EQ(*select->join_table, "orders");
        ASSERT_TRUE(select->join_left_column.has_value());
        ASSERT_TRUE(select->join_right_column.has_value());
        EXPECT_EQ(*select->join_left_column, "users.id");
        EXPECT_EQ(*select->join_right_column, "orders.user_id");
        ASSERT_EQ(select->columns.size(), 2);
        EXPECT_EQ(select->columns[0], "users.id");
        EXPECT_EQ(select->columns[1], "orders.amount");
    }

} // namespace sql
