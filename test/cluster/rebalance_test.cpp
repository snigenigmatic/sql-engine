#include <gtest/gtest.h>
#include "test_cluster.h"
#include <atomic>
#include <thread>

// D8: replicas move when storage nodes join, are taken out, or go down,
// while the data stays readable and writable throughout

namespace sql::cluster
{

    namespace
    {
        using namespace std::chrono_literals;
        using testing::Cluster;
        using testing::SHARDS;
        using testing::WaitFor;

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

        // Shards whose replicas include node `id`
        size_t ShardsOn(Coordinator &leader, int id)
        {
            size_t count = 0;
            for (const auto &[shard, nodes] : leader.Replicas())
                count += std::count(nodes.begin(), nodes.end(), id);
            return count;
        }

        bool Balanced(net::RemoteConnection &client)
        {
            const std::string status = client.Command("cluster", "");
            return status.find("Balanced") != std::string::npos;
        }

        // Inserts rows 1000, 1001, ... until stopped; counts the successes
        struct Writer
        {
            std::atomic<bool> stop{false};
            std::atomic<int> written{0}, failed{0};
            std::thread thread;

            explicit Writer(Cluster &cluster)
            {
                thread = std::thread(
                    [this, &cluster]
                    {
                        auto client = cluster.Client(1);
                        for (int id = 1000; !stop; ++id)
                        {
                            const auto result =
                                client->Execute("INSERT INTO t VALUES (" + std::to_string(id) + ", 1);");
                            if (result.success)
                                ++written;
                            else
                                ++failed;
                        }
                    });
            }
            ~Writer() { Stop(); }
            void Stop()
            {
                stop = true;
                if (thread.joinable())
                    thread.join();
            }
        };
    } // namespace

