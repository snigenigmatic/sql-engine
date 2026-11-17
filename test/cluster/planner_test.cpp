#include <gtest/gtest.h>
#include "cli/local_connection.h"
#include "cluster/node/script.h"
#include "session/session.h"
#include "test_cluster.h"

// D5: how the cluster runs a SELECT (pruning, single-shard, partial
// aggregates, filtered gathers, top-N), checked through EXPLAIN and the
// number of shard reads, and always against the answer of a single node.

namespace sql::cluster
{

    namespace
    {
        std::vector<std::string> Rows(const ExecutionResult &result)
        {
            std::vector<std::string> rows;
            for (const auto &tuple : result.tuples)
            {
                std::string text;
                for (size_t i = 0; i < tuple.GetValueCount(); ++i)
                    text += (i ? "|" : "") + (tuple.GetValue(i).IsNull() ? "NULL" : tuple.GetValue(i).ToString());
                rows.push_back(text);
            }
            return rows;
        }

        class PlannerTest : public ::testing::Test
        {
        protected:
            static void SetUpTestSuite()
            {
                cluster_ = new testing::Cluster();
                ASSERT_TRUE(cluster_->WaitFormed());
                db_ = Database::OpenInMemory().release();
                session_ = new Session(db_);
                local_ = new LocalConnection(db_, session_);
                std::string script =
                    "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR(20), age INTEGER, score FLOAT);"
                    "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount FLOAT) "
                    "SHARD BY HASH (user_id);"
                    "CREATE TABLE countries (code VARCHAR(2) PRIMARY KEY, name VARCHAR(20)) REPLICATED;"
                    "CREATE TABLE visits (user_id INTEGER, country VARCHAR(2)) SHARD BY HASH (user_id);";
                std::string users = "INSERT INTO users VALUES ", orders = "INSERT INTO orders VALUES ",
                            visits = "INSERT INTO visits VALUES ";
                for (int i = 1; i <= 60; ++i)
                {
                    users += (i > 1 ? ", (" : "(") + std::to_string(i) + ", 'u" + std::to_string(i % 13) + "', " +
                             (i % 9 == 0 ? std::string("NULL") : std::to_string(18 + i % 40)) + ", " +
                             std::to_string(i % 7) + ".25)";
                    orders += (i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(1 + i % 20) + ", " +
                              std::to_string(i * 3) + ".5)";
                    visits += (i > 1 ? ", (" : "(") + std::to_string(1 + i % 20) + ", '" +
                              (i % 3 == 0 ? "in" : i % 3 == 1 ? "fr" : "jp") + "')";
                }
                script += users + ";" + orders + ";" + visits + ";" +
                          "INSERT INTO countries VALUES ('in', 'India'), ('fr', 'France'), ('jp', 'Japan');";
                auto client = Client();
                for (const auto &statement : SplitStatements(script))
                {
                    ASSERT_TRUE(client->Execute(statement).success) << statement;
                    ASSERT_TRUE(local_->Execute(statement).success) << statement;
                }
            }

            static void TearDownTestSuite()
            {
                delete local_;
                delete session_;
                delete db_;
                delete cluster_;
            }

            static std::unique_ptr<net::RemoteConnection> Client() { return cluster_->Client(0); }

            // The cluster's answer equals a single node's; returns how many
            // shard reads it took
            static uint64_t Same(const std::string &sql)
            {
                auto client = Client();
                const uint64_t before = cluster_->Leader()->ShardReads();
                const auto a = client->Execute(sql), b = local_->Execute(sql);
                const uint64_t reads = cluster_->Leader()->ShardReads() - before;
                EXPECT_EQ(a.success, b.success) << sql << "\n  " << a.message;
                EXPECT_EQ(a.message, b.message) << sql;
                EXPECT_EQ(a.column_names, b.column_names) << sql;
                EXPECT_EQ(Rows(a), Rows(b)) << sql;
                return reads;
            }

            static std::string Explain(const std::string &sql)
            {
                auto result = Client()->Execute("EXPLAIN " + sql);
                EXPECT_TRUE(result.success) << result.message;
                return result.message;
            }

            static testing::Cluster *cluster_;
            static Database *db_;
            static Session *session_;
            static LocalConnection *local_;
        };

