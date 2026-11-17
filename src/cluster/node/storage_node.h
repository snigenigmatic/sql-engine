#pragma once

#include "cluster/shard/replicated_shard.h"
#include "server/server.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sql::cluster
{

    // A storage node: holds replicas of some shards (each a ReplicatedShard
    // with its own Raft port) and answers its coordinator over the wire
    // protocol's CALLs:
    //
    //   ensure_replica  epoch, shard, members ("id@host:port,...")
    //   write           epoch, shard, sql   (on the shard's leader)
    //   read            epoch, shard, sql   (linearizable, on the leader)
    //
    // A request carrying an older epoch than one already seen is refused:
    // after a coordinator failover the deposed leader cannot change data.
    // A node joins the cluster by calling "join" on the coordinators until
    // the leader accepts, and keeps re-announcing itself.
    class StorageNode
    {
    public:
        struct Options
        {
            std::string name;                     // stable across restarts, e.g. node-0
            std::string host = "127.0.0.1";       // how others reach this node
            uint16_t port = 0;                    // its CALL port (0: any free)
            int raft_port_base = 20000;           // shard s's replica listens on base + s
            std::string directory;                // its replicas' files
            std::vector<std::string> coordinators; // host:port of each coordinator
            int heartbeat_ms = 100;
            int election_timeout_ms = 1000;
            int snapshot_distance = 1000;
        };

        explicit StorageNode(const Options &options);
        ~StorageNode();

        void Start();
        void Stop();
        uint16_t Port() const { return server_ ? server_->Port() : 0; }
        int NodeId() const { return node_id_; }

        // For tests: this node's replica of a shard, if it holds one
        ReplicatedShard *Replica(uint32_t shard);

        ExecutionResult Handle(const std::string &method, const std::vector<std::string> &arguments);

    private:
        void Announce(); // join loop
        bool CheckEpoch(const std::string &epoch, ExecutionResult *refusal);

        Options options_;
        std::unique_ptr<Server> server_;
        std::mutex mutex_;
        std::map<uint32_t, std::unique_ptr<ReplicatedShard>> replicas_;
        std::atomic<uint64_t> epoch_{0};
        std::atomic<int> node_id_{-1};
        std::atomic<bool> stopping_{false};
        std::thread announcer_;
    };

    // "id@host:port,..." <-> members
    std::string EncodeMembers(const std::vector<ReplicatedShard::Member> &members);
    std::vector<ReplicatedShard::Member> DecodeMembers(const std::string &text);

    // The leader id in a "not the leader (leader: N)" refusal, or -1
    int LeaderHint(const std::string &message);

} // namespace sql::cluster
