#include <gtest/gtest.h>
#include "cli/local_connection.h"
#include "cluster/node/coordinator.h"
#include "cluster/node/script.h"
#include "cluster/node/storage_node.h"
#include "net/remote_connection.h"
#include <arpa/inet.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <netinet/in.h>
#include <random>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// D4: a whole cluster in one process (3 coordinators, 3 storage nodes) over
// real TCP and Raft.

namespace sql::cluster
{

    namespace
    {
        using namespace std::chrono_literals;

        bool PortFree(int port)
        {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(static_cast<uint16_t>(port));
            const bool free = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
            close(fd);
            return free;
        }

        // `count` consecutive free ports, never handed out before
        int FreePorts(int count)
        {
            static std::set<int> taken;
            static std::mt19937 random(static_cast<unsigned>(getpid()));
            for (;;)
            {
                const int base = 20000 + static_cast<int>(random() % 30000);
                bool ok = true;
                for (int p = base; p < base + count && ok; ++p)
                    ok = !taken.count(p) && PortFree(p);
                if (!ok)
                    continue;
                for (int p = base; p < base + count; ++p)
                    taken.insert(p);
                return base;
            }
        }

        bool WaitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = 30s)
        {
            const auto deadline = std::chrono::steady_clock::now() + limit;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (condition())
                    return true;
                std::this_thread::sleep_for(50ms);
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
                    text += (i ? "|" : "") + (tuple.GetValue(i).IsNull() ? "NULL" : tuple.GetValue(i).ToString());
                rows.push_back(text);
            }
            return rows;
        }

        constexpr uint32_t SHARDS = 4;

        class Cluster
        {
        public:
            explicit Cluster(size_t storage_nodes = 3)
            {
                static int counter = 0;
                root_ = (fs::temp_directory_path() /
                         ("cluster_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++)))
                            .string();
                fs::remove_all(root_);
                std::vector<ReplicatedShard::Member> peers;
                std::map<int, std::string> clients;
                for (int id = 1; id <= 3; ++id)
                {
                    const int base = FreePorts(2);
                    peers.push_back({id, "127.0.0.1:" + std::to_string(base)});
                    clients[id] = "127.0.0.1:" + std::to_string(base + 1);
                }
                for (int id = 1; id <= 3; ++id)
                {
                    Coordinator::Options options;
                    options.id = id;
                    options.raft_port = std::stoi(peers[id - 1].endpoint.substr(10));
                    options.client_port = static_cast<uint16_t>(std::stoi(clients[id].substr(10)));
                    options.directory = root_ + "/coordinator-" + std::to_string(id);
                    options.peers = peers;
                    options.client_addresses = clients;
                    options.shard_count = SHARDS;
                    options.replication_factor = 3;
                    options.heartbeat_ms = 50;
                    options.election_timeout_ms = 1000; // shorter ones cause needless elections under sanitizers
                    coordinator_options_.push_back(options);
                    coordinator_addresses_.push_back(clients[id]);
                }
                for (auto &options : coordinator_options_)
                    StartCoordinator(options.id - 1);
                for (size_t i = 0; i < storage_nodes; ++i)
                    AddNode();
            }

            ~Cluster()
            {
                nodes_.clear();
                coordinators_.clear();
                fs::remove_all(root_);
            }

            void StartCoordinator(size_t i)
            {
                if (coordinators_.size() <= i)
                    coordinators_.resize(i + 1);
                coordinators_[i] = std::make_unique<Coordinator>(coordinator_options_[i]);
                coordinators_[i]->Start();
            }
            void StopCoordinator(size_t i) { coordinators_[i].reset(); }

            StorageNode &AddNode()
            {
                StorageNode::Options options;
                options.name = "node-" + std::to_string(node_options_.size());
                options.raft_port_base = FreePorts(static_cast<int>(SHARDS));
                options.directory = root_ + "/" + options.name;
                options.coordinators = coordinator_addresses_;
                options.heartbeat_ms = 50;
                options.election_timeout_ms = 1000; // shorter ones cause needless elections under sanitizers
                node_options_.push_back(options);
                nodes_.push_back(std::make_unique<StorageNode>(options));
                nodes_.back()->Start();
                return *nodes_.back();
            }
            void StopNode(size_t i) { nodes_[i].reset(); }
            StorageNode &Node(size_t i) { return *nodes_[i]; }

            Coordinator *Leader()
            {
                Coordinator *leader = nullptr;
                WaitFor(
                    [&]
                    {
                        for (auto &c : coordinators_)
                            if (c && c->IsLeader())
                                return (leader = c.get()) != nullptr;
                        return false;
                    });
                return leader;
            }

            bool WaitFormed()
            {
                return WaitFor([&] { return Leader() && Leader()->Formed(); });
            }

            // A client of coordinator i (a follower forwards to the leader)
            std::unique_ptr<net::RemoteConnection> Client(size_t i)
            {
                const std::string &address = coordinator_addresses_[i];
                return std::make_unique<net::RemoteConnection>("127.0.0.1",
                                                               static_cast<uint16_t>(std::stoi(address.substr(10))));
            }

        private:
            std::string root_;
            std::vector<Coordinator::Options> coordinator_options_;
            std::vector<std::string> coordinator_addresses_;
            std::vector<std::unique_ptr<Coordinator>> coordinators_;
            std::vector<StorageNode::Options> node_options_;
            std::vector<std::unique_ptr<StorageNode>> nodes_;
        };

        // Runs every statement, failing the test on the first failure
        void RunAll(Connection &client, const std::string &script)
        {
            for (const auto &statement : SplitStatements(script))
            {
                const auto result = client.Execute(statement);
                ASSERT_TRUE(result.success) << statement << "\n  " << result.message;
            }
        }
    } // namespace

    TEST(ScriptTest, SplitsAtSemicolonsOutsideStrings)
    {
        EXPECT_EQ(SplitStatements("SELECT 1;  INSERT INTO t VALUES ('a;b');\n\nSELECT\n x FROM t;"),
                  (std::vector<std::string>{"SELECT 1;", "INSERT INTO t VALUES ('a;b');", "SELECT\n x FROM t;"}));
        EXPECT_EQ(SplitStatements("SELECT 1; SELECT 2"), (std::vector<std::string>{"SELECT 1;", "SELECT 2"}));
        EXPECT_TRUE(SplitStatements("  \n ").empty());
        EXPECT_EQ(SplitStatements(";;SELECT 1;"), (std::vector<std::string>{"SELECT 1;"}));
    }

    TEST(ClusterTest, FormsAndRunsSql)
    {
        auto db = Database::OpenInMemory();
        Session session(db.get());
        LocalConnection local(db.get(), &session); // the same statements on one node
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        EXPECT_EQ(cluster.Leader()->NodeCount(), 3u);
        auto client = cluster.Client(0); // whichever coordinator this is
        EXPECT_NE(client->Command("describe", "").find("3 storage node(s), 4 shards x 3 replicas"), std::string::npos)
            << client->Command("describe", "");

        const std::string schema = "CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR(20) NOT NULL, age INTEGER);"
                                   "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, amount FLOAT);"
                                   "CREATE INDEX orders_user ON orders (user_id);";
        RunAll(*client, schema);
        RunAll(local, schema);
        std::string users = "INSERT INTO users VALUES ", orders = "INSERT INTO orders VALUES ";
        for (int i = 1; i <= 40; ++i)
        {
            users += (i > 1 ? ", (" : "(") + std::to_string(i) + ", 'u" + std::to_string(i) + "', " +
                     std::to_string(20 + i % 30) + ")";
            orders += (i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(1 + i % 10) + ", " +
                      std::to_string(i) + ".5)";
        }
        auto inserted = client->Execute(users + ";");
        ASSERT_TRUE(inserted.success) << inserted.message;
        EXPECT_EQ(inserted.message, "40 row(s) inserted.");
        ASSERT_TRUE(client->Execute(orders + ";").success);
        RunAll(local, users + ";" + orders + ";");

        // Every node holds some of the rows: they were spread over shards
        for (size_t n = 0; n < 3; ++n)
        {
            long long held = 0;
            for (uint32_t s = 0; s < SHARDS; ++s)
                if (auto *replica = cluster.Node(n).Replica(s))
                    held += replica->Machine().Query("SELECT COUNT(*) FROM users;").tuples.at(0).GetValue(0).GetAsInt();
            EXPECT_EQ(held, 40) << "node " << n; // rf 3 of 3 nodes: every node has every shard
        }
        size_t shards_with_rows = 0;
        for (uint32_t s = 0; s < SHARDS; ++s)
            if (cluster.Node(0).Replica(s)->Machine().Query("SELECT COUNT(*) FROM users;").tuples.at(0).GetValue(0).GetAsInt() > 0)
                ++shards_with_rows;
        EXPECT_GT(shards_with_rows, 1u);

        // Every statement: the same outcome as on one node
        for (const std::string sql : {
                 "SELECT COUNT(*), SUM(age) FROM users;",
                 "SELECT u.name, SUM(o.amount) FROM users u JOIN orders o ON o.user_id = u.id "
                 "GROUP BY u.name ORDER BY u.name LIMIT 3;",
                 "SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) > 1 ORDER BY age;",
                 "SELECT name FROM users WHERE id IN (SELECT user_id FROM orders WHERE amount > 35.0) ORDER BY name;",
                 "UPDATE users SET age = age + 1 WHERE id <= 10;",
                 "DELETE FROM orders WHERE amount > 30.0;",
                 "SELECT COUNT(*), MIN(amount), MAX(amount) FROM orders;",
                 "SELECT id, age FROM users WHERE id <= 3 ORDER BY id;",
             })
        {
            const auto a = client->Execute(sql), b = local.Execute(sql);
            EXPECT_TRUE(a.success) << sql << "\n  " << a.message;
            EXPECT_EQ(a.message, b.message) << sql;
            EXPECT_EQ(a.column_names, b.column_names) << sql;
            EXPECT_EQ(Rows(a), Rows(b)) << sql;
        }
    }

    TEST(ClusterTest, ErrorsMatchASingleNode)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(1);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(5) NOT NULL);");

        auto db = Database::OpenInMemory();
        Session session(db.get());
        LocalConnection local(db.get(), &session);
        RunAll(local, "CREATE TABLE t (id INTEGER PRIMARY KEY, name VARCHAR(5) NOT NULL);");
        for (const std::string sql : {
                 "CREATE TABLE t (a INTEGER);",
                 "INSERT INTO t VALUES (1, NULL);",
                 "INSERT INTO t VALUES (1, 'toolongname');",
                 "INSERT INTO t VALUES (1);",
                 "INSERT INTO nosuch VALUES (1);",
                 "SELECT nosuch FROM t;",
                 "UPDATE t SET nosuch = 1;",
                 "DROP TABLE nosuch;",
                 "SELEC 1;",
             })
        {
            // The same outcome and message (UPDATE of a missing column on
            // an empty table succeeds on both: no row is evaluated)
            const auto a = client->Execute(sql), b = local.Execute(sql);
            EXPECT_EQ(a.success, b.success) << sql;
            EXPECT_EQ(a.message, b.message) << sql;
        }
        // Across shards: a duplicate key lands on the shard that has it
        ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (1, 'a');").success);
        auto duplicate = client->Execute("INSERT INTO t VALUES (1, 'b');");
        EXPECT_FALSE(duplicate.success);
        RunAll(local, "INSERT INTO t VALUES (1, 'a');");
        EXPECT_EQ(duplicate.message, local.Execute("INSERT INTO t VALUES (1, 'b');").message);
        // The shard key cannot change; transactions are not distributed yet
        EXPECT_FALSE(client->Execute("UPDATE t SET id = 2 WHERE id = 1;").success);
        EXPECT_FALSE(client->Execute("BEGIN;").success);
    }

    TEST(ClusterTest, TableKinds)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(2);
        RunAll(*client, "CREATE TABLE countries (code VARCHAR(2) PRIMARY KEY, name VARCHAR(20)) REPLICATED;"
                     "CREATE TABLE events (country VARCHAR(2), kind VARCHAR(10)) SHARD BY HASH (country);"
                     "CREATE TABLE log (line VARCHAR(20));" // no key: placed by the whole row
                     "INSERT INTO countries VALUES ('in', 'India'), ('fr', 'France');"
                     "INSERT INTO events VALUES ('in', 'a'), ('in', 'b'), ('fr', 'c'), ('in', 'd');"
                     "INSERT INTO log VALUES ('x'), ('x'), ('y');");
        // A replicated table is whole on every shard; counted once
        for (uint32_t s = 0; s < SHARDS; ++s)
            EXPECT_EQ(Rows(cluster.Node(0).Replica(s)->Machine().Query("SELECT COUNT(*) FROM countries;")),
                      (std::vector<std::string>{"2"}));
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM countries;")), (std::vector<std::string>{"2"}));
        EXPECT_EQ(client->Execute("UPDATE countries SET name = 'Bharat' WHERE code = 'in';").message,
                  "1 row(s) updated.");
        // Rows with the same shard key share a shard
        size_t shards_with_in = 0;
        for (uint32_t s = 0; s < SHARDS; ++s)
            if (Rows(cluster.Node(0).Replica(s)->Machine().Query("SELECT COUNT(*) FROM events WHERE country = 'in';")) !=
                std::vector<std::string>{"0"})
                ++shards_with_in;
        EXPECT_EQ(shards_with_in, 1u);
        EXPECT_EQ(Rows(client->Execute("SELECT c.name, COUNT(*) FROM events e JOIN countries c ON c.code = e.country "
                                       "GROUP BY c.name ORDER BY c.name;")),
                  (std::vector<std::string>{"Bharat|3", "France|1"}));
        EXPECT_EQ(Rows(client->Execute("SELECT line, COUNT(*) FROM log GROUP BY line ORDER BY line;")),
                  (std::vector<std::string>{"x|2", "y|1"}));
        auto tables = client->Command("tables", "");
        EXPECT_NE(tables.find("countries [replicated]"), std::string::npos) << tables;
        EXPECT_NE(tables.find("events [by country]"), std::string::npos) << tables;
        EXPECT_NE(tables.find("log [by row]"), std::string::npos) << tables;
    }

    TEST(ClusterTest, SchemaChangesReachShardsThatFormLater)
    {
        Cluster cluster(/*storage_nodes=*/2); // one short of the replication factor
        ASSERT_NE(cluster.Leader(), nullptr);
        auto client = cluster.Client(0);
        ASSERT_TRUE(WaitFor([&] { return cluster.Leader()->NodeCount() == 2; }));
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY);");
        auto early = client->Execute("INSERT INTO t VALUES (1);");
        EXPECT_FALSE(early.success);
        EXPECT_NE(early.message.find("still forming: 2 of 3"), std::string::npos) << early.message;
        cluster.AddNode();
        ASSERT_TRUE(cluster.WaitFormed());
        // The table created before the shards existed was created on them
        auto inserted = client->Execute("INSERT INTO t VALUES (1), (2), (3);");
        ASSERT_TRUE(inserted.success) << inserted.message;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"3"}));
    }

    TEST(ClusterTest, SurvivesLosingAStorageNodeAndTheLeadingCoordinator)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY, v INTEGER);");
        ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, 40);").success);

        cluster.StopNode(2); // every shard keeps 2 of its 3 replicas
        auto inserted = client->Execute("INSERT INTO t VALUES (5, 50), (6, 60);");
        ASSERT_TRUE(inserted.success) << inserted.message;
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"6|210"}));

        // The leading coordinator goes; another takes over with a newer epoch
        Coordinator *leader = cluster.Leader();
        ASSERT_NE(leader, nullptr);
        const uint64_t epoch = leader->Epoch();
        size_t index = 0;
        for (size_t i = 0; i < 3; ++i)
        {
            auto probe = cluster.Client(i);
            if (probe->Command("describe", "").find("coordinator " + std::to_string(i + 1) + " (leader)") !=
                std::string::npos)
                index = i;
        }
        cluster.StopCoordinator(index);
        auto survivor = cluster.Client((index + 1) % 3);
        ASSERT_TRUE(WaitFor([&] { return survivor->Execute("SELECT COUNT(*) FROM t;").success; }));
        EXPECT_GT(cluster.Leader()->Epoch(), epoch);
        auto after = survivor->Execute("INSERT INTO t VALUES (7, 70);");
        ASSERT_TRUE(after.success) << after.message;
        EXPECT_EQ(Rows(survivor->Execute("SELECT COUNT(*), SUM(v) FROM t;")), (std::vector<std::string>{"7|280"}));
    }

    TEST(ClusterTest, StorageNodesRefuseADeposedCoordinator)
    {
        Cluster cluster;
        ASSERT_TRUE(cluster.WaitFormed());
        auto client = cluster.Client(0);
        RunAll(*client, "CREATE TABLE t (id INTEGER PRIMARY KEY);");
        ASSERT_TRUE(client->Execute("INSERT INTO t VALUES (1);").success);
        const uint64_t epoch = cluster.Leader()->Epoch();
        // A request from an older leader (an older epoch) changes nothing
        for (uint32_t s = 0; s < SHARDS; ++s)
        {
            auto stale = cluster.Node(0).Handle("write", {std::to_string(epoch - 1), std::to_string(s),
                                                          "INSERT INTO t VALUES (99);"});
            EXPECT_FALSE(stale.success);
            EXPECT_NE(stale.message.find("stale epoch"), std::string::npos) << stale.message;
        }
        EXPECT_EQ(Rows(client->Execute("SELECT COUNT(*) FROM t;")), (std::vector<std::string>{"1"}));
    }

} // namespace sql::cluster