    TEST(RebalanceTest, ANewNodeGetsReplicasWhileWritesGoOn)
    {
        // Small snapshot distance: a new replica catches up from a snapshot
        Cluster cluster(3, 60000, 300000, /*snapshot_distance=*/20);
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        std::string rows;
        for (int i = 1; i <= 200; ++i)
            rows += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", 1)";
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES " + rows + ";");
        for (int i = 201; i <= 260; ++i) // enough log entries for snapshots
            RunAll(*client, "INSERT INTO t VALUES (" + std::to_string(i) + ", 1);");
        EXPECT_TRUE(Balanced(*client)) << client->Command("cluster", "");

        Writer writer(cluster);
        cluster.AddNode(); // node-3
        ASSERT_TRUE(WaitFor([&] { return cluster.Leader()->NodeIdOf("node-3") > 0; }));
        const int added = cluster.Leader()->NodeIdOf("node-3");
        ASSERT_TRUE(WaitFor([&] { return ShardsOn(*cluster.Leader(), added) > 0 && Balanced(*client); }, 90s))
            << client->Command("cluster", "");
        writer.Stop();
        EXPECT_GT(writer.written.load(), 0);

        // Every shard still has 3 replicas; the new node holds some of them
        for (const auto &[shard, nodes] : cluster.Leader()->Replicas())
            EXPECT_EQ(nodes.size(), 3u) << "shard " << shard;
        EXPECT_GE(ShardsOn(*cluster.Leader(), added), 1u);
        // No row was lost or duplicated
        const std::string expected = std::to_string(260 + writer.written.load());
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{expected}));
        // Each node holds exactly the replicas it is listed for, with the data
        for (const auto &[shard, nodes] : cluster.Leader()->Replicas())
            for (size_t n = 0; n < cluster.NodeCount(); ++n)
            {
                const int id = cluster.Leader()->NodeIdOf("node-" + std::to_string(n));
                const bool member = std::count(nodes.begin(), nodes.end(), id) > 0;
                EXPECT_TRUE(WaitFor([&] { return (cluster.Node(n).Replica(shard) != nullptr) == member; }))
                    << "node-" << n << " shard " << shard;
            }
        const auto replicas = cluster.Leader()->Replicas();
        for (const auto &[shard, nodes] : replicas)
            if (std::count(nodes.begin(), nodes.end(), added))
            {
                // The new replica has the shard's rows: compare with another member
                const int other = nodes[0] == added ? nodes[1] : nodes[0];
                size_t other_index = 0;
                for (size_t n = 0; n < cluster.NodeCount(); ++n)
                    if (cluster.Leader()->NodeIdOf("node-" + std::to_string(n)) == other)
                        other_index = n;
                EXPECT_TRUE(WaitFor([&] {
                    return Rows(cluster.ReplicaOf(3, shard).Machine().Query("SELECT COUNT(*) FROM t;")) ==
                           Rows(cluster.ReplicaOf(other_index, shard).Machine().Query("SELECT COUNT(*) FROM t;"));
                })) << "shard " << shard;
            }
    }

    TEST(RebalanceTest, ARemovedNodeIsDrained)
    {
        Cluster cluster(4);
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(2);
        ASSERT_TRUE(WaitFor([&] { return Balanced(*client); }, 60s)) << client->Command("cluster", "");
        std::string rows;
        for (int i = 1; i <= 100; ++i)
            rows += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", " + std::to_string(i) + ")";
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER); INSERT INTO t VALUES " + rows + ";");

        const int gone = cluster.Leader()->NodeIdOf("node-1");
        ASSERT_GT(ShardsOn(*cluster.Leader(), gone), 0u);
        auto removing = client->Command("cluster", "remove node-1");
        EXPECT_NE(removing.find("Removing node-1"), std::string::npos) << removing;
        ASSERT_TRUE(WaitFor([&] { return ShardsOn(*cluster.Leader(), gone) == 0 && Balanced(*client); }, 90s))
            << client->Command("cluster", "");
        for (const auto &[shard, nodes] : cluster.Leader()->Replicas())
            EXPECT_EQ(nodes.size(), 3u) << "shard " << shard;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"100|5050"}));
        // Its copies are deleted, and it is shown as removed
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_TRUE(WaitFor([&] { return cluster.Node(1).Replica(s) == nullptr; })) << s;
        EXPECT_NE(client->Command("cluster", "").find("node-1 (id " + std::to_string(gone) + ")"), std::string::npos);
        EXPECT_NE(client->Command("cluster", "").find("removed"), std::string::npos);
        // The replication factor cannot be given up
        auto refused = client->Command("cluster", "remove node-0");
        EXPECT_NE(refused.find("only 2 active node(s) would remain"), std::string::npos) << refused;
        EXPECT_NE(client->Command("cluster", "remove nosuch").find("no storage node named nosuch"), std::string::npos);
        EXPECT_TRUE(client->Execute("INSERT INTO t VALUES (101, 101);").success);
        // Let back in (still running, it announces itself): it gets replicas again
        EXPECT_NE(client->Command("cluster", "add node-1").find("may join again"), std::string::npos);
        EXPECT_NE(client->Command("cluster", "add node-1").find("already a member"), std::string::npos);
        ASSERT_TRUE(WaitFor([&] { return ShardsOn(*cluster.Leader(), gone) > 0 && Balanced(*client); }, 90s))
            << client->Command("cluster", "");
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"101|5151"}));
    }

    TEST(RebalanceTest, ADownNodesReplicasArePlacedElsewhere)
    {
        Cluster cluster(4, 60000, /*node_down_after_ms=*/3000);
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        ASSERT_TRUE(WaitFor([&] { return Balanced(*client); }, 60s)) << client->Command("cluster", "");
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);"
                        "INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50), (6, 60), (7, 70), (8, 80);");
        const int down = cluster.Leader()->NodeIdOf("node-2");
        ASSERT_GT(ShardsOn(*cluster.Leader(), down), 0u);
        cluster.StopNode(2);
        // Writes go on meanwhile (each shard keeps a quorum)
        ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (9, 90);").success);
        ASSERT_TRUE(WaitFor([&] { return ShardsOn(*cluster.Leader(), down) == 0 && Balanced(*client); }, 90s))
            << client->Command("cluster", "");
        EXPECT_NE(client->Command("cluster", "").find("down"), std::string::npos);
        for (const auto &[shard, nodes] : cluster.Leader()->Replicas())
            EXPECT_EQ(nodes.size(), 3u) << "shard " << shard;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"9|450"}));
        // Each shard has 3 working replicas again: losing one more is fine
        cluster.StopNode(0);
        ExecutionResult after;
        ASSERT_TRUE(WaitFor([&] { return (after = client->Execute("INSERT INTO t VALUES (10, 100);")).success; }))
            << after.message;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"10|550"}));
    }

} // namespace sql::cluster
