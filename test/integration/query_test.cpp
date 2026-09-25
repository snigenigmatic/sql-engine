#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <cstring>

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

    // ── Page-backed storage ───────────────────────────────────────────────────────

    TEST(IntegrationTest, ManyRowsSpanPagesAndStayIndexed)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE big (id INTEGER, payload VARCHAR(100));");
        for (int batch = 0; batch < 30; ++batch)
        {
            std::string sql = "INSERT INTO big VALUES ";
            for (int i = 0; i < 100; ++i)
            {
                const int id = batch * 100 + i;
                if (i > 0)
                    sql += ", ";
                sql += "(" + std::to_string(id) + ", 'payload-for-row-" + std::to_string(id) + "')";
            }
            ASSERT_TRUE(RunSQL(catalog, sql + ";").success);
        }
        EXPECT_EQ(catalog.GetTable("big")->GetTupleCount(), 3000u);

        RunSQL(catalog, "CREATE INDEX idx_big ON big (id);");
        auto point = RunSQL(catalog, "SELECT payload FROM big WHERE id = 2718;");
        ASSERT_TRUE(point.success);
        ASSERT_EQ(point.tuples.size(), 1u);
        EXPECT_EQ(point.tuples[0].GetValue(0).GetAsString(), "payload-for-row-2718");

        auto del = RunSQL(catalog, "DELETE FROM big WHERE id >= 1000;");
        ASSERT_TRUE(del.success);
        auto remaining = RunSQL(catalog, "SELECT * FROM big WHERE id > 990;");
        EXPECT_EQ(remaining.tuples.size(), 9u);
    }

    TEST(IntegrationTest, UpdateGrowingRowsKeepsAllRows)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, s VARCHAR(2000));");
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 0; i < 200; ++i)
        {
            if (i > 0)
                sql += ", ";
            sql += "(" + std::to_string(i) + ", 'x')";
        }
        RunSQL(catalog, sql + ";");

        // Each row grows ~10x, forcing rows to move to new pages
        std::string long_value(1000, 'y');
        auto update = RunSQL(catalog, "UPDATE t SET s = '" + long_value + "' WHERE id < 50;");
        ASSERT_TRUE(update.success);
        EXPECT_EQ(update.message, "50 row(s) updated.");

        auto all = RunSQL(catalog, "SELECT * FROM t;");
        ASSERT_EQ(all.tuples.size(), 200u);
        auto grown = RunSQL(catalog, "SELECT id FROM t WHERE s = '" + long_value + "';");
        EXPECT_EQ(grown.tuples.size(), 50u);
    }

    TEST(IntegrationTest, InsertOversizedRowFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (s VARCHAR(10000));");
        auto result = RunSQL(catalog, "INSERT INTO t VALUES ('" + std::string(5000, 'z') + "');");
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.message.find("Row too large"), std::string::npos);
    }

    TEST(IntegrationTest, FailedUpdateLeavesIndexConsistent)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, s VARCHAR(10000));");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c');");
        RunSQL(catalog, "CREATE INDEX idx_s ON t (s);");

        // Row 1 grows (and may move) and its index entry follows it
        std::string ok_value(600, 'k');
        auto update = RunSQL(catalog, "UPDATE t SET s = '" + ok_value + "' WHERE id = 1;");
        ASSERT_TRUE(update.success) << update.message;

        // A value too large to index is rejected before the row is modified
        auto failing = RunSQL(catalog, "UPDATE t SET s = '" + std::string(2000, 'z') + "' WHERE id >= 2;");
        EXPECT_FALSE(failing.success);
        EXPECT_NE(failing.message.find("Index key too large"), std::string::npos) << failing.message;

        auto via_index = RunSQL(catalog, "SELECT id FROM t WHERE s = '" + ok_value + "';");
        ASSERT_EQ(via_index.tuples.size(), 1u);
        EXPECT_EQ(via_index.tuples[0].GetValue(0).GetAsInt(), 1);
        auto untouched = RunSQL(catalog, "SELECT id FROM t WHERE s = 'b';");
        EXPECT_EQ(untouched.tuples.size(), 1u);
        auto also_untouched = RunSQL(catalog, "SELECT id FROM t WHERE s = 'c';");
        EXPECT_EQ(also_untouched.tuples.size(), 1u);
    }

    TEST(IntegrationTest, DropTableRemovesItsIndexes)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (1), (2);");
        ASSERT_TRUE(RunSQL(catalog, "CREATE INDEX idx_t ON t (id);").success);
        ASSERT_TRUE(RunSQL(catalog, "DROP TABLE t;").success);
        EXPECT_EQ(catalog.GetIndex("t", "id"), nullptr);

        // Recreate: the index name is free again and lookups see only new rows
        RunSQL(catalog, "CREATE TABLE t (id INTEGER);");
        RunSQL(catalog, "INSERT INTO t VALUES (5);");
        ASSERT_TRUE(RunSQL(catalog, "CREATE INDEX idx_t ON t (id);").success);
        auto result = RunSQL(catalog, "SELECT * FROM t WHERE id = 1;");
        ASSERT_TRUE(result.success);
        EXPECT_TRUE(result.tuples.empty());
    }

    // ── Index maintenance when storage fails mid-operation ───────────────────────

    // Overwrite an index page with garbage so the next access to it throws,
    // simulating a storage failure after key validation has passed
    static void CorruptPage(Catalog &catalog, page_id_t page_id)
    {
        PageGuard guard = catalog.GetBufferPool()->FetchPageGuarded(page_id);
        ASSERT_TRUE(guard);
        std::memset(guard.GetData() + 4, 0xFF, PAGE_SIZE - 4);
        guard.MarkDirty();
    }

    static size_t CountRows(Catalog &catalog, const std::string &table)
    {
        return RunSQL(catalog, "SELECT * FROM " + table + ";").tuples.size(); // SeqScan
    }

    TEST(IntegrationTest, InsertIsUndoneWhenAnIndexWriteFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, name VARCHAR(20));");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 'a'), (2, 'b');");
        // Indexes are maintained in name order: the healthy one is written
        // first, so the failure on the second must undo it
        RunSQL(catalog, "CREATE INDEX a_idx_id ON t (id);");
        RunSQL(catalog, "CREATE INDEX z_idx_name ON t (name);");
        CorruptPage(catalog, catalog.GetIndex("t", "name")->GetRootPageId());

        auto result = RunSQL(catalog, "INSERT INTO t VALUES (3, 'c');");
        EXPECT_FALSE(result.success);
        EXPECT_EQ(CountRows(catalog, "t"), 2u);
        EXPECT_TRUE(catalog.GetIndex("t", "id")->Search(Value(3)).empty());
    }

    TEST(IntegrationTest, DeleteKeepsRowAndIndexEntriesWhenAnIndexWriteFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, name VARCHAR(20));");
        RunSQL(catalog, "INSERT INTO t VALUES (1, 'a'), (2, 'b');");
        // Indexes are maintained in name order: the healthy one is written
        // first, so the failure on the second must undo it
        RunSQL(catalog, "CREATE INDEX a_idx_id ON t (id);");
        RunSQL(catalog, "CREATE INDEX z_idx_name ON t (name);");
        CorruptPage(catalog, catalog.GetIndex("t", "name")->GetRootPageId());

        auto result = RunSQL(catalog, "DELETE FROM t WHERE id = 1;");
        EXPECT_FALSE(result.success);
        EXPECT_EQ(CountRows(catalog, "t"), 2u);

        // The healthy index still finds the row
        auto via_index = RunSQL(catalog, "SELECT name FROM t WHERE id = 1;");
        ASSERT_EQ(via_index.tuples.size(), 1u);
        EXPECT_EQ(via_index.tuples[0].GetValue(0).GetAsString(), "a");
    }

    TEST(IntegrationTest, UpdateRestoresRowWhenNewIndexEntryFails)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE t (id INTEGER, pad VARCHAR(100));");
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 0; i < 600; ++i)
            sql += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'row-" + std::to_string(i) + "')";
        ASSERT_TRUE(RunSQL(catalog, sql + ";").success);
        RunSQL(catalog, "CREATE INDEX idx_id ON t (id);");

        // Removing the old key (5) succeeds; inserting the new key lands in
        // a different, corrupted leaf and fails
        BTree *index = catalog.GetIndex("t", "id");
        const page_id_t old_leaf = index->GetLeafPageForKey(Value(5));
        const page_id_t new_leaf = index->GetLeafPageForKey(Value(100000));
        ASSERT_NE(old_leaf, new_leaf);
        CorruptPage(catalog, new_leaf);

        auto result = RunSQL(catalog, "UPDATE t SET id = 100000 WHERE id = 5;");
        EXPECT_FALSE(result.success);
        EXPECT_EQ(CountRows(catalog, "t"), 600u);

        // Row content and its index entry are back
        auto via_index = RunSQL(catalog, "SELECT pad FROM t WHERE id = 5;");
        ASSERT_EQ(via_index.tuples.size(), 1u);
        EXPECT_EQ(via_index.tuples[0].GetValue(0).GetAsString(), "row-5");
        auto rids = index->Search(Value(5));
        ASSERT_EQ(rids.size(), 1u);
        Tuple row;
        ASSERT_TRUE(catalog.GetTable("t")->GetTuple(rids[0], &row));
        EXPECT_EQ(row.GetValue(0).GetAsInt(), 5);
    }

    // ── NULL semantics ────────────────────────────────────────────────────────────

    static std::vector<int> Ids(const ExecutionResult &result)
    {
        std::vector<int> ids;
        for (const auto &t : result.tuples)
            ids.push_back(t.GetValue(0).GetAsInt());
        return ids;
    }

    static void CreatePeople(Catalog &catalog)
    {
        RunSQL(catalog, "CREATE TABLE people (id INTEGER, name VARCHAR(20), age INTEGER);");
        ASSERT_TRUE(RunSQL(catalog, "INSERT INTO people VALUES (1, 'ann', 30), (2, NULL, 40), "
                                    "(3, 'cy', NULL), (4, NULL, NULL);")
                        .success);
    }

    TEST(IntegrationTest, NullValuesAreStoredWithColumnType)
    {
        Catalog catalog;
        CreatePeople(catalog);
        auto rows = RunSQL(catalog, "SELECT * FROM people;");
        ASSERT_EQ(rows.tuples.size(), 4u);
        EXPECT_TRUE(rows.tuples[1].GetValue(1).IsNull());
        EXPECT_EQ(rows.tuples[1].GetValue(1).GetType(), DataType::VARCHAR);
        EXPECT_EQ(rows.tuples[1].GetValue(1).ToString(), "NULL");
    }

    TEST(IntegrationTest, IsNullAndIsNotNull)
    {
        Catalog catalog;
        CreatePeople(catalog);
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE name IS NULL;")), (std::vector<int>{2, 4}));
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE age IS NOT NULL;")), (std::vector<int>{1, 2}));
    }

    TEST(IntegrationTest, ComparisonsWithNullMatchNothing)
    {
        Catalog catalog;
        CreatePeople(catalog);
        EXPECT_TRUE(RunSQL(catalog, "SELECT id FROM people WHERE age = NULL;").tuples.empty());
        EXPECT_TRUE(RunSQL(catalog, "SELECT id FROM people WHERE age <> NULL;").tuples.empty());
        // Rows with a NULL age are neither > 35 nor NOT > 35
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE age > 35;")), (std::vector<int>{2}));
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE NOT age > 35;")), (std::vector<int>{1}));
    }

    TEST(IntegrationTest, ThreeValuedLogicInWhere)
    {
        Catalog catalog;
        CreatePeople(catalog);
        // NULL OR TRUE is TRUE; NULL AND TRUE is unknown
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE age > 35 OR id = 3;")), (std::vector<int>{2, 3}));
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE age < 100 AND id >= 2;")), (std::vector<int>{2}));
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE NOT (name IS NULL OR age IS NULL);")),
                  (std::vector<int>{1}));
    }

    TEST(IntegrationTest, NullInUpdateAndDelete)
    {
        Catalog catalog;
        CreatePeople(catalog);
        ASSERT_TRUE(RunSQL(catalog, "UPDATE people SET age = NULL WHERE id = 1;").success);
        // UPDATE / DELETE with an unknown WHERE touch nothing
        EXPECT_EQ(RunSQL(catalog, "UPDATE people SET name = 'x' WHERE age > 0;").message, "1 row(s) updated.");
        EXPECT_EQ(RunSQL(catalog, "DELETE FROM people WHERE age = NULL;").message, "0 row(s) deleted.");
        EXPECT_EQ(RunSQL(catalog, "DELETE FROM people WHERE age IS NULL;").message, "3 row(s) deleted.");
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people;")), (std::vector<int>{2}));
    }

    TEST(IntegrationTest, NullJoinKeysNeverMatch)
    {
        // Every join algorithm: nested loop, hash join, index nested loop
        for (const char *setup : {"", "HASH", "INDEX"})
        {
            Catalog catalog;
            RunSQL(catalog, "CREATE TABLE a (k INTEGER, tag VARCHAR(5));");
            RunSQL(catalog, "CREATE TABLE b (k INTEGER, tag VARCHAR(5));");
            RunSQL(catalog, "INSERT INTO a VALUES (1, 'a1'), (NULL, 'aN');");
            RunSQL(catalog, "INSERT INTO b VALUES (1, 'b1'), (NULL, 'bN');");
            if (std::string(setup) == "HASH")
            {
                // Enough rows that the optimizer prefers a hash join
                std::string sql = "INSERT INTO b VALUES ";
                for (int i = 100; i < 400; ++i)
                    sql += std::string(i > 100 ? ", " : "") + "(" + std::to_string(i) + ", 'x')";
                RunSQL(catalog, sql + ";");
            }
            if (std::string(setup) == "INDEX")
                RunSQL(catalog, "CREATE INDEX idx_b ON b (k);");

            auto joined = RunSQL(catalog, "SELECT * FROM a JOIN b ON a.k = b.k;");
            ASSERT_TRUE(joined.success) << setup << ": " << joined.message;
            ASSERT_EQ(joined.tuples.size(), 1u) << setup;
            EXPECT_EQ(joined.tuples[0].GetValue(1).GetAsString(), "a1") << setup;
        }
    }

    TEST(IntegrationTest, IndexIgnoresNullsAndNullLiterals)
    {
        Catalog catalog;
        CreatePeople(catalog);
        RunSQL(catalog, "CREATE INDEX idx_age ON people (age);");
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM people WHERE age >= 0;")), (std::vector<int>{1, 2}));
        EXPECT_TRUE(RunSQL(catalog, "SELECT id FROM people WHERE age > NULL;").tuples.empty());
        auto explain = RunSQL(catalog, "EXPLAIN SELECT * FROM people WHERE age = NULL;");
        EXPECT_EQ(explain.message.find("IndexScan"), std::string::npos) << explain.message;
    }

    TEST(IntegrationTest, MixedIntegerAndFloatComparisons)
    {
        Catalog catalog;
        RunSQL(catalog, "CREATE TABLE prices (id INTEGER, price FLOAT);");
        RunSQL(catalog, "INSERT INTO prices VALUES (1, 4.5), (2, 5.0), (3, 7.25);");
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM prices WHERE price > 5;")), (std::vector<int>{3}));
        EXPECT_EQ(Ids(RunSQL(catalog, "SELECT id FROM prices WHERE price = 5;")), (std::vector<int>{2}));
    }

} // namespace sql
