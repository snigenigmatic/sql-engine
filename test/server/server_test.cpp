#include <gtest/gtest.h>
#include "cli/local_connection.h"
#include "net/remote_connection.h"
#include "server/server.h"
#include "session/session.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>
#include <unistd.h>

namespace sql
{

    namespace
    {
        using namespace std::chrono_literals;

        std::string RowText(const Tuple &tuple)
        {
            std::string text;
            for (size_t i = 0; i < tuple.GetValueCount(); ++i)
                text += (i ? "|" : "") + (tuple.GetValue(i).IsNull() ? "NULL" : tuple.GetValue(i).ToString());
            return text;
        }

        std::vector<std::string> Rows(const ExecutionResult &result)
        {
            std::vector<std::string> rows;
            for (const auto &tuple : result.tuples)
                rows.push_back(RowText(tuple));
            return rows;
        }

        // A server on a free localhost port, over a temporary database file
        struct TestServer
        {
            std::string path;
            struct RemoveFiles // declared first: runs after the database closes
            {
                std::string path;
                ~RemoveFiles()
                {
                    std::filesystem::remove(path);
                    std::filesystem::remove(path + "-wal");
                }
            } cleanup;
            std::unique_ptr<Database> db;
            std::unique_ptr<Server> server;

            TestServer()
                : path((std::filesystem::temp_directory_path() /
                        ("server_test_" + std::to_string(getpid()) + "_" + std::to_string(Counter()++) + ".db"))
                           .string()),
                  cleanup{path}
            {
                std::filesystem::remove(path);
                std::filesystem::remove(path + "-wal");
                std::string error;
                db = Database::Open(path, &error);
                EXPECT_NE(db, nullptr) << error;
                server = std::make_unique<Server>(db.get());
                server->Start("127.0.0.1", 0);
            }

            ~TestServer() { server->Stop(); }

            std::unique_ptr<net::RemoteConnection> Connect()
            {
                return std::make_unique<net::RemoteConnection>("127.0.0.1", server->Port());
            }

            static int &Counter()
            {
                static int counter = 0;
                return counter;
            }
        };
    } // namespace

    TEST(ServerTest, RemoteResultsMatchLocalOnes)
    {
        const std::string setup = "CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(10), score FLOAT);"
                                  "INSERT INTO t VALUES (1, 'ann', 2.5), (2, NULL, -1.0), (3, 'cy', NULL);";
        const std::vector<std::string> queries = {
            "SELECT * FROM t ORDER BY id;",
            "SELECT name, score * 2 AS doubled FROM t WHERE score IS NOT NULL ORDER BY id;",
            "SELECT COUNT(*), AVG(score) FROM t;",
            "SELECT nosuch FROM t;",
            "INSERT INTO t VALUES (1, 'dup', 0.0);",
            "UPDATE t SET score = 9.0 WHERE id = 3;",
            "SELECT * FROM t WHERE id = 3;",
        };

        TestServer remote;
        auto client = remote.Connect();
        TestServer local; // the same statements, in this process
        Session session(local.db.get());
        LocalConnection direct(local.db.get(), &session);

        auto a = client->ExecuteScript(setup), b = direct.ExecuteScript(setup);
        ASSERT_EQ(a.size(), b.size());
        for (const auto &sql : queries)
        {
            const ExecutionResult got = client->Execute(sql), want = direct.Execute(sql);
            EXPECT_EQ(got.success, want.success) << sql;
            EXPECT_EQ(got.message, want.message) << sql;
            EXPECT_EQ(got.column_names, want.column_names) << sql;
            EXPECT_EQ(Rows(got), Rows(want)) << sql;
        }
        EXPECT_EQ(client->Command("tables", ""), direct.Command("tables", ""));
        EXPECT_EQ(client->Command("schema", "t"), direct.Command("schema", "t"));
        EXPECT_EQ(client->Command("schema", "nosuch"), "Error: no table named nosuch\n");
        // Execute takes exactly one statement, as Session::Execute does
        const std::string two = "SELECT id FROM t; SELECT name FROM t;";
        EXPECT_FALSE(direct.Execute(two).success);
        EXPECT_EQ(client->Execute(two).success, direct.Execute(two).success);
        EXPECT_EQ(client->Execute(two).message, direct.Execute(two).message);
        // A script stops at its first failure only when asked to
        const std::string script = "SELECT id FROM t WHERE id = 1; SELECT x FROM nosuch; SELECT id FROM t WHERE id = 3;";
        EXPECT_EQ(client->ExecuteScript(script, true).size(), 2u);
        const auto all = client->ExecuteScript(script);
        ASSERT_EQ(all.size(), 3u);
        EXPECT_EQ(Rows(all[2]), (std::vector<std::string>{"3"}));
    }

