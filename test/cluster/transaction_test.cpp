#include <gtest/gtest.h>
#include "cluster/shard/shard_state_machine.h"
#include "test_cluster.h"
#include <atomic>
#include <iostream>
#include <random>
#include <filesystem>
#include <thread>

// D6: distributed transactions. First a shard replica's side (log entries
// that open, extend, undo, prepare, commit and abort a transaction), then
// two-phase commit across a whole cluster.

namespace sql::cluster
{

    namespace
    {
        using namespace std::chrono_literals;
        using testing::Cluster;
        using testing::SHARDS;
        using testing::WaitFor;
        using Op = ShardStateMachine::TxnOp;

        std::vector<std::string> Rows(const ExecutionResult &result)
        {
            std::vector<std::string> rows;
            for (const auto &tuple : result.tuples)
            {
                std::string row;
                for (size_t i = 0; i < tuple.GetValueCount(); ++i)
                    row += (i ? "|" : "") + (tuple.GetValue(i).IsNull() ? "NULL" : tuple.GetValue(i).ToString());
                rows.push_back(row);
            }
            return rows;
        }

        void RunAll(Connection &connection, const std::string &script)
        {
            for (const auto &result : connection.ExecuteScript(script, true))
                ASSERT_TRUE(result.success) << result.message;
        }

        // A replica fed log entries directly, as NuRaft would
        class Replica
        {
        public:
            Replica()
                : dir_((std::filesystem::temp_directory_path() / ("txn_replica_" + std::to_string(getpid()) + "_" +
                                                                   std::to_string(counter_++)))
                           .string())
            {
                std::filesystem::remove_all(dir_);
                Open();
            }
            ~Replica()
            {
                machine_.reset();
                std::filesystem::remove_all(dir_);
            }

            void Open() { machine_ = std::make_unique<ShardStateMachine>(dir_); }
            void Crash() { machine_.reset(); } // the open transaction is lost with the process

            ExecutionResult Apply(nuraft::ptr<nuraft::buffer> entry)
            {
                log_.push_back(entry);
                return Commit(log_.size());
            }
            // Replays the log after the last applied index, as NuRaft does
            // on a restart
            void Replay()
            {
                for (size_t i = machine_->last_commit_index() + 1; i <= log_.size(); ++i)
                    Commit(i);
            }
            ExecutionResult Write(const std::string &sql)
            {
                return Apply(ShardStateMachine::EncodeWrite(sql, ReplicatedShard::NewRequestId()));
            }
            ExecutionResult Txn(Op op, const std::string &txid, const std::string &sql = "")
            {
                return Apply(ShardStateMachine::EncodeTxn(op, txid, sql, ReplicatedShard::NewRequestId()));
            }
            ShardStateMachine &Machine() { return *machine_; }

        private:
            ExecutionResult Commit(size_t index)
            {
                auto &entry = *log_[index - 1];
                entry.pos(0);
                auto result = machine_->commit(index, entry);
                return ShardStateMachine::DecodeResult(result.get());
            }

            static inline int counter_ = 0;
            std::string dir_;
            std::unique_ptr<ShardStateMachine> machine_;
            std::vector<nuraft::ptr<nuraft::buffer>> log_;
        };
    } // namespace