        testing::Cluster *PlannerTest::cluster_ = nullptr;
        Database *PlannerTest::db_ = nullptr;
        Session *PlannerTest::session_ = nullptr;
        LocalConnection *PlannerTest::local_ = nullptr;
    } // namespace

    TEST_F(PlannerTest, AKeyLookupRunsOnOneShard)
    {
        const std::string point = "SELECT id, name, age FROM users WHERE id = 7;";
        EXPECT_NE(Explain(point).find("the whole statement on shard"), std::string::npos) << Explain(point);
        EXPECT_EQ(Same(point), 1u);
        // Joined with a replicated table, or with rows on the same key's shard
        const std::string colocated = "SELECT v.country, c.name FROM visits v JOIN countries c ON c.code = v.country "
                                      "WHERE v.user_id = 3 ORDER BY v.country;";
        EXPECT_NE(Explain(colocated).find("the whole statement on shard"), std::string::npos);
        EXPECT_EQ(Same(colocated), 1u);
        const std::string same_key = "SELECT o.id, v.country FROM orders o JOIN visits v ON v.user_id = o.user_id "
                                     "WHERE o.user_id = 5 AND v.user_id = 5 ORDER BY o.id, v.country;";
        EXPECT_EQ(Same(same_key), 1u);
        // Only replicated tables: any one shard
        EXPECT_EQ(Same("SELECT name FROM countries ORDER BY name;"), 1u);
        // A key no row can have: no shard at all
        EXPECT_EQ(Same("SELECT id FROM users WHERE id = 1 AND id = 2;"), 0u);
        EXPECT_EQ(Same("SELECT id FROM users WHERE id = NULL;"), 0u);
    }

    TEST_F(PlannerTest, KeyListsPrune)
    {
        const std::string some = "SELECT id, name FROM users WHERE id IN (3, 4, 50) ORDER BY id;";
        const uint64_t reads = Same(some);
        EXPECT_GE(reads, 1u);
        EXPECT_LE(reads, 3u); // three keys: at most three of the four shards
        // NOT IN cannot prune: every shard
        EXPECT_EQ(Same("SELECT COUNT(*) FROM users WHERE id NOT IN (3, 4);"), testing::SHARDS);
    }

    TEST_F(PlannerTest, AggregatesWithoutGroupingAreCombinedFromShards)
    {
        const std::string sql = "SELECT COUNT(*), COUNT(age), SUM(age), AVG(age), MIN(name), MAX(score) FROM users "
                                "WHERE age > 25;";
        const std::string plan = Explain(sql);
        EXPECT_NE(plan.find("partial aggregates on shards"), std::string::npos) << plan;
        EXPECT_NE(plan.find("SUM(age), COUNT(age)"), std::string::npos) << plan; // AVG = SUM / COUNT
        EXPECT_EQ(Same(sql), testing::SHARDS);
        Same("SELECT AVG(age), SUM(age), MIN(age), COUNT(*) FROM users WHERE age > 1000;"); // no rows: NULLs
        Same("SELECT SUM(amount), AVG(amount) FROM orders;");
        // ...but not with grouping or DISTINCT: gathered and finished here
        EXPECT_NE(Explain("SELECT age, COUNT(*) FROM users GROUP BY age;").find("rows gathered"), std::string::npos);
        Same("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1 ORDER BY age;");
        Same("SELECT COUNT(DISTINCT name) FROM users;");
    }

