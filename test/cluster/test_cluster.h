#pragma once

// A whole cluster in one process (3 coordinators, some storage nodes) over
// real TCP and Raft, for tests

#include "cluster/node/coordinator.h"
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

namespace sql::cluster::testing
{

    using namespace std::chrono_literals;

    inline bool PortFree(int port)
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
    inline int FreePorts(int count)
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

    inline bool WaitFor(const std::function<bool()> &condition, std::chrono::milliseconds limit = 30s)
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

    constexpr uint32_t SHARDS = 4;

    class Cluster
    {
    public:
        explicit Cluster(size_t storage_nodes = 3, int idle_transaction_timeout_ms = 60000)
        {
            static int counter = 0;
            root_ = (std::filesystem::temp_directory_path() /
                     ("cluster_test_" + std::to_string(getpid()) + "_" + std::to_string(counter++)))
                        .string();
            std::filesystem::remove_all(root_);
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
                options.idle_transaction_timeout_ms = idle_transaction_timeout_ms;
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
            std::filesystem::remove_all(root_);
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


} // namespace sql::cluster::testing
