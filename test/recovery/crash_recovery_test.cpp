#include <gtest/gtest.h>
#include "catalog/database.h"
#include "execution/executor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "storage/buffer_pool.h"
#include "storage/table_heap.h"
#include "storage/wal.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sql
{

    // Each test runs its writer in a forked child that ends with _exit():
    // no destructors, no close, no checkpoint - exactly what a crash leaves
    // behind. The parent then reopens the database and checks what survived.
    class CrashRecoveryTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            path_ = ::testing::TempDir() + "crash_test_" + std::to_string(getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
            wal_path_ = path_ + "-wal";
            Cleanup();
        }
        void TearDown() override { Cleanup(); }
        void Cleanup()
        {
            std::remove(path_.c_str());
            std::remove(wal_path_.c_str());
            std::remove((wal_path_ + ".bak").c_str());
        }

        // Runs body in a child process that "crashes" when body returns
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

        // Keep the database open until the child exits, so the "crash"
        // happens with it live (no destructor commits or closes it)
        static bool CrashWithOpen(std::unique_ptr<Database> &db, bool ok)
        {
            (void)db.release();
            return ok;
        }

        static bool Exec(Database &db, const std::string &sql)
        {
            Lexer lexer(sql);
            Parser parser(lexer);
            auto stmt = parser.ParseStatement();
            Executor executor(&db.GetCatalog());
            return executor.Execute(stmt.get()).success;
        }

        // A statement in its own transaction, as the REPL runs it
        static bool ExecCommit(Database &db, const std::string &sql)
        {
            return Exec(db, sql) && db.Commit();
        }

        static std::string InsertRows(const std::string &table, int from, int to)
        {
            std::string sql = "INSERT INTO " + table + " VALUES ";
            for (int i = from; i < to; ++i)
                sql += std::string(i > from ? ", " : "") + "(" + std::to_string(i) + ", 'row-" + std::to_string(i) + "')";
            return sql + ";";
        }

        std::unique_ptr<Database> Open()
        {
            std::string error;
            auto db = Database::Open(path_, &error);
            EXPECT_NE(db, nullptr) << error;
            return db;
        }

        static size_t Count(Database &db, const std::string &sql)
        {
            Lexer lexer(sql);
            Parser parser(lexer);
            auto stmt = parser.ParseStatement();
            Executor executor(&db.GetCatalog());
            return executor.Execute(stmt.get()).tuples.size();
        }

        static std::string ReadFile(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        static void WriteFile(const std::string &path, const std::string &bytes)
        {
            std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        }

        static uint64_t FileSize(const std::string &path)
        {
            struct stat st;
            return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
        }

        // Two committed transactions; returns the log's frame count after
        // the first (written to a side file by the child)
        uint64_t CommitTwoTransactionsAndCrash()
        {
            const std::string marker = path_ + ".frames";
            EXPECT_TRUE(RunAndCrash([&]
                                    {
                std::string error;
                auto db = Database::Open(path_, &error);
                if (!db || !ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));") ||
                    !ExecCommit(*db, InsertRows("t", 0, 10)))
                    return false;
                std::ofstream(marker) << db->GetPager().GetWalFrameCount();
                return CrashWithOpen(db, ExecCommit(*db, InsertRows("t", 10, 20))); }));
            uint64_t frames = 0;
            std::ifstream(marker) >> frames;
            std::remove(marker.c_str());
            return frames;
        }

        std::string path_;
        std::string wal_path_;
    };

    TEST_F(CrashRecoveryTest, CommittedWorkSurvivesACrash)
    {
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error);
            if (!db)
                return false;
            return CrashWithOpen(db, ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));") &&
                                         ExecCommit(*db, InsertRows("t", 0, 500)) &&
                                         ExecCommit(*db, "CREATE INDEX idx_t ON t (id);") &&
                                         ExecCommit(*db, "DELETE FROM t WHERE id >= 400;") &&
                                         ExecCommit(*db, "UPDATE t SET name = 'changed' WHERE id = 7;")); }));

        // Nothing reached the database file itself: it all lives in the log
        EXPECT_GT(FileSize(wal_path_), 0u);

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 400u);
        ASSERT_NE(db->GetCatalog().GetIndex("t", "id"), nullptr);
        EXPECT_EQ(Count(*db, "SELECT * FROM t WHERE id = 399;"), 1u);
        EXPECT_EQ(Count(*db, "SELECT * FROM t WHERE id = 450;"), 0u);
        EXPECT_EQ(Count(*db, "SELECT * FROM t WHERE name = 'changed';"), 1u);
    }

    TEST_F(CrashRecoveryTest, UncommittedWorkIsLost)
    {
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            // A tiny cache forces uncommitted pages out into the log
            auto db = Database::Open(path_, &error, 8);
            if (!db || !ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));") ||
                !ExecCommit(*db, InsertRows("t", 0, 10)))
                return false;
            return CrashWithOpen(db, Exec(*db, InsertRows("t", 10, 3000)) &&
                                         Exec(*db, "CREATE TABLE u (x INTEGER);") &&
                                         Exec(*db, "DELETE FROM t WHERE id < 5;")); }));

        // Uncommitted pages were spilled to the log before the crash
        EXPECT_GT(FileSize(wal_path_), WriteAheadLog::HEADER_SIZE + 20 * WriteAheadLog::FRAME_SIZE);

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(db->GetCatalog().GetTableNames(), std::vector<std::string>{"t"});
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 10u);

        // Fully usable afterwards
        ASSERT_TRUE(ExecCommit(*db, InsertRows("t", 10, 20)));
        db.reset();
        db = Open();
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 20u);
    }

    TEST_F(CrashRecoveryTest, TornCommitFrameDiscardsThatTransaction)
    {
        CommitTwoTransactionsAndCrash();
        // Cut the log in the middle of its last frame: the second
        // transaction's commit frame
        const uint64_t size = FileSize(wal_path_);
        ASSERT_EQ(truncate(wal_path_.c_str(), static_cast<off_t>(size - WriteAheadLog::FRAME_SIZE / 2)), 0);

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 10u);
    }

    TEST_F(CrashRecoveryTest, CorruptFrameStopsReplay)
    {
        const uint64_t frames_after_first = CommitTwoTransactionsAndCrash();
        ASSERT_GT(frames_after_first, 0u);

        // Flip a byte in the page data of the second transaction's first frame
        {
            std::fstream wal(wal_path_, std::ios::in | std::ios::out | std::ios::binary);
            const uint64_t offset = WriteAheadLog::HEADER_SIZE + frames_after_first * WriteAheadLog::FRAME_SIZE +
                                    WriteAheadLog::FRAME_HEADER_SIZE + 100;
            wal.seekg(static_cast<std::streamoff>(offset));
            char c = 0;
            wal.read(&c, 1);
            wal.seekp(static_cast<std::streamoff>(offset));
            c = static_cast<char>(c ^ 0x5A);
            wal.write(&c, 1);
        }

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 10u);
    }

    TEST_F(CrashRecoveryTest, ReplayingTheSameLogTwiceIsHarmless)
    {
        // Simulates a crash after a checkpoint updated the database file but
        // before it emptied the log: the next open replays the log again
        CommitTwoTransactionsAndCrash();
        std::filesystem::copy_file(wal_path_, wal_path_ + ".bak");
        {
            auto db = Open();
            ASSERT_NE(db, nullptr);
            EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 20u);
        }
        EXPECT_FALSE(std::filesystem::exists(wal_path_));
        std::filesystem::copy_file(wal_path_ + ".bak", wal_path_);

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 20u);
        ASSERT_TRUE(ExecCommit(*db, "CREATE INDEX idx_t ON t (id);"));
        EXPECT_EQ(Count(*db, "SELECT * FROM t WHERE id = 15;"), 1u);
    }

    TEST_F(CrashRecoveryTest, LeftoverLogIsNotAppliedToANewDatabase)
    {
        CommitTwoTransactionsAndCrash();
        ASSERT_GT(FileSize(wal_path_), 0u);
        std::remove(path_.c_str()); // database deleted, its log left behind

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_TRUE(db->WasCreated());
        EXPECT_TRUE(db->GetCatalog().GetTableNames().empty());
    }

    TEST_F(CrashRecoveryTest, CrashDuringInitialSetupLeavesAUsableFile)
    {
        // Crash right after creating the database, before any statement
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error);
            return CrashWithOpen(db, db != nullptr); }));

        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_TRUE(ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));"));
    }

    TEST_F(CrashRecoveryTest, RecoveryDefersCheckpointUntilClose)
    {
        CommitTwoTransactionsAndCrash();
        const uint64_t db_size = FileSize(path_);
        {
            auto db = Open();
            ASSERT_NE(db, nullptr);
            EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 20u); // read through the log
            EXPECT_EQ(FileSize(path_), db_size);             // database file untouched
        }
        EXPECT_GT(FileSize(path_), db_size); // checkpointed on close
        EXPECT_FALSE(std::filesystem::exists(wal_path_));
    }

    TEST_F(CrashRecoveryTest, FailedOpenChangesNeitherFile)
    {
        // Commit a corrupt schema row, then crash with it only in the log
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error);
            if (!db || !ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));"))
                return false;
            BufferPoolManager bpm(8, &db->GetPager());
            TableHeap schema(&bpm, db->GetPager().GetCatalogRoot());
            schema.InsertTuple(Tuple({Value("table"), Value("bogus"), Value("bogus"), Value(999), Value("id:0:0")}));
            bool ok = bpm.WriteDirtyPages() && db->GetPager().Commit();
            return CrashWithOpen(db, ok); }));

        const std::string db_bytes = ReadFile(path_);
        const std::string wal_bytes = ReadFile(wal_path_);
        std::string error;
        EXPECT_EQ(Database::Open(path_, &error), nullptr);
        EXPECT_NE(error.find("invalid root page"), std::string::npos) << error;
        EXPECT_EQ(ReadFile(path_), db_bytes);
        EXPECT_EQ(ReadFile(wal_path_), wal_bytes);
    }

    TEST_F(CrashRecoveryTest, LogOfAnotherDatabaseIsDiscarded)
    {
        // Database A crashes with commits in its log...
        CommitTwoTransactionsAndCrash();
        const std::string foreign_log = ReadFile(wal_path_);
        std::remove(path_.c_str());
        std::remove(wal_path_.c_str());

        // ...and database B is created at the same path and closed cleanly
        {
            auto db = Open();
            ASSERT_TRUE(ExecCommit(*db, "CREATE TABLE b (x INTEGER);"));
        }

        // A's log reappears next to B: it must not be replayed into B
        WriteFile(wal_path_, foreign_log);
        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_EQ(db->GetCatalog().GetTableNames(), std::vector<std::string>{"b"});
    }

    TEST_F(CrashRecoveryTest, CorruptLogHeaderWithFramesIsRefusedAndKept)
    {
        CommitTwoTransactionsAndCrash();
        std::string wal = ReadFile(wal_path_);
        wal[3] = static_cast<char>(wal[3] ^ 0x20); // damage the magic
        WriteFile(wal_path_, wal);
        const std::string db_bytes = ReadFile(path_);

        std::string error;
        EXPECT_EQ(Database::Open(path_, &error), nullptr);
        EXPECT_NE(error.find("corrupt"), std::string::npos) << error;
        EXPECT_EQ(ReadFile(wal_path_), wal); // committed frames preserved
        EXPECT_EQ(ReadFile(path_), db_bytes);
    }

    TEST_F(CrashRecoveryTest, TornHeaderOnlyLogIsReset)
    {
        {
            auto db = Open();
            ASSERT_TRUE(ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));"));
        }
        // A reset that tore while rewriting the header leaves at most a
        // header's worth of junk and no frames
        WriteFile(wal_path_, std::string(WriteAheadLog::HEADER_SIZE - 3, 'j'));
        auto db = Open();
        ASSERT_NE(db, nullptr);
        EXPECT_TRUE(db->GetCatalog().TableExists("t"));
    }

    TEST_F(CrashRecoveryTest, OlderFilesGetADatabaseId)
    {
        {
            auto db = Open();
            ASSERT_TRUE(ExecCommit(*db, "CREATE TABLE t (id INTEGER, name VARCHAR(20));"));
            ASSERT_TRUE(ExecCommit(*db, InsertRows("t", 0, 5)));
        }
        // Files written before database ids existed have zeros there
        std::string bytes = ReadFile(path_);
        for (size_t i = 24; i < 32; ++i)
            bytes[i] = 0;
        WriteFile(path_, bytes);

        {
            auto db = Open();
            ASSERT_NE(db, nullptr);
            EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 5u);
        }
        bytes = ReadFile(path_);
        EXPECT_NE(bytes.substr(24, 8), std::string(8, '\0'));

        // And its log is tied to it from then on
        ASSERT_TRUE(RunAndCrash([&]
                                {
            std::string error;
            auto db = Database::Open(path_, &error);
            return CrashWithOpen(db, db && ExecCommit(*db, InsertRows("t", 5, 10))); }));
        auto db = Open();
        EXPECT_EQ(Count(*db, "SELECT * FROM t;"), 10u);
    }

} // namespace sql