    TEST_F(PlannerTest, ConditionsGoToTheShards)
    {
        const std::string sql = "SELECT u.name, o.amount FROM users u JOIN orders o ON o.user_id = u.id "
                                "WHERE u.age > 50 AND o.amount < 40.0 ORDER BY o.amount, u.name;";
        const std::string plan = Explain(sql);
        EXPECT_NE(plan.find("SELECT * FROM users u WHERE (u.age > 50)"), std::string::npos) << plan;
        EXPECT_NE(plan.find("SELECT * FROM orders o WHERE (o.amount < 40.0)"), std::string::npos) << plan;
        Same(sql);
        // Not onto the NULL side of a LEFT JOIN: that would change the answer
        // (pushed down, o.amount IS NULL would keep no order, pad every user
        // with NULLs and return them all)
        const std::string left = "SELECT u.id, o.id FROM users u LEFT JOIN orders o ON o.user_id = u.id "
                                 "WHERE o.amount IS NULL ORDER BY u.id;";
        EXPECT_NE(Explain(left).find("orders from shards"), std::string::npos);
        EXPECT_EQ(Explain(left).find("FROM orders o WHERE"), std::string::npos) << Explain(left);
        Same(left);
        // Nor for a table used twice (self-join) or inside a subquery
        // (pushed down, a.id < 4 would also drop the rows b needs)
        const std::string self = "SELECT a.id, b.id FROM users a JOIN users b ON b.id = a.id + 50 "
                                 "WHERE a.id < 4 ORDER BY a.id;";
        EXPECT_NE(Explain(self).find("users from shards [0,1,2,3]: SELECT * FROM users;"), std::string::npos)
            << Explain(self);
        Same(self);
        Same("SELECT name FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > 150.0) ORDER BY name;");
        // users also inside the subquery: age < 30 must not thin it (11 is
        // in the answer because 12, whose age is 30, is in the subquery's)
        Same("SELECT id FROM users WHERE age < 30 AND id IN (SELECT id - 1 FROM users WHERE age > 28) ORDER BY id;");
        Same("SELECT id FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.user_id = u.id AND o.amount > 170.0) "
             "ORDER BY id;");
    }

    TEST_F(PlannerTest, TopNGoesToTheShards)
    {
        const std::string sql = "SELECT id, name FROM users WHERE age IS NOT NULL ORDER BY age DESC, id LIMIT 4 OFFSET 2;";
        const std::string plan = Explain(sql);
        EXPECT_NE(plan.find("ORDER BY age DESC, id LIMIT 6"), std::string::npos) << plan;
        Same(sql);
        // ORDER BY a position or an alias: not pushed, still right
        Same("SELECT id, age AS years FROM users ORDER BY years, id LIMIT 3;");
        Same("SELECT id, age FROM users ORDER BY 2 DESC, 1 LIMIT 3;");
    }

    TEST_F(PlannerTest, UpdateAndDeleteWithSubqueries)
    {
        auto client = Client();
        for (const std::string sql : {
                 "UPDATE users SET score = (SELECT MAX(amount) FROM orders) WHERE id = 1;",
                 "UPDATE users SET name = 'buyer' WHERE id IN (SELECT user_id FROM orders WHERE amount > 160.0);",
                 "DELETE FROM visits WHERE country NOT IN (SELECT code FROM countries WHERE name <> 'Japan');",
                 "UPDATE users SET age = 0 WHERE EXISTS (SELECT code FROM countries WHERE code = 'zz');",
                 "DELETE FROM orders WHERE user_id IN (SELECT id FROM users WHERE age IS NULL);",
                 "UPDATE users SET age = (SELECT age FROM users WHERE id = 1000) WHERE id = 2;",
                 "UPDATE users SET age = (SELECT age FROM users) WHERE id = 3;", // more than one row
                 // Subqueries with no rows: IN is false, NOT IN true
                 "DELETE FROM visits WHERE country IN (SELECT code FROM countries WHERE code = 'zz');",
                 "UPDATE users SET name = name WHERE id NOT IN (SELECT id FROM users WHERE id > 1000);",
             })
            Same(sql);
        Same("SELECT id, name, age, score FROM users ORDER BY id;");
        Same("SELECT COUNT(*) FROM visits;");
        Same("SELECT COUNT(*) FROM orders;");
        // A subquery that refers to the row being changed cannot run alone
        auto correlated = client->Execute("UPDATE users SET age = 1 WHERE EXISTS (SELECT 1 FROM orders o "
                                          "WHERE o.user_id = users.id);");
        EXPECT_FALSE(correlated.success);
        EXPECT_NE(correlated.message.find("correlated"), std::string::npos) << correlated.message;
    }

} // namespace sql::cluster
