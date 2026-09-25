#include <gtest/gtest.h>
#include "catalog/database.h"
#include "catalog/legacy_import.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "storage/table_heap.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
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

    TEST_F(CatalogPersistenceTest, IndexIsStoredOnDiskAndMaintainedAcrossReopen)
    {
        {
            auto db = OpenDb();
            Run(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));");
            std::string sql = "INSERT INTO t VALUES ";
            for (int i = 0; i < 2000; ++i)
                sql += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'n" + std::to_string(i) + "')";
            ASSERT_TRUE(Run(*db, sql + ";").success);
            ASSERT_TRUE(Run(*db, "CREATE INDEX idx_t_id ON t (id);").success);
        }
        {
            auto db = OpenDb();
            BTree *index = db->GetCatalog().GetIndex("t", "id");
            ASSERT_NE(index, nullptr);
            EXPECT_GE(index->GetHeight(), 2); // loaded from pages, not rebuilt
            EXPECT_EQ(index->Search(Value(1999)).size(), 1u);

            // Keep modifying through the reopened index
            ASSERT_TRUE(Run(*db, "INSERT INTO t VALUES (5000, 'new');").success);
            ASSERT_TRUE(Run(*db, "DELETE FROM t WHERE id < 1000;").success);
            ASSERT_TRUE(Run(*db, "UPDATE t SET id = 7000 WHERE id = 1500;").success);
        }
        auto db = OpenDb();
        EXPECT_EQ(Run(*db, "SELECT * FROM t WHERE id = 5000;").tuples.size(), 1u);
        EXPECT_EQ(Run(*db, "SELECT * FROM t WHERE id = 10;").tuples.size(), 0u);
        EXPECT_EQ(Run(*db, "SELECT * FROM t WHERE id = 1500;").tuples.size(), 0u);
        auto moved = Run(*db, "SELECT name FROM t WHERE id = 7000;");
        ASSERT_EQ(moved.tuples.size(), 1u);
        EXPECT_EQ(moved.tuples[0].GetValue(0).GetAsString(), "n1500");
        EXPECT_EQ(Run(*db, "SELECT * FROM t WHERE id >= 1000;").tuples.size(), 1001u);
        EXPECT_EQ(db->GetCatalog().GetIndex("t", "id")->GetAllEntries().size(), 1001u);
    }

    TEST_F(CatalogPersistenceTest, IndexWithoutStoredTreeIsBuiltOnOpen)
    {
        // Databases written before indexes were stored on disk record
        // root_page = -1 for indexes; opening one builds and records the tree
        {
            auto db = OpenDb();
            Run(*db, "CREATE TABLE t (id INTEGER);");
            Run(*db, "INSERT INTO t VALUES (1), (2), (3);");
        }
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            BufferPoolManager bpm(8, &pager);
            TableHeap schema(&bpm, pager.GetCatalogRoot());
            schema.InsertTuple(Tuple({Value("index"), Value("old_idx"), Value("t"),
                                      Value(static_cast<int32_t>(INVALID_PAGE_ID)), Value("id")}));
        }
        {
            auto db = OpenDb();
            ASSERT_NE(db->GetCatalog().GetIndex("t", "id"), nullptr);
            EXPECT_EQ(db->GetCatalog().GetIndex("t", "id")->Search(Value(2)).size(), 1u);
        }
        auto db = OpenDb();
        EXPECT_EQ(db->GetCatalog().GetIndex("t", "id")->Search(Value(3)).size(), 1u);
        auto explain = Run(*db, "EXPLAIN SELECT * FROM t WHERE id = 3;");
        EXPECT_NE(explain.message.find("IndexScan"), std::string::npos);
    }

    TEST_F(CatalogPersistenceTest, CreateIndexFailsCleanlyOnUnindexableValue)
    {
        auto db = OpenDb();
        Run(*db, "CREATE TABLE t (s VARCHAR(3000));");
        Run(*db, "INSERT INTO t VALUES ('short'), ('" + std::string(2000, 'x') + "');");
        auto result = Run(*db, "CREATE INDEX idx_s ON t (s);");
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.message.find("Index key too large"), std::string::npos) << result.message;
        EXPECT_EQ(db->GetCatalog().GetIndex("t", "s"), nullptr);
        // Name is not taken by the failed attempt
        Run(*db, "DELETE FROM t WHERE s = '" + std::string(2000, 'x') + "';");
        EXPECT_TRUE(Run(*db, "CREATE INDEX idx_s ON t (s);").success);
    }

    // ── Corrupt files ─────────────────────────────────────────────────────────────

    TEST_F(CatalogPersistenceTest, FileWithPagesButNoSchemaIsRejected)
    {
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            pager.AllocatePage();
            pager.AllocatePage();
        }
        std::string error;
        EXPECT_EQ(Database::Open(path_, &error), nullptr);
        EXPECT_NE(error.find("no schema table"), std::string::npos) << error;

        // The file was not modified into a "new" database
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        EXPECT_EQ(pager.GetCatalogRoot(), INVALID_PAGE_ID);
        EXPECT_EQ(pager.GetPageCount(), 3u);
    }

    TEST_F(CatalogPersistenceTest, TableWithInvalidRootPageIsRejected)
    {
        for (int32_t bad_root : {INVALID_PAGE_ID, 0, 999})
        {
            std::remove(path_.c_str());
            {
                auto db = OpenDb();
                Run(*db, "CREATE TABLE ok (id INTEGER);");
            }
            {
                Pager pager;
                ASSERT_TRUE(pager.Open(path_));
                BufferPoolManager bpm(8, &pager);
                TableHeap schema(&bpm, pager.GetCatalogRoot());
                schema.InsertTuple(Tuple({Value("table"), Value("bogus"), Value("bogus"),
                                          Value(bad_root), Value("id:0:0")}));
            }
            std::string error;
            EXPECT_EQ(Database::Open(path_, &error), nullptr) << bad_root;
            EXPECT_NE(error.find("invalid root page"), std::string::npos) << error;
        }
    }

    TEST_F(CatalogPersistenceTest, IndexWithInvalidRootPageIsRejected)
    {
        for (int32_t bad_root : {0, 999})
        {
            std::remove(path_.c_str());
            {
                auto db = OpenDb();
                Run(*db, "CREATE TABLE t (id INTEGER);");
            }
            {
                Pager pager;
                ASSERT_TRUE(pager.Open(path_));
                BufferPoolManager bpm(8, &pager);
                TableHeap schema(&bpm, pager.GetCatalogRoot());
                schema.InsertTuple(Tuple({Value("index"), Value("bad_idx"), Value("t"),
                                          Value(bad_root), Value("id")}));
            }
            std::string error;
            EXPECT_EQ(Database::Open(path_, &error), nullptr) << bad_root;
            EXPECT_NE(error.find("invalid root page"), std::string::npos) << error;
        }
    }

    // ── Legacy snapshot migration ─────────────────────────────────────────────────

    class LegacyMigrationTest : public CatalogPersistenceTest
    {
    protected:
        void SetUp() override
        {
            CatalogPersistenceTest::SetUp();
            dir_ = path_ + ".snapshot";
            std::filesystem::remove_all(dir_);
            std::filesystem::create_directories(dir_);
            std::remove((path_ + ".importing").c_str());
        }
        void TearDown() override
        {
            std::filesystem::remove_all(dir_);
            std::remove((path_ + ".importing").c_str());
            CatalogPersistenceTest::TearDown();
        }

        void Write(const std::string &name, const std::string &content)
        {
            std::ofstream(dir_ + "/" + name) << content;
        }

        static bool Exists(const std::string &path)
        {
            struct stat st;
            return stat(path.c_str(), &st) == 0;
        }

        std::string dir_;
    };

    TEST_F(LegacyMigrationTest, ImportsEveryTable)
    {
        Write("catalog.meta", "2\npeople\nflags\n");
        Write("people.tbl", "2\nid 0 0\nname 2 50\n3\n0 1\n2 11:Alice Smith\n0 2\n2 NULL\n0 3\n2 3:Bob\n");
        Write("flags.tbl", "1\non 3 0\n2\n3 true\n3 false\n");

        size_t count = 0;
        std::string error;
        ASSERT_TRUE(MigrateLegacySnapshot(dir_, path_, &count, &error)) << error;
        EXPECT_EQ(count, 2u);
        EXPECT_FALSE(Exists(path_ + ".importing"));

        auto db = OpenDb();
        EXPECT_EQ(db->GetCatalog().GetTableNames(), (std::vector<std::string>{"flags", "people"}));
        auto people = Run(*db, "SELECT name FROM people WHERE id = 1;");
        ASSERT_EQ(people.tuples.size(), 1u);
        EXPECT_EQ(people.tuples[0].GetValue(0).GetAsString(), "Alice Smith");
        EXPECT_TRUE(Run(*db, "SELECT name FROM people WHERE id = 2;").tuples[0].GetValue(0).IsNull());
        EXPECT_EQ(Run(*db, "SELECT * FROM flags;").tuples.size(), 2u);
    }

    TEST_F(LegacyMigrationTest, MissingTableFileCreatesNothing)
    {
        Write("catalog.meta", "2\npeople\nmissing\n");
        Write("people.tbl", "1\nid 0 0\n1\n0 1\n");

        std::string error;
        EXPECT_FALSE(MigrateLegacySnapshot(dir_, path_, nullptr, &error));
        EXPECT_NE(error.find("missing.tbl"), std::string::npos) << error;
        EXPECT_FALSE(Exists(path_));
        EXPECT_FALSE(Exists(path_ + ".importing"));
    }

    TEST_F(LegacyMigrationTest, MalformedDataCreatesNothing)
    {
        const std::vector<std::string> bad_tables = {
            "1\nflag 3 0\n1\n3 maybe\n",   // bad boolean
            "1\nid 0 0\n3\n0 1\n0 2\n",   // fewer rows than declared
            "1\nid 0 0\n1\n0 notanumber\n", // bad integer
            "1\nname 2 50\n1\n2 20:too short\n", // string ends early
        };
        for (const auto &table : bad_tables)
        {
            Write("catalog.meta", "1\nt\n");
            Write("t.tbl", table);
            std::string error;
            EXPECT_FALSE(MigrateLegacySnapshot(dir_, path_, nullptr, &error)) << table;
            EXPECT_FALSE(error.empty());
            EXPECT_FALSE(Exists(path_)) << table;
            EXPECT_FALSE(Exists(path_ + ".importing")) << table;
        }
    }

    TEST_F(LegacyMigrationTest, ReplacesLeftoverFromInterruptedImport)
    {
        std::ofstream(path_ + ".importing") << "half-written garbage";
        Write("catalog.meta", "1\nt\n");
        Write("t.tbl", "1\nid 0 0\n1\n0 42\n");

        std::string error;
        ASSERT_TRUE(MigrateLegacySnapshot(dir_, path_, nullptr, &error)) << error;
        auto db = OpenDb();
        EXPECT_EQ(Run(*db, "SELECT * FROM t;").tuples.size(), 1u);
    }

} // namespace sql
