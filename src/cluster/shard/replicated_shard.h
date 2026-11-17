#pragma once

#include "cluster/raft/nuraft.h"
#include "cluster/shard/file_state_manager.h"
#include "cluster/shard/shard_state_machine.h"
#include "execution/executor.h"
#include <memory>
#include <string>
#include <vector>

namespace sql::cluster
{

    // One replica of a shard, served over Raft: a NuRaft server with a
    // durable log (FileStateManager) applying entries to a database
    // (ShardStateMachine). Every replica of a shard starts with the same
    // member list and they elect a leader among themselves.
    class ReplicatedShard
    {
    public:
        struct Member
        {
            int id;
            std::string endpoint; // host:port of its Raft listener
        };

        struct Options
        {
            int id = 0;                 // this replica's member id
            int port = 0;               // its Raft port
            std::string directory;      // its log, state and data
            std::vector<Member> members; // every replica, this one included
            int heartbeat_ms = 100;
            int election_timeout_ms = 1000; // randomized up to twice this
            // Take a snapshot (and compact the log) every this many entries;
            // 0 disables snapshots
            int snapshot_distance = 1000;
            size_t snapshot_chunk = 1 << 20; // snapshots travel in pieces of this size
            int log_level = 1;               // NuRaft messages printed: 1 fatal ... 6 trace
        };

        // Starts the Raft server. Throws std::runtime_error on failure.
        explicit ReplicatedShard(const Options &options);
        ~ReplicatedShard();

        ReplicatedShard(const ReplicatedShard &) = delete;
        ReplicatedShard &operator=(const ReplicatedShard &) = delete;

        // Stops the Raft server (idempotent); the files stay for a restart
        void Stop();

        // One statement, replicated: committed by a quorum and applied on
        // every replica. Only the leader accepts writes; elsewhere (or if
        // the quorum is lost) the result is a failure saying so.
        ExecutionResult Write(const std::string &sql);

        // A SELECT/EXPLAIN that sees every write committed before it began
        // (linearizable): the leader commits a no-op through the log first,
        // proving it still leads, then reads its own data. No clocks.
        ExecutionResult Read(const std::string &sql);

        bool IsLeader() const;
        int LeaderId() const; // -1 if unknown
        int Id() const { return options_.id; }

        ShardStateMachine &Machine() { return *machine_; }
        nuraft::ptr<nuraft::raft_server> Raft() const { return raft_; }

    private:
        ExecutionResult Replicate(nuraft::ptr<nuraft::buffer> entry);

        Options options_;
        nuraft::ptr<ShardStateMachine> machine_;
        nuraft::ptr<FileStateManager> state_;
        std::unique_ptr<nuraft::raft_launcher> launcher_;
        nuraft::ptr<nuraft::raft_server> raft_;
    };

} // namespace sql::cluster
