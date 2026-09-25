#include <gtest/gtest.h>
#include "catalog/database.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "storage/buffer_pool.h"
#include "storage/pager.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace sql
{

    class WalTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "wal_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            Cleanup();
        }
        void TearDown() override { Cleanup(); }
        void Cleanup()
        {
            std::remove(path_.c_str());
            std::remove((path_ + "-wal").c_str());
        }

        static bool Exists(const std::string &path)
        {
            struct stat st;
            return stat(path.c_str(), &st) == 0;
        }

        static std::array<char, PAGE_SIZE> Filled(char c)
        {
            std::array<char, PAGE_SIZE> buf;
            buf.fill(c);
            return buf;
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

    // ── Pager ─────────────────────────────────────────────────────────────────────

    TEST_F(WalTest, RollbackRestoresLastCommit)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_)) << pager.GetLastError();
        page_id_t a = pager.AllocatePage();
        ASSERT_TRUE(pager.WritePage(a, Filled('A').data()));
        ASSERT_TRUE(pager.SetCatalogRoot(a));
        ASSERT_TRUE(pager.Commit());
        EXPECT_FALSE(pager.HasUncommittedChanges());

        // Change the page, allocate and free pages, move the catalog root...
        ASSERT_TRUE(pager.WritePage(a, Filled('B').data()));
        page_id_t b = pager.AllocatePage();
        page_id_t c = pager.AllocatePage();
        ASSERT_TRUE(pager.DeallocatePage(b));
        ASSERT_TRUE(pager.SetCatalogRoot(c));
        EXPECT_TRUE(pager.HasUncommittedChanges());

        // ...then roll all of it back
        ASSERT_TRUE(pager.Rollback());
        EXPECT_EQ(pager.GetPageCount(), 2u);
        EXPECT_EQ(pager.GetFreeListHead(), INVALID_PAGE_ID);
        EXPECT_EQ(pager.GetCatalogRoot(), a);
        std::array<char, PAGE_SIZE> buf{};
        ASSERT_TRUE(pager.ReadPage(a, buf.data()));
        EXPECT_EQ(buf, Filled('A'));
        EXPECT_FALSE(pager.HasUncommittedChanges());
    }

    TEST_F(WalTest, AllocatedPagesReadAsZeros)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        page_id_t a = pager.AllocatePage();
        pager.WritePage(a, Filled('x').data());
        pager.Commit();
        pager.DeallocatePage(a);
        page_id_t reused = pager.AllocatePage();
        ASSERT_EQ(reused, a);
        std::array<char, PAGE_SIZE> buf{};
        ASSERT_TRUE(pager.ReadPage(reused, buf.data()));
        EXPECT_EQ(buf, Filled(0));
    }

    TEST_F(WalTest, LogIsUsedDuringSessionAndRemovedOnClose)
    {
        {
            Pager pager;
            ASSERT_TRUE(pager.Open(path_));
            EXPECT_TRUE(Exists(path_ + "-wal"));
            page_id_t a = pager.AllocatePage();
            pager.WritePage(a, Filled('q').data());
            ASSERT_TRUE(pager.Commit());
            EXPECT_GT(pager.GetWalFrameCount(), 0u);

            // The database file is untouched until a checkpoint
            struct stat st;
            ASSERT_EQ(stat(path_.c_str(), &st), 0);
            EXPECT_EQ(st.st_size, static_cast<off_t>(PAGE_SIZE));
        }
        EXPECT_FALSE(Exists(path_ + "-wal"));

        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        std::array<char, PAGE_SIZE> buf{};
        ASSERT_TRUE(pager.ReadPage(1, buf.data()));
        EXPECT_EQ(buf, Filled('q'));
    }

    TEST_F(WalTest, CheckpointsWhenLogGrows)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        pager.SetCheckpointThreshold(8);
        page_id_t a = pager.AllocatePage();
        for (int i = 0; i < 50; ++i)
        {
            pager.WritePage(a, Filled(static_cast<char>('a' + i % 26)).data());
            ASSERT_TRUE(pager.Commit());
            EXPECT_LT(pager.GetWalFrameCount(), 8u);
        }
        std::array<char, PAGE_SIZE> buf{};
        ASSERT_TRUE(pager.ReadPage(a, buf.data()));
        EXPECT_EQ(buf, Filled(static_cast<char>('a' + 49 % 26)));
    }

    TEST_F(WalTest, CheckpointRefusedWithUncommittedChanges)
    {
        Pager pager;
        ASSERT_TRUE(pager.Open(path_));
        page_id_t a = pager.AllocatePage();
        pager.WritePage(a, Filled('z').data());
        EXPECT_FALSE(pager.Checkpoint());
        ASSERT_TRUE(pager.Commit());
        EXPECT_TRUE(pager.Checkpoint());
        EXPECT_EQ(pager.GetWalFrameCount(), 0u);
    }

    TEST_F(WalTest, InMemoryPagerCannotRollBack)
    {
        Pager pager;
        pager.OpenInMemory();
        EXPECT_TRUE(pager.Commit());
        EXPECT_FALSE(pager.Rollback());
    }

    // ── Database ──────────────────────────────────────────────────────────────────

    TEST_F(WalTest, DatabaseRollbackRestoresTablesRowsAndIndexes)
    {
        std::string error;
        auto db = Database::Open(path_, &error);
        ASSERT_NE(db, nullptr) << error;
        ASSERT_TRUE(Run(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));").success);
        ASSERT_TRUE(Run(*db, "INSERT INTO t VALUES (1, 'one'), (2, 'two');").success);
        ASSERT_TRUE(db->Commit());
        const uint32_t pages = db->GetPager().GetPageCount();

        // A pile of uncommitted work, big enough to spill out of the cache
        ASSERT_TRUE(Run(*db, "CREATE TABLE u (x INTEGER);").success);
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = 3; i < 2000; ++i)
            sql += std::string(i > 3 ? ", " : "") + "(" + std::to_string(i) + ", 'more')";
        ASSERT_TRUE(Run(*db, sql + ";").success);
        ASSERT_TRUE(Run(*db, "CREATE INDEX idx_t ON t (id);").success);
        ASSERT_TRUE(Run(*db, "DELETE FROM t WHERE id = 1;").success);
        EXPECT_TRUE(db->HasUncommittedChanges());

        ASSERT_TRUE(db->Rollback(&error)) << error;
        EXPECT_FALSE(db->HasUncommittedChanges());
        EXPECT_EQ(db->GetCatalog().GetTableNames(), std::vector<std::string>{"t"});
        EXPECT_EQ(db->GetCatalog().GetIndex("t", "id"), nullptr);
        EXPECT_EQ(db->GetPager().GetPageCount(), pages);
        auto rows = Run(*db, "SELECT * FROM t;");
        ASSERT_EQ(rows.tuples.size(), 2u);
        EXPECT_EQ(rows.tuples[0].GetValue(0).GetAsInt(), 1);

        // The database keeps working after a rollback, and the index name is free
        ASSERT_TRUE(Run(*db, "CREATE INDEX idx_t ON t (id);").success);
        ASSERT_TRUE(Run(*db, "INSERT INTO t VALUES (3, 'three');").success);
        ASSERT_TRUE(db->Commit());
        db.reset();

        db = Database::Open(path_, &error);
        ASSERT_NE(db, nullptr) << error;
        EXPECT_EQ(Run(*db, "SELECT * FROM t WHERE id = 3;").tuples.size(), 1u);
        EXPECT_EQ(Run(*db, "SELECT * FROM t;").tuples.size(), 3u);
    }

    TEST_F(WalTest, FailedStatementCanBeRolledBackWhole)
    {
        std::string error;
        auto db = Database::Open(path_, &error);
        ASSERT_NE(db, nullptr) << error;
        Run(*db, "CREATE TABLE t (s VARCHAR(10000));");
        ASSERT_TRUE(db->Commit());

        // The first two rows are written before the third fails
        auto result = Run(*db, "INSERT INTO t VALUES ('a'), ('b'), ('" + std::string(5000, 'z') + "');");
        ASSERT_FALSE(result.success);
        EXPECT_EQ(Run(*db, "SELECT * FROM t;").tuples.size(), 2u);

        ASSERT_TRUE(db->Rollback(&error)) << error;
        EXPECT_EQ(Run(*db, "SELECT * FROM t;").tuples.size(), 0u);
    }

    TEST_F(WalTest, InMemoryDatabaseRollbackIsRejected)
    {
        auto db = Database::OpenInMemory();
        std::string error;
        EXPECT_FALSE(db->Rollback(&error));
        EXPECT_NE(error.find("in-memory"), std::string::npos);
    }

} // namespace sql
