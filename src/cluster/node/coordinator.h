#pragma once

#include "catalog/database.h"
#include "cluster/shard/replicated_shard.h"
#include "net/remote_connection.h"
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

    // A coordinator. The coordinators form a Raft group (a ReplicatedShard)
    // whose database holds the cluster's metadata:
    //
    //   cluster   (shards, rf, epoch, formed)
    //   nodes     (id, name, host, port, raft_base)     storage nodes
    //   replicas  (shard, node)                          fixed at formation
    //   tables    (name, definition, kind, shard_column) kind: hash|row|replicated
    //   ddl       (seq, statement)                       every schema change, in order
    //   shard_ddl (shard, applied)                       how far each shard got
    //
    // The group's leader is the cluster's coordinator: it accepts storage
    // nodes, forms the cluster once `replication_factor` nodes have joined
    // (placing every shard's replicas with Placement and creating their
    // Raft groups), keeps every shard's schema up to date from the ddl log,
    // and runs clients' SQL:
    //
    //   DDL               validated, appended to ddl, applied to every shard
    //   INSERT            each row to its shard (by key hash; replicated
    //                     tables to every shard)
    //   UPDATE / DELETE   to every shard; row counts are summed
    //   SELECT / EXPLAIN  the tables it names are gathered from the shards
    //                     into an in-memory database, which runs it (D5 adds
    //                     pruning and pushdown)
    //
    // Becoming leader bumps the epoch, which storage nodes use to refuse a
    // deposed leader. Followers redirect clients to the leader.
    class Coordinator
    {
    public:
        struct Options
        {
            int id = 1;
            std::string host = "127.0.0.1";
            uint16_t client_port = 0; // 0: any free port
            int raft_port = 0;
            std::string directory;
            std::vector<ReplicatedShard::Member> peers;    // every coordinator's Raft endpoint
            std::map<int, std::string> client_addresses;  // coordinator id -> host:client_port
            uint32_t shard_count = 16;
            size_t replication_factor = 3;
            int heartbeat_ms = 100;
            int election_timeout_ms = 1000;
            int snapshot_distance = 1000;
        };

        explicit Coordinator(const Options &options);
        ~Coordinator();

        void Start();
        void Stop();
        uint16_t Port() const { return server_ ? server_->Port() : 0; }
        bool IsLeader() const { return metadata_ && metadata_->IsLeader(); }
        // host:client_port of the current leader, or "" if unknown
        std::string LeaderAddress() const;

        // The client-facing operations (on the leader)
        ExecutionResult Execute(const std::string &sql);
        std::string Command(const std::string &name, const std::string &argument);
        ExecutionResult Call(const std::string &method, const std::vector<std::string> &arguments);

        // For tests and status: as last loaded by the leader
        bool Formed();
        size_t NodeCount();
        uint64_t Epoch();

    private:
        struct NodeInfo
        {
            int id = 0;
            std::string name, host;
            uint16_t port = 0;
            int raft_base = 0;
        };
        struct TableInfo
        {
            std::string name, definition, kind, shard_column;
        };
        struct View
        {
            uint32_t shards = 0;
            size_t rf = 0;
            uint64_t epoch = 0;
            bool formed = false;
            std::map<int, NodeInfo> nodes;
            std::map<uint32_t, std::vector<int>> replicas;
            std::map<std::string, TableInfo> tables;
            std::vector<std::pair<int64_t, std::string>> ddl;
            std::map<uint32_t, int64_t> shard_ddl;
        };

        // All below: caller holds op_mutex_, on the leader
        void Loop();
        void BecomeLeader();
        void Reconcile();
        void Load();
        ExecutionResult MetaWrite(const std::string &sql);
        ExecutionResult MetaRead(const std::string &sql);
        void Form();
        void EnsureReplicas(bool force);
        void CatchUpDdl();
        ExecutionResult ShardCall(uint32_t shard, const std::string &method, const std::string &sql);
        net::RemoteConnection *NodeConnection(int node);
        std::unique_ptr<Database> SchemaDatabase(); // every table and index, empty
        ExecutionResult Ddl(const std::string &sql);
        ExecutionResult Insert(const std::string &sql);
        ExecutionResult Broadcast(const std::string &sql, const std::string &table);
        ExecutionResult Select(const std::string &sql);
        ExecutionResult Join(const std::vector<std::string> &arguments);
        std::vector<uint32_t> AllShards() const;

        Options options_;
        std::unique_ptr<ReplicatedShard> metadata_;
        std::unique_ptr<Server> server_;
        std::thread loop_;
        std::atomic<bool> stopping_{false};

        std::mutex op_mutex_; // one cluster operation at a time
        bool was_leader_ = false;
        View view_;
        std::map<int, std::unique_ptr<net::RemoteConnection>> connections_;
        std::map<uint32_t, int> shard_leaders_;
        std::map<std::pair<uint32_t, int>, bool> ensured_;
        std::chrono::steady_clock::time_point last_full_ensure_{};
    };

} // namespace sql::cluster