    TEST(ServerTest, ChangesAreSharedAndDurable)
    {
        TestServer remote;
        auto writer = remote.Connect();
        ASSERT_TRUE(writer->Execute("CREATE TABLE t (a INTEGER);").success);
        ASSERT_TRUE(writer->Execute("INSERT INTO t VALUES (1), (2);").success);
        auto reader = remote.Connect();
        EXPECT_EQ(Rows(reader->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"2"}));
        // Committed on the server's database: visible there directly
        Session session(remote.db.get());
        remote.server->Stop(); // no client holds the database now
        EXPECT_EQ(Rows(session.Execute("SELECT SUM(a) FROM t;")), (std::vector<std::string>{"3"}));
    }

    TEST(ServerTest, ATransactionHasTheDatabaseToItself)
    {
        TestServer remote;
        auto a = remote.Connect(), b = remote.Connect();
        ASSERT_TRUE(a->Execute("CREATE TABLE t (a INTEGER);").success);
        ASSERT_TRUE(a->Execute("BEGIN;").success);
        EXPECT_TRUE(a->InTransaction());
        EXPECT_FALSE(b->InTransaction());
        ASSERT_TRUE(a->Execute("INSERT INTO t VALUES (1);").success);

        // b's read waits for a's transaction, then sees its outcome
        std::atomic<bool> committed{false};
        auto read = std::async(std::launch::async,
                               [&]
                               {
                                   const ExecutionResult result = b->Execute("SELECT COUNT(*) FROM t;");
                                   return std::make_pair(committed.load(), Rows(result));
                               });
        EXPECT_EQ(read.wait_for(300ms), std::future_status::timeout);
        ASSERT_TRUE(a->Execute("INSERT INTO t VALUES (2);").success);
        committed = true;
        ASSERT_TRUE(a->Execute("COMMIT;").success);
        EXPECT_FALSE(a->InTransaction());
        const auto [after_commit, rows] = read.get();
        EXPECT_TRUE(after_commit);
        EXPECT_EQ(rows, (std::vector<std::string>{"2"}));
    }

    TEST(ServerTest, DisconnectingRollsBack)
    {
        TestServer remote;
        auto observer = remote.Connect();
        ASSERT_TRUE(observer->Execute("CREATE TABLE t (a INTEGER);").success);
        {
            auto client = remote.Connect();
            ASSERT_TRUE(client->Execute("BEGIN;").success);
            ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (1);").success);
        } // gone without COMMIT
        EXPECT_EQ(Rows(observer->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"0"}));
        // The database is free again: a new transaction can start and commit
        ASSERT_TRUE(observer->Execute("BEGIN;").success);
        ASSERT_TRUE(observer->Execute("INSERT INTO t VALUES (5);").success);
        ASSERT_TRUE(observer->Execute("COMMIT;").success);
        EXPECT_EQ(Rows(observer->Execute("SELECT a FROM t;")), (std::vector<std::string>{"5"}));
    }

    TEST(ServerTest, StopDisconnectsClients)
    {
        TestServer remote;
        auto idle = remote.Connect();
        auto in_transaction = remote.Connect();
        ASSERT_TRUE(idle->Execute("CREATE TABLE t (a INTEGER);").success);
        ASSERT_TRUE(in_transaction->Execute("BEGIN;").success);
        ASSERT_TRUE(in_transaction->Execute("INSERT INTO t VALUES (1);").success);
        // A third client is waiting for the database when the server stops
        auto waiting_client = remote.Connect();
        auto waiting =
            std::async(std::launch::async, [&] { return waiting_client->Execute("SELECT COUNT(*) FROM t;"); });
        std::this_thread::sleep_for(100ms);

        const auto start = std::chrono::steady_clock::now();
        remote.server->Stop();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
        try
        {
            const auto unexpected = waiting.get();
            ADD_FAILURE() << "the waiting client's request ran: success=" << unexpected.success
                          << " rows=" << unexpected.tuples.size();
        }
        catch (const std::runtime_error &)
        {
            // disconnected, as it should be
        }
        EXPECT_THROW(idle->Execute("SELECT COUNT(*) FROM t;"), std::runtime_error);
        EXPECT_TRUE(in_transaction->InTransaction()); // as last reported, until it tries again
        EXPECT_THROW(in_transaction->Execute("COMMIT;"), std::runtime_error);
        EXPECT_FALSE(in_transaction->InTransaction()); // the server rolled it back
        // Nothing of the open transaction was kept
        Session session(remote.db.get());
        EXPECT_EQ(Rows(session.Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"0"}));
    }

    TEST(ServerTest, UnreachableServersAndGarbageAreErrors)
    {
        EXPECT_THROW(net::RemoteConnection("127.0.0.1", 1), std::runtime_error);
        TestServer remote;
        // A client speaking nonsense gets an error frame, and the server
        // keeps serving others
        net::Socket raw = net::Connect("127.0.0.1", remote.server->Port());
        net::WriteFrame(raw, net::Frame{net::MessageType::RESULTS, "nonsense"});
        net::Frame reply;
        ASSERT_TRUE(net::ReadFrame(raw, &reply));
        EXPECT_EQ(net::DecodeError(reply), std::optional<std::string>("malformed request"));
        auto client = remote.Connect();
        EXPECT_TRUE(client->Execute("CREATE TABLE t (a INTEGER);").success);
    }

} // namespace sql
