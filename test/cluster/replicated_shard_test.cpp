#include <gtest/gtest.h>
#include "cluster/shard/replicated_shard.h"
#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// D3: one shard replicated over Raft. Three replicas (each a NuRaft server
// with a durable log and a database) on localhost ports.

namespace sql::cluster
{

    namespace
    {
        using namespace std::chrono_literals;

        int FreePort()
        {
            static std::set<int> handed_out;
            for (;;)
            {
                const int fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
                socklen_t length = sizeof(addr);
                getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &length);
                close(fd);
                const int port = ntohs(addr.sin_port);
                if (handed_out.insert(port).second)
                    return port;
            }
        }

        bool WaitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = 15s)
        {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                    return true;
                std::this_thread::sleep_for(20ms);
            }
            return condition();
        }

        std::vector<std::string> Rows(const ExecutionResult &result)
        {
            std::vector<std::string> rows;
            for (const auto &tuple : result.tuples)
            {
                std::string text;
                for (size_t i = 0; i < tuple.GetValueCount(); ++i)
                    text += (i ? "|" : "") + tuple.GetValue(i).ToString();
                rows.push_back(text);
            }
            return rows;
        }

        // Three replicas of one shard; each can be stopped, wiped and
        // restarted from its files
        class Shard
        {
        public:
            explicit Shard(int snapshot_distance = 0, size_t snapshot_chunk = 1 << 20, size_t size = 3)
            {
                static int counter = 0;
                root_ = (fs::temp_directory_path() /
                         ("shard_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++)))
                            .string();
                fs::remove_all(root_);
                for (size_t i = 0; i < size; ++i)
                    members_.push_back({static_cast<int>(i + 1), "127.0.0.1:" + std::to_string(FreePort())});
                for (size_t i = 0; i < size; ++i)
                {
                    ReplicatedShard::Options options;
                    options.id = members_[i].id;
                    options.port = std::stoi(members_[i].endpoint.substr(members_[i].endpoint.rfind(':') + 1));
                    options.directory = root_ + "/replica-" + std::to_string(options.id);
                    options.members = members_;
                    options.heartbeat_ms = 50;
                    // Generous: under sanitizers on a loaded machine shorter
                    // timeouts cause needless elections
                    options.election_timeout_ms = 1000;
                    options.snapshot_distance = snapshot_distance;
                    options.snapshot_chunk = snapshot_chunk;
                    options_.push_back(options);
                    replicas_.push_back(std::make_unique<ReplicatedShard>(options));
                }
            }

            ~Shard()
            {
                replicas_.clear();
                fs::remove_all(root_);
            }

            ReplicatedShard &operator[](size_t i) { return *replicas_[i]; }
            size_t Size() const { return replicas_.size(); }
            bool Running(size_t i) const { return replicas_[i] != nullptr; }

            void Stop(size_t i) { replicas_[i].reset(); }
            void Start(size_t i) { replicas_[i] = std::make_unique<ReplicatedShard>(options_[i]); }
            void Wipe(size_t i) { fs::remove_all(options_[i].directory); }

            ReplicatedShard *Leader()
            {
                ReplicatedShard *leader = nullptr;
                WaitFor(
                    [&]
                    {
                        for (auto &replica : replicas_)
                            if (replica && replica->IsLeader())
                                return (leader = replica.get()) != nullptr;
                        return false;
                    });
                return leader;
            }

            // As a client should: an uncertain failure (no leader, or a
            // leader change cancelled the request) is retried with the same
            // request id, which a shard applies at most once
            ExecutionResult Write(const std::string &sql, std::string id = "")
            {
                if (id.empty())
                    id = ReplicatedShard::NewRequestId();
                ExecutionResult result{false, "no leader", {}, {}};
                const auto deadline = std::chrono::steady_clock::now() + 20s;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    if (ReplicatedShard *leader = Leader())
                        result = leader->Write(sql, id);
                    if (result.message.find("not committed") == std::string::npos &&
                        result.message.find("not the leader") == std::string::npos && result.message != "no leader")
                        return result;
                    std::this_thread::sleep_for(50ms);
                }
                return result;
            }

            // A linearizable read through whichever replica leads
            ExecutionResult Read(const std::string &sql)
            {
                ExecutionResult result{false, "no leader", {}, {}};
                WaitFor(
                    [&]
                    {
                        ReplicatedShard *leader = Leader();
                        return leader && (result = leader->Read(sql)).success;
                    });
                return result;
            }

            // Every running replica's own data answers `sql` with `rows`
            bool AllAgree(const std::string &sql, const std::vector<std::string> &rows)
            {
                return WaitFor(
                    [&]
                    {
                        for (auto &replica : replicas_)
                            if (replica && Rows(replica->Machine().Query(sql)) != rows)
                                return false;
                        return true;
                    });
            }

        private:
            std::string root_;
            std::vector<ReplicatedShard::Member> members_;
            std::vector<ReplicatedShard::Options> options_;
            std::vector<std::unique_ptr<ReplicatedShard>> replicas_;
        };
    } // namespace

    TEST(ReplicatedShardTest, WritesReachEveryReplica)
    {
        Shard shard;
        ASSERT_NE(shard.Leader(), nullptr);
        auto created = shard.Write("CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(10));");
        ASSERT_TRUE(created.success) << created.message;
        EXPECT_EQ(created.message, "Table 't' created.");
        auto inserted = shard.Write("INSERT INTO t VALUES (1, 'ann'), (2, 'bo');");
        ASSERT_TRUE(inserted.success) << inserted.message;
        EXPECT_EQ(inserted.message, "2 row(s) inserted.");
        ASSERT_TRUE(shard.Write("UPDATE t SET name = 'cy' WHERE id = 2;").success);
        ASSERT_TRUE(shard.Write("CREATE INDEX t_name ON t (name);").success);

        auto read = shard.Leader()->Read("SELECT id, name FROM t ORDER BY id;");
        ASSERT_TRUE(read.success) << read.message;
        EXPECT_EQ(Rows(read), (std::vector<std::string>{"1|ann", "2|cy"}));
        EXPECT_TRUE(shard.AllAgree("SELECT id, name FROM t ORDER BY id;", {"1|ann", "2|cy"}));
        // Every replica applied the same entries
        EXPECT_TRUE(WaitFor(
            [&]
            {
                return shard[0].Machine().last_commit_index() == shard[1].Machine().last_commit_index() &&
                       shard[1].Machine().last_commit_index() == shard[2].Machine().last_commit_index();
            }));
    }

    TEST(ReplicatedShardTest, OnlyTheLeaderServes)
    {
        Shard shard;
        ASSERT_TRUE(shard.Write("CREATE TABLE t (a INTEGER);").success);
        ReplicatedShard *leader = shard.Leader();
        ASSERT_NE(leader, nullptr);
        for (size_t i = 0; i < shard.Size(); ++i)
        {
            if (&shard[i] == leader)
                continue;
            const auto write = shard[i].Write("INSERT INTO t VALUES (1);");
            EXPECT_FALSE(write.success);
            EXPECT_NE(write.message.find("not the leader"), std::string::npos) << write.message;
            EXPECT_FALSE(shard[i].Read("SELECT a FROM t;").success);
        }
        // Reads through the log are for queries only
        EXPECT_FALSE(leader->Read("INSERT INTO t VALUES (1);").success);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*) FROM t;", {"0"}));
    }

    TEST(ReplicatedShardTest, FailuresAreDeterministic)
    {
        Shard shard;
        ASSERT_TRUE(shard.Write("CREATE TABLE t (id INTEGER PRIMARY KEY);").success);
        ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (1);").success);
        // Fails on every replica, the same way; nothing half-applied
        const auto duplicate = shard.Write("INSERT INTO t VALUES (2), (1);");
        EXPECT_FALSE(duplicate.success);
        EXPECT_FALSE(duplicate.message.empty());
        const auto bad = shard.Write("SELECT nosuch FROM t;");
        EXPECT_FALSE(bad.success);
        EXPECT_FALSE(shard.Write("BEGIN;").success); // transactions are D6's
        // The applied index is the engine's, not SQL's to change
        const auto meta = shard.Write("UPDATE __raft_meta SET applied = 0;");
        EXPECT_FALSE(meta.success);
        EXPECT_NE(meta.message.find("belongs to the engine"), std::string::npos) << meta.message;
        EXPECT_FALSE(shard.Write("DROP TABLE __raft_meta;").success);
        ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (3);").success);
        EXPECT_TRUE(shard.AllAgree("SELECT id FROM t ORDER BY id;", {"1", "3"}));
    }

    TEST(ReplicatedShardTest, RetriedRequestsApplyOnce)
    {
        Shard shard;
        ASSERT_TRUE(shard.Write("CREATE TABLE t (a INTEGER);").success);
        // The same request twice (as after an uncertain failure): applied once,
        // and the retry gets the first outcome
        const auto first = shard.Write("INSERT INTO t VALUES (1), (2);", "request-1");
        ASSERT_TRUE(first.success) << first.message;
        const auto retry = shard.Write("INSERT INTO t VALUES (1), (2);", "request-1");
        EXPECT_TRUE(retry.success);
        EXPECT_EQ(retry.message, first.message);
        // ...also a failed one: the retry fails the same way, unapplied
        const auto bad = shard.Write("INSERT INTO nosuch VALUES (1);", "request-2");
        const auto bad_retry = shard.Write("INSERT INTO nosuch VALUES (1);", "request-2");
        EXPECT_FALSE(bad_retry.success);
        EXPECT_EQ(bad_retry.message, bad.message);
        // A new id is a new write
        ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (3);", "request-3").success);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*), SUM(a) FROM t;", {"3|6"}));
        // ...and so it stays across a full restart
        for (size_t i = 0; i < shard.Size(); ++i)
            shard.Stop(i);
        for (size_t i = 0; i < shard.Size(); ++i)
            shard.Start(i);
        EXPECT_EQ(shard.Write("INSERT INTO t VALUES (1), (2);", "request-1").message, first.message);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*), SUM(a) FROM t;", {"3|6"}));
        // The ledger is the engine's: SQL cannot change it
        EXPECT_FALSE(shard.Write("DELETE FROM __raft_requests;").success);
    }

    TEST(ReplicatedShardTest, ReadsNeedAQuorum)
    {
        Shard shard;
        {
            const auto created = shard.Write("CREATE TABLE t (a INTEGER);");
            ASSERT_TRUE(created.success) << created.message;
        }
        ReplicatedShard *leader = shard.Leader();
        ASSERT_NE(leader, nullptr);
        ASSERT_TRUE(shard.Read("SELECT COUNT(*) FROM t;").success);
        leader = shard.Leader(); // whoever leads now
        ASSERT_NE(leader, nullptr);
        // Cut off from both followers, the leader cannot prove it still
        // leads: it may not serve reads, since a new leader elsewhere could
        // already have taken writes it has not seen
        for (size_t i = 0; i < shard.Size(); ++i)
            if (&shard[i] != leader)
                shard.Stop(i);
        const auto read = leader->Read("SELECT COUNT(*) FROM t;");
        EXPECT_FALSE(read.success);
        EXPECT_FALSE(leader->Write("INSERT INTO t VALUES (1);").success);
    }

    TEST(ReplicatedShardTest, SurvivesItsLeaderAndCatchesUpOnRestart)
    {
        Shard shard;
        {
            const auto created = shard.Write("CREATE TABLE t (a INTEGER);");
            ASSERT_TRUE(created.success) << created.message;
        }
        for (int i = 0; i < 10; ++i)
            ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (" + std::to_string(i) + ");").success);
        ReplicatedShard *old_leader = shard.Leader();
        ASSERT_NE(old_leader, nullptr);
        size_t old = 0;
        while (&shard[old] != old_leader)
            ++old;
        shard.Stop(old);

        // The two survivors elect a new leader and keep taking writes
        ReplicatedShard *leader = shard.Leader();
        ASSERT_NE(leader, nullptr);
        EXPECT_NE(leader->Id(), static_cast<int>(old) + 1);
        for (int i = 10; i < 20; ++i)
            ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (" + std::to_string(i) + ");").success);
        auto count = shard.Read("SELECT COUNT(*), SUM(a) FROM t;");
        EXPECT_EQ(Rows(count), (std::vector<std::string>{"20|190"}));

        // The old leader restarts from its own files and catches up
        shard.Start(old);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*), SUM(a) FROM t;", {"20|190"}));
    }

    TEST(ReplicatedShardTest, RestartsApplyNothingTwice)
    {
        Shard shard;
        // No primary key: a replayed INSERT would show up as an extra row
        {
            const auto created = shard.Write("CREATE TABLE t (a INTEGER);");
            ASSERT_TRUE(created.success) << created.message;
        }
        for (int i = 0; i < 15; ++i)
            ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (1);").success);
        ASSERT_TRUE(shard.AllAgree("SELECT COUNT(*) FROM t;", {"15"}));
        // Every replica down at once, then back from disk
        for (size_t i = 0; i < shard.Size(); ++i)
            shard.Stop(i);
        for (size_t i = 0; i < shard.Size(); ++i)
            shard.Start(i);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*) FROM t;", {"15"}));
        ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (2);").success);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*), SUM(a) FROM t;", {"16|17"}));
    }

    TEST(ReplicatedShardTest, ALostReplicaIsRebuiltFromASnapshot)
    {
        // Small pieces: the snapshot (a few pages) crosses in many of them
        Shard shard(/*snapshot_distance=*/10, /*snapshot_chunk=*/1000);
        {
            const auto created = shard.Write("CREATE TABLE t (a INTEGER);");
            ASSERT_TRUE(created.success) << created.message;
        }
        for (int i = 0; i < 60; ++i)
            ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (" + std::to_string(i) + ");").success);
        ASSERT_TRUE(shard.AllAgree("SELECT COUNT(*) FROM t;", {"60"}));
        // The leader compacted its log behind its snapshots
        ReplicatedShard *leader = shard.Leader();
        ASSERT_NE(leader, nullptr);
        ASSERT_NE(leader->Machine().last_snapshot(), nullptr);
        EXPECT_GT(leader->Raft()->get_log_store()->start_index(), 1u);

        // A replica loses its disk: it can only come back from a snapshot
        size_t lost = 0;
        while (&shard[lost] == leader)
            ++lost;
        shard.Stop(lost);
        shard.Wipe(lost);
        for (int i = 60; i < 70; ++i)
            ASSERT_TRUE(shard.Write("INSERT INTO t VALUES (" + std::to_string(i) + ");").success);
        shard.Start(lost);
        EXPECT_TRUE(shard.AllAgree("SELECT COUNT(*), SUM(a) FROM t;", {"70|2415"}));
        EXPECT_NE(shard[lost].Machine().last_snapshot(), nullptr);
    }

    TEST(ReplicatedShardTest, ConcurrentWritersAgree)
    {
        Shard shard;
        ASSERT_TRUE(shard.Write("CREATE TABLE t (writer INTEGER, n INTEGER);").success);
        std::vector<std::thread> writers;
        std::atomic<int> failures{0};
        for (int w = 0; w < 4; ++w)
            writers.emplace_back(
                [&, w]
                {
                    for (int n = 0; n < 25; ++n)
                        if (!shard.Write("INSERT INTO t VALUES (" + std::to_string(w) + ", " + std::to_string(n) +
                                         ");")
                                 .success)
                            ++failures;
                });
        for (auto &writer : writers)
            writer.join();
        EXPECT_EQ(failures, 0);
        EXPECT_TRUE(shard.AllAgree("SELECT writer, COUNT(*), SUM(n) FROM t GROUP BY writer ORDER BY writer;",
                                   {"0|25|300", "1|25|300", "2|25|300", "3|25|300"}));
    }

} // namespace sql::cluster
