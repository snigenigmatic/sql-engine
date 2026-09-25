#include <gtest/gtest.h>
#include "catalog/database.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include <cstdio>
#include <string>
#include <unistd.h>

namespace sql
{

    class CatalogPersistenceTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "catalog_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            std::remove(path_.c_str());
        }
        void TearDown() override { std::remove(path_.c_str()); }

        std::unique_ptr<Database> OpenDb(size_t pool_size = Database::DEFAULT_POOL_SIZE)
        {
            std::string error;
            auto db = Database::Open(path_, &error, pool_size);
            EXPECT_NE(db, nullptr) << error;
            return db;
        }

        static ExecutionResult Run(Database &db, const std::string &sql)
        {
            Lexer lexer(sql);
            Parser parser(lexer);
            auto stmt = parser.ParseStatement();
            Executor executor(&db.GetCatalog());
            return executor.Execute(stmt.get());
        }

        std::string path_;
    };

    TEST_F(CatalogPersistenceTest, FreshFileIsCreated)
    {
        auto db = OpenDb();
        ASSERT_NE(db, nullptr);
        EXPECT_TRUE(db->WasCreated());
        EXPECT_TRUE(db->GetCatalog().GetTableNames().empty());
        db.reset();

        auto reopened = OpenDb();
        ASSERT_NE(reopened, nullptr);
        EXPECT_FALSE(reopened->WasCreated());
    }

    TEST_F(CatalogPersistenceTest, TablesRowsAndIndexesSurviveReopen)
    {
        {
            auto db = OpenDb();
            ASSERT_TRUE(Run(*db, "CREATE TABLE users (id INTEGER, name VARCHAR(50), score FLOAT, active BOOLEAN);").success);
            ASSERT_TRUE(Run(*db, "INSERT INTO users VALUES (1, 'Alice', 9.5, TRUE), (2, 'Bob', 7.25, FALSE), (3, 'Cara', 8.0, TRUE);").success);
            ASSERT_TRUE(Run(*db, "CREATE INDEX idx_users_id ON users (id);").success);
            ASSERT_TRUE(Run(*db, "CREATE TABLE empty_one (x INTEGER);").success);
            ASSERT_TRUE(Run(*db, "UPDATE users SET name = 'Robert' WHERE id = 2;").success);
            ASSERT_TRUE(Run(*db, "DELETE FROM users WHERE id = 3;").success);
        }

        auto db = OpenDb();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(db->GetCatalog().GetTableNames(), (std::vector<std::string>{"empty_one", "users"}));

        Table *users = db->GetCatalog().GetTable("users");
        ASSERT_NE(users, nullptr);
        ASSERT_EQ(users->GetSchema().GetColumnCount(), 4u);
        EXPECT_EQ(users->GetSchema().GetColumn(1).name, "name");
        EXPECT_EQ(users->GetSchema().GetColumn(1).type, DataType::VARCHAR);
        EXPECT_EQ(users->GetSchema().GetColumn(1).length, 50);
        EXPECT_EQ(users->GetTupleCount(), 2u);

        // The index definition is restored and used by the planner
        ASSERT_NE(db->GetCatalog().GetIndex("users", "id"), nullptr);
        auto explain = Run(*db, "EXPLAIN SELECT * FROM users WHERE id = 2;");
        EXPECT_NE(explain.message.find("IndexScan"), std::string::npos) << explain.message;

        auto result = Run(*db, "SELECT name, score FROM users WHERE id = 2;");
        ASSERT_TRUE(result.success);
        ASSERT_EQ(result.tuples.size(), 1u);
        EXPECT_EQ(result.tuples[0].GetValue(0).GetAsString(), "Robert");
        EXPECT_DOUBLE_EQ(result.tuples[0].GetValue(1).GetAsFloat(), 7.25);

        // Index name is still registered
        EXPECT_FALSE(Run(*db, "CREATE INDEX idx_users_id ON users (name);").success);
    }

    TEST_F(CatalogPersistenceTest, DropTableIsPersistedAndFreesPages)
    {
        {
            auto db = OpenDb();
            Run(*db, "CREATE TABLE doomed (id INTEGER, pad VARCHAR(200));");
            std::string sql = "INSERT INTO doomed VALUES ";
            for (int i = 0; i < 300; ++i)
                sql += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", '" + std::string(100, 'p') + "')";
            ASSERT_TRUE(Run(*db, sql + ";").success);
            Run(*db, "CREATE INDEX idx_doomed ON doomed (id);");
            Run(*db, "CREATE TABLE keeper (id INTEGER);");
            Run(*db, "INSERT INTO keeper VALUES (42);");
            ASSERT_TRUE(Run(*db, "DROP TABLE doomed;").success);
        }

        auto db = OpenDb();
        EXPECT_EQ(db->GetCatalog().GetTableNames(), (std::vector<std::string>{"keeper"}));
        EXPECT_EQ(db->GetCatalog().GetIndex("doomed", "id"), nullptr);
        // The dropped table's pages went onto the free list
        {
            Pager probe;
            ASSERT_TRUE(probe.Open(path_));
            EXPECT_NE(probe.GetFreeListHead(), INVALID_PAGE_ID);
        }

        // Index name can be reused after the drop
        Run(*db, "CREATE TABLE doomed (id INTEGER);");
        EXPECT_TRUE(Run(*db, "CREATE INDEX idx_doomed ON doomed (id);").success);
        auto keeper = Run(*db, "SELECT * FROM keeper;");
        ASSERT_EQ(keeper.tuples.size(), 1u);
        EXPECT_EQ(keeper.tuples[0].GetValue(0).GetAsInt(), 42);
    }

    TEST_F(CatalogPersistenceTest, ManyTablesAndSmallBufferPool)
    {
        {
            auto db = OpenDb(8);
            for (int t = 0; t < 50; ++t)
            {
                const std::string name = "t" + std::to_string(t);
                ASSERT_TRUE(Run(*db, "CREATE TABLE " + name + " (id INTEGER, label VARCHAR(20));").success);
                ASSERT_TRUE(Run(*db, "INSERT INTO " + name + " VALUES (" + std::to_string(t) + ", 'row');").success);
            }
        }

        auto db = OpenDb(8);
        ASSERT_EQ(db->GetCatalog().GetTableNames().size(), 50u);
        for (int t = 0; t < 50; ++t)
        {
            auto result = Run(*db, "SELECT id FROM t" + std::to_string(t) + ";");
            ASSERT_TRUE(result.success);
            ASSERT_EQ(result.tuples.size(), 1u);
            EXPECT_EQ(result.tuples[0].GetValue(0).GetAsInt(), t);
        }
    }

    TEST_F(CatalogPersistenceTest, RejectsNonDatabaseFile)
    {
        FILE *f = std::fopen(path_.c_str(), "wb");
        std::fputs("CREATE TABLE this is not a database;", f);
        std::fclose(f);

        std::string error;
        EXPECT_EQ(Database::Open(path_, &error), nullptr);
        EXPECT_FALSE(error.empty());
    }

    TEST_F(CatalogPersistenceTest, ReservedNamesAreRejected)
    {
        auto db = OpenDb();
        auto result = Run(*db, "CREATE TABLE __schema (x INTEGER);");
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.message.find("reserved"), std::string::npos);
        EXPECT_FALSE(db->GetCatalog().TableExists("__schema"));
    }

    TEST_F(CatalogPersistenceTest, InMemoryDatabaseWorks)
    {
        auto db = Database::OpenInMemory();
        ASSERT_TRUE(Run(*db, "CREATE TABLE m (id INTEGER);").success);
        ASSERT_TRUE(Run(*db, "INSERT INTO m VALUES (1), (2);").success);
        EXPECT_EQ(Run(*db, "SELECT * FROM m;").tuples.size(), 2u);
        EXPECT_EQ(db->GetPath(), ":memory:");
    }

} // namespace sql
