#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

namespace sql
{

    static ExecutionResult RunSQL(Catalog &catalog, const std::string &sql)
    {
        Lexer lexer(sql);
        Parser parser(lexer);
        auto stmt = parser.ParseStatement();
        Executor executor(&catalog);
        return executor.Execute(stmt.get());
    }

    // ── CREATE TABLE ──────────────────────────────────────────────────────────────

    TEST(IntegrationTest, CreateTable)
    {
        Catalog catalog;
        auto result = RunSQL(catalog, "CREATE TABLE t (id INTEGER, name VARCHAR(50));");
        EXPECT_TRUE(result.success);
        EXPECT_NE(catalog.GetTable("t"), nullptr);
    }

    TEST(IntegrationTest, CreateTableDuplicate)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        auto result = RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        EXPECT_FALSE(result.success);
    }

    // ── INSERT ────────────────────────────────────────────────────────────────────

    TEST(IntegrationTest, InsertAndSelectAll)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50), age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice', 25);");
        RunSQL(catalog, "INSERT INTO users VALUES (2, 'Bob', 30);");

        auto result = RunSQL(catalog, "SELECT * FROM users;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 1);
        EXPECT_EQ(result.tuples[1].GetValue(0).GetAsInt(), 2);
    }

    TEST(IntegrationTest, InsertMultipleRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");

        auto result = RunSQL(catalog, "SELECT * FROM t;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);
    }

    TEST(IntegrationTest, InsertColumnCountMismatch)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, name VARCHAR(50));");
        auto result = RunSQL(catalog, "INSERT INTO t VALUES (1, 'Alice', 99);");
        EXPECT_FALSE(result.success);
    }

    TEST(IntegrationTest, InsertIntoMissingTable)
    {
        Catalog catalog;
        auto result = RunSQL(catalog, "INSERT INTO ghost VALUES (1);");
        EXPECT_FALSE(result.success);
    }

    // ── SELECT + WHERE ────────────────────────────────────────────────────────────

    TEST(IntegrationTest, SelectWithWhere)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 20);");
        RunSQL(catalog, "INSERT INTO users VALUES (2, 30);");
        RunSQL(catalog, "INSERT INTO users VALUES (3, 40);");

        auto result = RunSQL(catalog, "SELECT * FROM users WHERE age > 25;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 2);
        EXPECT_EQ(result.tuples[1].GetValue(0).GetAsInt(), 3);
    }

    TEST(IntegrationTest, SelectSpecificColumns)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50), age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice', 25);");

        auto result = RunSQL(catalog, "SELECT name, age FROM users;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.column_names.size(), 2);
        EXPECT_EQ(result.column_names[0], "name");
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsString(), "Alice");
    }

    TEST(IntegrationTest, SelectFromMissingTable)
    {
        Catalog catalog;
        auto result = RunSQL(catalog, "SELECT * FROM ghost;");
        EXPECT_FALSE(result.success);
    }

    // ── UPDATE ────────────────────────────────────────────────────────────────────

    TEST(IntegrationTest, UpdateWithWhere)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 25);");
        RunSQL(catalog, "INSERT INTO users VALUES (2, 30);");

        auto upd = RunSQL(catalog, "UPDATE users SET age = 99 WHERE id = 1;");
        EXPECT_TRUE(upd.success);

        auto result = RunSQL(catalog, "SELECT * FROM users WHERE id = 1;");
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(1).GetAsInt(), 99);

        auto unchanged = RunSQL(catalog, "SELECT * FROM users WHERE id = 2;");
        ASSERT_EQ(unchanged.tuples.size(), 1);
        EXPECT_EQ(unchanged.tuples[0].GetValue(1).GetAsInt(), 30);
    }

    TEST(IntegrationTest, UpdateWithWrongQualifiedColumnFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 25), (2, 30);");

        auto upd = RunSQL(catalog, "UPDATE users SET age = 99 WHERE orders.id = 1;");
        EXPECT_FALSE(upd.success);
    }

    TEST(IntegrationTest, UpdateAllRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 0), (2, 0), (3, 0);");

        auto upd = RunSQL(catalog, "UPDATE t SET val = 1;");
        EXPECT_TRUE(upd.success);

        auto result = RunSQL(catalog, "SELECT * FROM t;");
        for (const auto &tuple : result.tuples)
            EXPECT_EQ(tuple.GetValue(1).GetAsInt(), 1);
    }

    // ── DELETE ────────────────────────────────────────────────────────────────────

    TEST(IntegrationTest, DeleteWithWhere)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 25);");
        RunSQL(catalog, "INSERT INTO users VALUES (2, 30);");
        RunSQL(catalog, "INSERT INTO users VALUES (3, 35);");

        auto del = RunSQL(catalog, "DELETE FROM users WHERE age > 25;");
        EXPECT_TRUE(del.success);

        auto result = RunSQL(catalog, "SELECT * FROM users;");
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 1);
    }

    TEST(IntegrationTest, DeleteWithWrongQualifiedColumnFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, age INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 25), (2, 30);");

        auto del = RunSQL(catalog, "DELETE FROM users WHERE orders.id = 1;");
        EXPECT_FALSE(del.success);
    }

    TEST(IntegrationTest, DeleteAllRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1), (2), (3);");

        auto del = RunSQL(catalog, "DELETE FROM t;");
        EXPECT_TRUE(del.success);

        auto result = RunSQL(catalog, "SELECT * FROM t;");
        EXPECT_EQ(result.tuples.size(), 0);
    }

    // ── CREATE INDEX ──────────────────────────────────────────────────────────────

    TEST(IntegrationTest, CreateIndex)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        auto result = RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        EXPECT_TRUE(result.success);
        EXPECT_NE(catalog.GetIndex("t", "id"), nullptr);
    }

    TEST(IntegrationTest, CreateIndexDuplicateName)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        auto result = RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        EXPECT_FALSE(result.success);
    }

    TEST(IntegrationTest, CreateIndexMissingTable)
    {
        Catalog catalog;
        auto result = RunSQL(catalog, "CREATE INDEX idx_id ON ghost (id);");
        EXPECT_FALSE(result.success);
    }

    TEST(IntegrationTest, CreateIndexMissingColumn)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        auto result = RunSQL(catalog, "CREATE INDEX idx_x ON t (nonexistent);");
        EXPECT_FALSE(result.success);
    }

    // ── INDEX SCAN ────────────────────────────────────────────────────────────────

    TEST(IntegrationTest, IndexPointLookup)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');");
        RunSQL(catalog, "CREATE INDEX idx_id ON users (id);");

        auto result = RunSQL(catalog, "SELECT * FROM users WHERE id = 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 2);
        EXPECT_EQ(result.tuples[0].GetValue(1).GetAsString(), "Bob");
    }

    TEST(IntegrationTest, IndexRangeScanGreaterThan)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");

        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id > 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 3);
        EXPECT_EQ(result.tuples[1].GetValue(0).GetAsInt(), 4);
    }

    TEST(IntegrationTest, IndexRangeScanLessThanOrEqual)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");

        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id <= 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 1);
        EXPECT_EQ(result.tuples[1].GetValue(0).GetAsInt(), 2);
    }

    TEST(IntegrationTest, IndexedPredicateTypeMismatchFallsBackSafely)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");

        // Mismatched literal type should avoid index plan and evaluate safely.
        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id = '2';");
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.tuples.size(), 0);
    }

    TEST(IntegrationTest, InnerJoinBasic)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER, amount FLOAT);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');");
        RunSQL(catalog, "INSERT INTO orders VALUES (101, 1, 100.0), (102, 1, 50.0), (103, 2, 75.0), (104, 4, 200.0);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);
        ASSERT_EQ(result.column_names.size(), 2);
        EXPECT_EQ(result.column_names[0], "id");
        EXPECT_EQ(result.column_names[1], "amount");
    }

    TEST(IntegrationTest, InnerJoinSelectStar)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, 1), (20, 2), (30, 2);");

        auto result = RunSQL(catalog, "SELECT * FROM users JOIN orders ON users.id = orders.user_id;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);
        ASSERT_EQ(result.column_names.size(), 4);
    }

    TEST(IntegrationTest, InnerJoinWithWhereOnRightColumn)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER, amount FLOAT);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');");
        RunSQL(catalog, "INSERT INTO orders VALUES (101, 1, 100.0), (102, 1, 50.0), (103, 2, 75.0), (104, 2, 10.0);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.amount FROM users JOIN orders ON users.id = orders.user_id WHERE orders.amount > 60.0;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
    }

    TEST(IntegrationTest, InnerJoinWithWhereOnLeftColumn)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, 1), (20, 2), (30, 2);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.id FROM users JOIN orders ON users.id = orders.user_id WHERE users.id = 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
    }

    TEST(IntegrationTest, InnerJoinSwappedOnSides)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, 1), (20, 2), (30, 2);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.id FROM users JOIN orders ON orders.user_id = users.id;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);
    }

    TEST(IntegrationTest, InnerJoinAmbiguousProjectionColumnFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, 1);");

        auto result = RunSQL(catalog, "SELECT id FROM users JOIN orders ON users.id = orders.user_id;");
        EXPECT_FALSE(result.success);
    }

    TEST(IntegrationTest, InnerJoinAmbiguousWhereColumnFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER);");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, 1);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.id FROM users JOIN orders ON users.id = orders.user_id WHERE id = 1;");
        EXPECT_FALSE(result.success);
    }

    TEST(IntegrationTest, InnerJoinOnTypeMismatchReturnsNoRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');");
        RunSQL(catalog, "INSERT INTO orders VALUES (10, '1'), (20, '2');");

        auto result = RunSQL(catalog, "SELECT users.id, orders.id FROM users JOIN orders ON users.id = orders.user_id;");
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.tuples.size(), 0);
    }

    TEST(IntegrationTest, HashJoinPathReturnsCorrectRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE users (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, user_id INTEGER, amount FLOAT);");

        // total rows >= 16 to trigger hash-join rule in planner
        RunSQL(catalog, "INSERT INTO users VALUES (1, 'u1'), (2, 'u2'), (3, 'u3'), (4, 'u4'), (5, 'u5'), (6, 'u6'), (7, 'u7'), (8, 'u8');");
        RunSQL(catalog, "INSERT INTO orders VALUES (101, 1, 10.0), (102, 2, 20.0), (103, 2, 30.0), (104, 4, 40.0), (105, 8, 80.0), (106, 9, 90.0), (107, 10, 100.0), (108, 1, 11.0);");

        auto result = RunSQL(catalog, "SELECT users.id, orders.id FROM users JOIN orders ON users.id = orders.user_id WHERE users.id >= 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 4);

        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 2);
        EXPECT_EQ(result.tuples[1].GetValue(0).GetAsInt(), 2);
        EXPECT_EQ(result.tuples[2].GetValue(0).GetAsInt(), 4);
        EXPECT_EQ(result.tuples[3].GetValue(0).GetAsInt(), 8);
    }

    TEST(IntegrationTest, IndexRemainsConsistentAfterInsert)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1), (2);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        RunSQL(catalog, "INSERT INTO t VALUES (3);");

        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id = 3;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), 3);
    }

    TEST(IntegrationTest, IndexRemainsConsistentAfterDelete)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1), (2), (3);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        RunSQL(catalog, "DELETE FROM t WHERE id = 2;");

        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id = 2;");
        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.tuples.size(), 0);

        auto all = RunSQL(catalog, "SELECT * FROM t;");
        ASSERT_EQ(all.tuples.size(), 2);
    }

    TEST(IntegrationTest, IndexRemainsConsistentAfterUpdate)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, val INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        RunSQL(catalog, "UPDATE t SET val = 99 WHERE id = 2;");

        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id = 2;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 1);
        EXPECT_EQ(result.tuples[0].GetValue(1).GetAsInt(), 99);
    }

    // ── EXPLAIN ───────────────────────────────────────────────────────────────────

    TEST(IntegrationTest, ExplainSeqScan)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, name VARCHAR(50));");
        auto result = RunSQL(catalog, "EXPLAIN SELECT * FROM t;");
        EXPECT_TRUE(result.success);
        EXPECT_NE(result.message.find("SeqScan"), std::string::npos);
        EXPECT_NE(result.message.find("Projection"), std::string::npos);
    }

    TEST(IntegrationTest, ExplainIndexScan)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1), (2), (3);");
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");
        auto result = RunSQL(catalog, "EXPLAIN SELECT * FROM t WHERE id = 2;");
        EXPECT_TRUE(result.success);
        EXPECT_NE(result.message.find("IndexScan"), std::string::npos);
    }

    TEST(IntegrationTest, ExplainJoin)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE orders (id INTEGER, cid INTEGER);");
        RunSQL(catalog, "CREATE TABLE customers (id INTEGER, name VARCHAR(50));");
        auto result = RunSQL(catalog, "EXPLAIN SELECT * FROM orders JOIN customers ON orders.cid = customers.id;");
        EXPECT_TRUE(result.success);
        // Without index: should be hash or nested loop join
        EXPECT_TRUE(result.message.find("Join") != std::string::npos);
    }

    // ── INDEX NESTED-LOOP JOIN ────────────────────────────────────────────────────

    TEST(IntegrationTest, IndexNestedLoopJoinBasic)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE orders (oid INTEGER, cid INTEGER);");
        RunSQL(catalog, "CREATE TABLE customers (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO orders VALUES (1, 10), (2, 20), (3, 10);");
        RunSQL(catalog, "INSERT INTO customers VALUES (10, 'Alice'), (20, 'Bob');");
        RunSQL(catalog, "CREATE INDEX idx_cid ON customers (id);");

        auto result = RunSQL(catalog, "SELECT * FROM orders JOIN customers ON orders.cid = customers.id;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);
    }

    TEST(IntegrationTest, IndexNestedLoopJoinExplain)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE orders (oid INTEGER, cid INTEGER);");
        RunSQL(catalog, "CREATE TABLE customers (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO orders VALUES (1, 10);");
        RunSQL(catalog, "INSERT INTO customers VALUES (10, 'Alice');");
        RunSQL(catalog, "CREATE INDEX idx_cid ON customers (id);");

        auto explain = RunSQL(catalog, "EXPLAIN SELECT * FROM orders JOIN customers ON orders.cid = customers.id;");
        EXPECT_TRUE(explain.success);
        EXPECT_NE(explain.message.find("IndexNestedLoopJoin"), std::string::npos);
    }

    TEST(IntegrationTest, IndexNestedLoopJoinSwappedOnColumns)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE orders (oid INTEGER, cid INTEGER);");
        RunSQL(catalog, "CREATE TABLE customers (id INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO orders VALUES (1, 10), (2, 20), (3, 10);");
        RunSQL(catalog, "INSERT INTO customers VALUES (10, 'Alice'), (20, 'Bob');");
        RunSQL(catalog, "CREATE INDEX idx_cid ON customers (id);");

        // ON clause has columns in reverse order: right table column first.
        auto result = RunSQL(catalog, "SELECT * FROM orders JOIN customers ON customers.id = orders.cid;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 3);

        // EXPLAIN should still pick IndexNestedLoopJoin despite swapped ON order.
        auto explain = RunSQL(catalog, "EXPLAIN SELECT * FROM orders JOIN customers ON customers.id = orders.cid;");
        EXPECT_TRUE(explain.success);
        EXPECT_NE(explain.message.find("IndexNestedLoopJoin"), std::string::npos);
    }

    TEST(IntegrationTest, InnerJoinSyntax)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE a (id INTEGER, val INTEGER);");
        RunSQL(catalog, "CREATE TABLE b (aid INTEGER, name VARCHAR(50));");
        RunSQL(catalog, "INSERT INTO a VALUES (1, 100), (2, 200);");
        RunSQL(catalog, "INSERT INTO b VALUES (1, 'x'), (2, 'y');");

        auto result = RunSQL(catalog, "SELECT * FROM a INNER JOIN b ON a.id = b.aid;");
        EXPECT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 2);
    }

} // namespace sql