    TEST(ShardTransactionTest, OpenTransactionLocksTheShard)
    {
        Replica replica;
        ASSERT_TRUE(replica.Write("CREATE TABLE t (id INTEGER PRIMARY KEY);").success);
        ASSERT_TRUE(replica.Write("INSERT INTO t VALUES (1);").success);

        auto exec = replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (2);");
        ASSERT_TRUE(exec.success) << exec.message;
        EXPECT_EQ(replica.Machine().OpenTransaction(), "a");
        // Its own reads see the change; nobody else may read or write
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT COUNT(*) FROM t;", "a")), (std::vector<std::string>{"2"}));
        EXPECT_NE(replica.Machine().Query("SELECT COUNT(*) FROM t;").message.find("locked by another transaction"),
                  std::string::npos);
        EXPECT_NE(replica.Write("INSERT INTO t VALUES (3);").message.find("locked by another transaction"),
                  std::string::npos);
        EXPECT_NE(replica.Txn(Op::EXEC, "b", "INSERT INTO t VALUES (3);").message.find("locked by another transaction"),
                  std::string::npos);
        // Another transaction's abort leaves this one alone
        EXPECT_TRUE(replica.Txn(Op::ABORT, "b").success);
        EXPECT_EQ(replica.Machine().OpenTransaction(), "a");

        ASSERT_TRUE(replica.Txn(Op::COMMIT, "a").success);
        EXPECT_EQ(replica.Machine().OpenTransaction(), "");
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"2"}));
        ASSERT_TRUE(replica.Write("INSERT INTO t VALUES (3);").success);
    }

    TEST(ShardTransactionTest, UndoAbortAndPrepare)
    {
        Replica replica;
        ASSERT_TRUE(replica.Write("CREATE TABLE t (id INTEGER PRIMARY KEY);").success);
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (1);").success);
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (2), (3);").success);
        // A failing statement undoes itself; the transaction stays
        auto duplicate = replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (4), (1);");
        EXPECT_FALSE(duplicate.success);
        EXPECT_EQ(duplicate.message.find("statement rolled back"), std::string::npos) << duplicate.message;
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT COUNT(*) FROM t;", "a")), (std::vector<std::string>{"3"}));
        // UNDO takes back the last statement only
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "DELETE FROM t WHERE id = 1;").success);
        ASSERT_TRUE(replica.Txn(Op::UNDO, "a").success);
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT id FROM t ORDER BY id;", "a")),
                  (std::vector<std::string>{"1", "2", "3"}));
        // Prepared: no more statements, and no undo
        ASSERT_TRUE(replica.Txn(Op::PREPARE, "a").success);
        EXPECT_FALSE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (9);").success);
        EXPECT_FALSE(replica.Txn(Op::UNDO, "a").success);
        EXPECT_FALSE(replica.Txn(Op::PREPARE, "nosuch").success);
        // Abort throws all of it away
        ASSERT_TRUE(replica.Txn(Op::ABORT, "a").success);
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"0"}));
        EXPECT_FALSE(replica.Txn(Op::COMMIT, "a").success); // gone
        // Schema changes roll back too
        ASSERT_TRUE(replica.Txn(Op::EXEC, "b", "CREATE TABLE u (x INTEGER);").success);
        ASSERT_TRUE(replica.Txn(Op::EXEC, "b", "INSERT INTO u VALUES (1);").success);
        ASSERT_TRUE(replica.Txn(Op::ABORT, "b").success);
        EXPECT_FALSE(replica.Machine().Query("SELECT * FROM u;").success);
    }

    TEST(ShardTransactionTest, ARestartReplaysTheOpenTransaction)
    {
        Replica replica;
        ASSERT_TRUE(replica.Write("CREATE TABLE t (id INTEGER PRIMARY KEY);").success);
        ASSERT_TRUE(replica.Write("INSERT INTO t VALUES (1);").success);
        const auto before = replica.Machine().last_commit_index();
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (2);").success);
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (3);").success);
        ASSERT_TRUE(replica.Txn(Op::PREPARE, "a").success);
        EXPECT_FALSE(replica.Write("INSERT INTO t VALUES (4);").success); // locked
        replica.Crash();
        replica.Open();
        // Nothing of the open transaction reached the database file...
        EXPECT_EQ(replica.Machine().last_commit_index(), before);
        EXPECT_EQ(replica.Machine().OpenTransaction(), "");
        // ...and the log brings it back, prepared, exactly as it was
        replica.Replay();
        EXPECT_EQ(replica.Machine().OpenTransaction(), "a");
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT COUNT(*) FROM t;", "a")), (std::vector<std::string>{"3"}));
        EXPECT_FALSE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (5);").success);
        ASSERT_TRUE(replica.Txn(Op::COMMIT, "a").success);
        replica.Crash();
        replica.Open();
        replica.Replay();
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT id FROM t ORDER BY id;")),
                  (std::vector<std::string>{"1", "2", "3"}));
    }

    TEST(ShardTransactionTest, NoSnapshotWhileATransactionIsOpen)
    {
        Replica replica;
        ASSERT_TRUE(replica.Write("CREATE TABLE t (id INTEGER);").success);
        ASSERT_TRUE(replica.Txn(Op::EXEC, "a", "INSERT INTO t VALUES (1);").success);
        auto snapshot_at = [&](nuraft::ulong index)
        {
            nuraft::snapshot s(index, 1, nuraft::cs_new<nuraft::cluster_config>());
            bool created = false;
            nuraft::async_result<bool>::handler_type done = [&](bool ok, nuraft::ptr<std::exception> &)
            { created = ok; };
            replica.Machine().create_snapshot(s, done);
            return created;
        };
        EXPECT_FALSE(snapshot_at(replica.Machine().last_commit_index()));
        ASSERT_TRUE(replica.Txn(Op::COMMIT, "a").success);
        EXPECT_TRUE(snapshot_at(replica.Machine().last_commit_index()));
    }

    TEST(ShardTransactionTest, BatchesAreAllOrNothing)
    {
        Replica replica;
        auto batch = [&](const std::string &script)
        { return replica.Apply(ShardStateMachine::EncodeBatch(script, ReplicatedShard::NewRequestId())); };
        ASSERT_TRUE(batch("CREATE TABLE t (id INTEGER PRIMARY KEY); INSERT INTO t VALUES (1);").success);
        auto failed = batch("INSERT INTO t VALUES (2); INSERT INTO t VALUES (1); INSERT INTO t VALUES (3);");
        EXPECT_FALSE(failed.success);
        EXPECT_NE(failed.message.find("UNIQUE"), std::string::npos) << failed.message;
        EXPECT_EQ(failed.message.find("statement rolled back"), std::string::npos) << failed.message;
        EXPECT_EQ(Rows(replica.Machine().Query("SELECT id FROM t;")), (std::vector<std::string>{"1"}));
    }

    // ---- Across a cluster ----

    TEST(ClusterTransactionTest, StatementsAreAtomicAcrossShards)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"
                        "INSERT INTO t VALUES (1, 10);");
        // 2..40 spread over every shard; 1 is a duplicate on one of them
        std::string rows = "(1, 0)";
        for (int i = 2; i <= 40; ++i)
            rows += ", (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")";
        auto duplicate = client->Execute("INSERT INTO t VALUES " + rows + ";");
        EXPECT_FALSE(duplicate.success);
        EXPECT_NE(duplicate.message.find("UNIQUE constraint failed"), std::string::npos) << duplicate.message;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"1"}));
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_TRUE(WaitFor([&] { return cluster.ReplicaOf(0, s).Machine().OpenTransaction().empty(); }))
                << s; // followers apply the end a heartbeat later

        ASSERT_TRUE(client->Execute("INSERT INTO t VALUES " + rows.substr(rows.find("), ") + 3) + ";").success);
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"40|8200"}));
        EXPECT_GT(cluster.Leader()->TwoPhaseCommits(), 0u);
        // An UPDATE that fails on one shard (one row divides by zero)
        // changes no shard
        auto divide = client->Execute("UPDATE t SET v = 1000 / (v - 350);");
        EXPECT_FALSE(divide.success);
        EXPECT_NE(divide.message.find("Division by zero"), std::string::npos) << divide.message;
        EXPECT_EQ(Rows(client->Execute("SELECT SUM(v) FROM t;")), (std::vector<std::string>{"8200"}));
        auto ok = client->Execute("UPDATE t SET v = v - 5 WHERE v < 400;");
        ASSERT_TRUE(ok.success) << ok.message;
        EXPECT_EQ(ok.message, "39 row(s) updated.");
        auto deleted = client->Execute("DELETE FROM t WHERE id > 30;");
        ASSERT_TRUE(deleted.success) << deleted.message;
        EXPECT_EQ(deleted.message, "10 row(s) deleted.");
    }

    TEST(ClusterTransactionTest, BeginCommitAndRollback)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(1); // through a follower
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);");
        const uint64_t before = cluster.Leader()->TwoPhaseCommits();

        RunAll(*client, "BEGIN;");
        EXPECT_TRUE(client->InTransaction());
        for (int i = 1; i <= 12; ++i)
            ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ");")
                            .success);
        // Its own writes are visible to it, and only to it
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"12"}));
        auto failed = client->Execute("INSERT INTO t VALUES (13, 0), (1, 0);");
        EXPECT_FALSE(failed.success);
        EXPECT_NE(failed.message.find("(statement rolled back; transaction still open)"), std::string::npos)
            << failed.message;
        ASSERT_TRUE(client->Execute("UPDATE t SET v = v * 100 WHERE id <= 2;").success);
        auto committed = client->Execute("COMMIT;");
        ASSERT_TRUE(committed.success) << committed.message;
        EXPECT_FALSE(client->InTransaction());
        EXPECT_EQ(cluster.Leader()->TwoPhaseCommits(), before + 1);
        auto other = cluster.Client(2);
        EXPECT_EQ(Rows(other->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"12|375"}));

        RunAll(*client, "BEGIN; DELETE FROM t; INSERT INTO t VALUES (100, 1);");
        EXPECT_EQ(Rows(client->Execute("SELECT id FROM t;")), (std::vector<std::string>{"100"}));
        RunAll(*client, "ROLLBACK;");
        EXPECT_EQ(Rows(other->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"12|375"}));
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_TRUE(WaitFor([&] { return cluster.ReplicaOf(1, s).Machine().OpenTransaction().empty(); }))
                << s; // followers apply the end a heartbeat later
    }

    TEST(ClusterTransactionTest, SchemaChangesInsideATransaction)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY);");
        RunAll(*client, "BEGIN; CREATE TABLE gone (id INTEGER PRIMARY KEY, s VARCHAR(5));"
                        "INSERT INTO gone VALUES (1, 'a'), (2, 'b'), (3, 'c');");
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM gone;")), (std::vector<std::string>{"3"}));
        RunAll(*client, "ROLLBACK;");
        EXPECT_FALSE(client->Execute("SELECT * FROM gone;").success);
        EXPECT_EQ(client->Command("schema", "gone"), "Error: no table named gone\n");

        // Replace a table in one transaction: drop it, create it anew
        RunAll(*client, "INSERT INTO t VALUES (1), (2);"
                        "BEGIN; DROP TABLE t; CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(10));"
                        "INSERT INTO t VALUES (7, 'seven'); COMMIT;");
        EXPECT_EQ(Rows(client->Execute("SELECT id, name FROM t;")), (std::vector<std::string>{"7|seven"}));
        EXPECT_NE(client->Command("schema", "t").find("name VARCHAR"), std::string::npos);
        // Later schema changes still reach every shard once
        RunAll(*client, "CREATE INDEX t_name ON t (name); INSERT INTO t VALUES (8, 'eight');");
        EXPECT_EQ(Rows(client->Execute("SELECT id FROM t WHERE name = 'eight';")), (std::vector<std::string>{"8"}));
        // A new leader rebuilds the same schema from the metadata
        size_t index = 0;
        for (size_t i = 0; i < 3; ++i)
            if (cluster.Client(i)->Command("describe", "").find("coordinator " + std::to_string(i + 1) + " (leader)") !=
                std::string::npos)
                index = i;
        cluster.StopCoordinator(index);
        auto survivor = cluster.Client((index + 1) % 3);
        ExecutionResult rows;
        ASSERT_TRUE(WaitFor([&] { return (rows = survivor->Execute("SELECT id, name FROM t ORDER BY id;")).success; }))
            << rows.message;
        EXPECT_EQ(Rows(rows), (std::vector<std::string>{"7|seven", "8|eight"}));
    }

    TEST(ClusterTransactionTest, OthersWaitForTheLockedShards)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto a = cluster.Client(0), b = cluster.Client(0);
        RunAll(*a, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 0), (2, 0), (3, 0);");
        RunAll(*a, "BEGIN; UPDATE t SET v = 1;");
        // b's read waits until a commits, then sees all of it
        std::atomic<bool> done{false};
        ExecutionResult seen;
        std::thread reader(
            [&]
            {
                seen = b->Execute("SELECT SUM(v) FROM t;");
                done = true;
            });
        std::this_thread::sleep_for(500ms);
        EXPECT_FALSE(done.load());
        RunAll(*a, "COMMIT;");
        reader.join();
        ASSERT_TRUE(seen.success) << seen.message;
        EXPECT_EQ(Rows(seen), (std::vector<std::string>{"3"}));

        // A client that goes away mid-transaction releases its shards
        {
            auto gone = cluster.Client(1);
            RunAll(*gone, "BEGIN; UPDATE t SET v = 100;");
        }
        auto write = b->Execute("UPDATE t SET v = v + 1;");
        ASSERT_TRUE(write.success) << write.message;
        EXPECT_EQ(Rows(b->Execute("SELECT SUM(v) FROM t;")), (std::vector<std::string>{"6"}));

        // Waiting gives up after the lock timeout; the transaction goes on
        RunAll(*a, "BEGIN; UPDATE t SET v = 0;");
        RunAll(*b, "BEGIN;");
        auto timed_out = b->Execute("UPDATE t SET v = 5;");
        EXPECT_FALSE(timed_out.success);
        EXPECT_NE(timed_out.message.find("lock wait timed out"), std::string::npos) << timed_out.message;
        EXPECT_TRUE(b->InTransaction());
        RunAll(*b, "ROLLBACK;");
        RunAll(*a, "COMMIT;");
        EXPECT_EQ(Rows(b->Execute("SELECT SUM(v) FROM t;")), (std::vector<std::string>{"0"}));
    }

    TEST(ClusterTransactionTest, ANewLeaderSettlesWhatTheOldOneLeftOpen)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY); INSERT INTO t VALUES (1);");
        Coordinator *leader = cluster.Leader();
        const std::string epoch = std::to_string(leader->Epoch());
        // Two transactions open on every shard as a coordinator would leave
        // them when it dies mid-commit: "lost" has no decision (so it rolls
        // back), "decided" was decided committed
        auto open = [&](const std::string &txid, uint32_t s, int value)
        {
            for (size_t n = 0; n < 3; ++n)
            {
                auto r = cluster.Node(n).Handle("txn", {epoch, std::to_string(s), "exec", txid,
                                                        "INSERT INTO t VALUES (" + std::to_string(value) + ");",
                                                        ReplicatedShard::NewRequestId()});
                if (r.success)
                    return true;
            }
            return false;
        };
        ASSERT_TRUE(open("lost", 0, 100));
        ASSERT_TRUE(open("decided", 1, 200));
        ASSERT_TRUE(open("decided", 2, 300));
        ASSERT_TRUE(leader->Metadata().Write("INSERT INTO txns VALUES ('decided', 'committed');").success);

        size_t index = 0;
        for (size_t i = 0; i < 3; ++i)
            if (cluster.Client(i)->Command("describe", "").find("coordinator " + std::to_string(i + 1) + " (leader)") !=
                std::string::npos)
                index = i;
        cluster.StopCoordinator(index);
        auto survivor = cluster.Client((index + 1) % 3);
        ExecutionResult rows;
        ASSERT_TRUE(WaitFor([&] { return (rows = survivor->Execute("SELECT id FROM t ORDER BY id;")).success; }))
            << rows.message;
        // 200 and 300 are rows on shards 1 and 2 only as far as this test is
        // concerned: the shard placement of a raw write is not checked
        EXPECT_EQ(Rows(rows), (std::vector<std::string>{"1", "200", "300"}));
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_TRUE(WaitFor([&] { return cluster.ReplicaOf(0, s).Machine().OpenTransaction().empty(); }))
                << s; // followers apply the end a heartbeat later
        EXPECT_TRUE(survivor->Execute("INSERT INTO t VALUES (2);").success);
    }

    TEST(ClusterTransactionTest, AnIdleTransactionIsRolledBack)
    {
        Cluster cluster(3, /*idle_transaction_timeout_ms=*/1500);
        ASSERT_TRUE(cluster.WaitFormed());
        auto idle = cluster.Client(1), busy = cluster.Client(2);
        RunAll(*idle, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES (1, 0), (2, 0);"
                      "BEGIN; UPDATE t SET v = 1;");
        // Someone else waits for it, but not forever
        auto write = busy->Execute("UPDATE t SET v = 2;");
        ASSERT_TRUE(write.success) << write.message;
        EXPECT_EQ(Rows(busy->Execute("SELECT SUM(v) FROM t;")), (std::vector<std::string>{"4"}));
        // The idle client still thinks it is in its transaction: its next
        // statement is told what happened instead of running on its own
        EXPECT_TRUE(idle->InTransaction());
        auto told = idle->Execute("SELECT COUNT(*) FROM t;");
        EXPECT_FALSE(told.success);
        EXPECT_NE(told.message.find("rolled back after being idle"), std::string::npos) << told.message;
        EXPECT_FALSE(idle->InTransaction());
        auto after = idle->Execute("UPDATE t SET v = 3 WHERE id = 1;");
        ASSERT_TRUE(after.success) << after.message; // now a statement of its own
        EXPECT_EQ(Rows(busy->Execute("SELECT SUM(v) FROM t;")), (std::vector<std::string>{"5"}));
        // ROLLBACK after the timeout just agrees
        RunAll(*idle, "BEGIN;");
        std::this_thread::sleep_for(2500ms);
        auto rollback = idle->Execute("ROLLBACK;");
        EXPECT_TRUE(rollback.success) << rollback.message;
        EXPECT_EQ(rollback.message, "Transaction rolled back.");
    }

    // The design's acceptance test: money moves between accounts on
    // different shards while a storage node and the leading coordinator are
    // killed, and none is created or lost
    TEST(ClusterTransactionTest, TransfersKeepTheTotalThroughFailures)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        constexpr int ACCOUNTS = 16, START = 100;
        {
            auto setup = cluster.Client(0);
            std::string rows;
            for (int i = 1; i <= ACCOUNTS; ++i)
                rows += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", " + std::to_string(START) + ")";
            RunAll(*setup, "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER NOT NULL);"
                           "INSERT INTO accounts VALUES " + rows + ";");
        }
        std::atomic<bool> stop{false};
        std::atomic<int> committed{0}, failed{0};
        std::mutex reasons_mutex;
        std::map<std::string, int> reasons;
        auto client_loop = [&](int seed)
        {
            std::mt19937 random(static_cast<unsigned>(seed));
            std::unique_ptr<net::RemoteConnection> connection;
            size_t coordinator = static_cast<size_t>(seed) % 3;
            while (!stop)
            {
                try
                {
                    if (!connection)
                        connection = cluster.Client(coordinator);
                    const int from = 1 + static_cast<int>(random() % ACCOUNTS);
                    const int to = 1 + static_cast<int>((from + random() % (ACCOUNTS - 1)) % ACCOUNTS);
                    const int amount = 1 + static_cast<int>(random() % 20);
                    std::string why;
                    auto run = [&](const std::string &sql)
                    {
                        auto r = connection->Execute(sql);
                        if (!r.success)
                            why = r.message;
                        return r.success;
                    };
                    bool ok = run("BEGIN;");
                    ok = ok && run("UPDATE accounts SET balance = balance - " + std::to_string(amount) +
                                   " WHERE id = " + std::to_string(from) + ";");
                    ok = ok && run("UPDATE accounts SET balance = balance + " + std::to_string(amount) +
                                   " WHERE id = " + std::to_string(to) + ";");
                    ok = ok && run("COMMIT;");
                    if (!ok)
                    {
                        std::lock_guard<std::mutex> lock(reasons_mutex);
                        ++reasons[why.substr(0, 60)];
                    }
                    if (ok)
                        ++committed;
                    else
                    {
                        ++failed;
                        if (connection->InTransaction())
                            connection->Execute("ROLLBACK;");
                    }
                }
                catch (const std::exception &)
                {
                    // the coordinator went away: another one, a new transaction
                    connection.reset();
                    coordinator = (coordinator + 1) % 3;
                    ++failed;
                    std::this_thread::sleep_for(100ms);
                }
            }
        };
        std::vector<std::thread> clients;
        for (int i = 0; i < 4; ++i)
            clients.emplace_back(client_loop, i + 1);

        std::this_thread::sleep_for(2s);
        const int before_failures = committed.load();
        cluster.StopNode(2); // shards lose a replica, some their leader
        std::this_thread::sleep_for(3s);
        size_t index = 0;
        for (size_t i = 0; i < 3; ++i)
            if (cluster.Client(i)->Command("describe", "").find("coordinator " + std::to_string(i + 1) + " (leader)") !=
                std::string::npos)
                index = i;
        cluster.StopCoordinator(index);
        std::this_thread::sleep_for(6s);
        const int before_stop = committed.load();
        std::this_thread::sleep_for(2s);
        stop = true;
        for (auto &t : clients)
            t.join();

        std::cout << committed.load() << " transfers committed, " << failed.load() << " failed or rolled back\n";
        for (const auto &[why, n] : reasons)
            std::cout << "  " << n << " x " << why << "\n";
        EXPECT_GT(before_failures, 0);
        EXPECT_GT(committed.load(), before_stop) << "transfers resume after the failures";
        auto auditor = cluster.Client((index + 1) % 3);
        ExecutionResult total;
        ASSERT_TRUE(WaitFor([&] { return (total = auditor->Execute("SELECT COUNT(*), SUM(balance) FROM accounts;"))
                                         .success; }))
            << total.message;
        EXPECT_EQ(Rows(total), (std::vector<std::string>{std::to_string(ACCOUNTS) + "|" +
                                                         std::to_string(ACCOUNTS * START)}))
            << committed.load() << " committed, " << failed.load() << " failed";
        // Nothing stays locked
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_TRUE(WaitFor([&] { return cluster.ReplicaOf(0, s).Machine().OpenTransaction().empty(); })) << s;
    }

} // namespace sql::cluster
