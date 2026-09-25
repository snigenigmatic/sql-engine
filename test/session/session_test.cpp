#include <gtest/gtest.h>
#include "catalog/database.h"
#include "session/session.h"
#include <cstdio>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace sql
{

    class SessionTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "session_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            Cleanup();
        }
        void TearDown() override { Cleanup(); }
        void Cleanup()
        {
            std::remove(path_.c_str());
            std::remove((path_ + "-wal").c_str());
        }

        std::unique_ptr<Database> Open(size_t pool_size = Database::DEFAULT_POOL_SIZE)
        {
            std::string error;
            auto db = Database::Open(path_, &error, pool_size);
            EXPECT_NE(db, nullptr) << error;
            return db;
        }

        static size_t Count(Session &session, const std::string &table)
        {
            auto result = session.Execute("SELECT * FROM " + table + ";");
            EXPECT_TRUE(result.success) << result.message;
            return result.tuples.size();
        }

        // Row count in a freshly reopened database (what is durable)
        size_t CountAfterReopen(const std::string &table)
        {
            auto db = Open();
            Session session(db.get());
            return Count(session, table);
        }

        static std::string InsertRows(int from, int to)
        {
            std::string sql = "INSERT INTO t VALUES ";
            for (int i = from; i < to; ++i)
                sql += std::string(i > from ? ", " : "") + "(" + std::to_string(i) + ", 'row')";
            return sql + ";";
        }

        std::string path_;
    };

    TEST_F(SessionTest, AutoCommitStatementsAreDurable)
    {
        {
            auto db = Open();
            Session session(db.get());
            ASSERT_TRUE(session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));").success);
            ASSERT_TRUE(session.Execute(InsertRows(0, 5)).success);
            EXPECT_FALSE(db->HasUncommittedChanges());
            EXPECT_FALSE(session.InTransaction());
        }
        EXPECT_EQ(CountAfterReopen("t"), 5u);
    }

    TEST_F(SessionTest, FailedAutoCommitStatementLeavesNoTrace)
    {
        auto db = Open();
        Session session(db.get());
        session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10000));");
        auto result = session.Execute("INSERT INTO t VALUES (1, 'a'), (2, '" + std::string(5000, 'z') + "');");
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.message.find("statement rolled back"), std::string::npos) << result.message;
        EXPECT_EQ(Count(session, "t"), 0u);
    }

    TEST_F(SessionTest, CommittedTransactionIsDurable)
    {
        {
            auto db = Open();
            Session session(db.get());
            session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));");
            ASSERT_TRUE(session.Execute("BEGIN;").success);
            EXPECT_TRUE(session.InTransaction());
            ASSERT_TRUE(session.Execute(InsertRows(0, 3)).success);
            ASSERT_TRUE(session.Execute("DELETE FROM t WHERE id = 0;").success);
            ASSERT_TRUE(session.Execute("CREATE INDEX idx_t ON t (id);").success);
            EXPECT_EQ(Count(session, "t"), 2u); // own changes are visible
            auto commit = session.Execute("COMMIT;");
            ASSERT_TRUE(commit.success) << commit.message;
            EXPECT_FALSE(session.InTransaction());
        }
        auto db = Open();
        Session session(db.get());
        EXPECT_EQ(Count(session, "t"), 2u);
        EXPECT_NE(db->GetCatalog().GetIndex("t", "id"), nullptr);
    }

    TEST_F(SessionTest, RolledBackTransactionLeavesNoTrace)
    {
        auto db = Open(16);
        Session session(db.get());
        session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));");
        session.Execute(InsertRows(0, 5));
        const uint32_t pages = db->GetPager().GetPageCount();

        ASSERT_TRUE(session.Execute("BEGIN;").success);
        ASSERT_TRUE(session.Execute(InsertRows(5, 3000)).success); // spills out of the cache
        ASSERT_TRUE(session.Execute("CREATE TABLE u (x INTEGER);").success);
        ASSERT_TRUE(session.Execute("CREATE INDEX idx_t ON t (id);").success);
        ASSERT_TRUE(session.Execute("DROP TABLE t;").success);
        EXPECT_FALSE(db->GetCatalog().TableExists("t"));

        auto rollback = session.Execute("ROLLBACK;");
        ASSERT_TRUE(rollback.success) << rollback.message;
        EXPECT_FALSE(session.InTransaction());
        EXPECT_EQ(db->GetCatalog().GetTableNames(), std::vector<std::string>{"t"});
        EXPECT_EQ(db->GetCatalog().GetIndex("t", "id"), nullptr);
        EXPECT_EQ(db->GetPager().GetPageCount(), pages);
        EXPECT_EQ(Count(session, "t"), 5u);
    }

    TEST_F(SessionTest, FailedStatementInsideTransactionIsUndoneAlone)
    {
        {
            auto db = Open();
            Session session(db.get());
            session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10000));");
            session.Execute("BEGIN;");
            ASSERT_TRUE(session.Execute("INSERT INTO t VALUES (1, 'first');").success);

            // Writes one row, then fails on the next
            auto failed = session.Execute("INSERT INTO t VALUES (2, 'b'), (3, '" + std::string(5000, 'z') + "');");
            EXPECT_FALSE(failed.success);
            EXPECT_NE(failed.message.find("transaction still open"), std::string::npos) << failed.message;
            EXPECT_TRUE(session.InTransaction());

            ASSERT_TRUE(session.Execute("INSERT INTO t VALUES (4, 'last');").success);
            ASSERT_TRUE(session.Execute("COMMIT;").success);
        }
        auto db = Open();
        Session session(db.get());
        auto rows = session.Execute("SELECT id FROM t;");
        ASSERT_EQ(rows.tuples.size(), 2u);
        EXPECT_EQ(rows.tuples[0].GetValue(0).GetAsInt(), 1);
        EXPECT_EQ(rows.tuples[1].GetValue(0).GetAsInt(), 4);
    }

    TEST_F(SessionTest, TransactionControlErrors)
    {
        auto db = Open();
        Session session(db.get());
        EXPECT_FALSE(session.Execute("COMMIT;").success);
        EXPECT_FALSE(session.Execute("ROLLBACK;").success);
        ASSERT_TRUE(session.Execute("BEGIN;").success);
        auto again = session.Execute("BEGIN;");
        EXPECT_FALSE(again.success);
        EXPECT_NE(again.message.find("already in progress"), std::string::npos);
        EXPECT_TRUE(session.InTransaction());

        std::string error;
        EXPECT_FALSE(session.Checkpoint(&error));
        EXPECT_NE(error.find("inside a transaction"), std::string::npos);
        ASSERT_TRUE(session.Execute("COMMIT;").success);
        EXPECT_TRUE(session.Checkpoint(&error)) << error;
    }

    TEST_F(SessionTest, OpenTransactionIsRolledBackWhenSessionEnds)
    {
        {
            auto db = Open();
            Session session(db.get());
            session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));");
            session.Execute("BEGIN;");
            session.Execute(InsertRows(0, 10));
        } // session ends, then the database closes
        EXPECT_EQ(CountAfterReopen("t"), 0u);
    }

    TEST_F(SessionTest, ParseErrorsAreReported)
    {
        auto db = Open();
        Session session(db.get());
        auto result = session.Execute("SELEKT * FROM t;");
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.message.find("Parse error"), std::string::npos);
    }

    TEST_F(SessionTest, InMemoryDatabaseRunsInAutoCommitOnly)
    {
        auto db = Database::OpenInMemory();
        Session session(db.get());
        ASSERT_TRUE(session.Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));").success);
        ASSERT_TRUE(session.Execute(InsertRows(0, 3)).success);
        EXPECT_EQ(Count(session, "t"), 3u);
        EXPECT_FALSE(session.Execute("BEGIN;").success);
    }

    // ── Crashes ───────────────────────────────────────────────────────────────────

    static bool RunAndCrash(const std::function<bool()> &body)
    {
        std::fflush(nullptr);
        pid_t pid = fork();
        if (pid == 0)
        {
            bool ok = false;
            try
            {
                ok = body();
            }
            catch (...)
            {
            }
            _exit(ok ? 0 : 1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    TEST_F(SessionTest, CrashInsideTransactionLosesOnlyThatTransaction)
    {
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error, 8);
            if (!db)
                return false;
            auto *session = new Session(db.get()); // never destroyed: the crash
            bool ok = session->Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));").success &&
                      session->Execute(InsertRows(0, 5)).success &&
                      session->Execute("BEGIN;").success &&
                      session->Execute(InsertRows(5, 3000)).success &&
                      session->Execute("DELETE FROM t WHERE id < 3;").success;
            (void)db.release();
            return ok; }));

        EXPECT_EQ(CountAfterReopen("t"), 5u);
    }

    TEST_F(SessionTest, CommittedTransactionSurvivesCrash)
    {
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error);
            if (!db)
                return false;
            auto *session = new Session(db.get());
            bool ok = session->Execute("CREATE TABLE t (id INTEGER, s VARCHAR(10));").success &&
                      session->Execute("BEGIN;").success &&
                      session->Execute(InsertRows(0, 100)).success &&
                      session->Execute("COMMIT;").success &&
                      session->Execute("BEGIN;").success &&
                      session->Execute(InsertRows(100, 200)).success; // never committed
            (void)db.release();
            return ok; }));

        EXPECT_EQ(CountAfterReopen("t"), 100u);
    }

} // namespace sql
