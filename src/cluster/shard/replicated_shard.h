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
            // A new replica of an existing group (D8): it starts knowing only
            // itself, at `endpoint`, and never calls an election until the
            // group's leader adds it (AddMember) and catches it up
            bool joining = false;
            std::string endpoint;
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
        //
        // A "not committed" failure is uncertain: a leader change may have
        // cancelled the request after it was replicated, and the new leader
        // may still commit it. Retrying with the same `request_id` is safe:
        // a write that went through returns its first outcome instead of
        // applying again. Without an id, a fresh one is used.
        ExecutionResult Write(const std::string &sql, const std::string &request_id = "");

        // Several statements, applied all or nothing (see
        // ShardStateMachine::EncodeBatch); retried like Write
        ExecutionResult WriteBatch(const std::string &script, const std::string &request_id = "");

        // A step of distributed transaction `txid` (see ShardStateMachine::
        // EncodeTxn); retried like Write
        ExecutionResult Transaction(ShardStateMachine::TxnOp op, const std::string &txid, const std::string &sql,
                                    const std::string &request_id = "");

        // The transaction open on the shard as of now (linearizable, on the
        // leader), "" if none; a failure if this replica cannot tell
        ExecutionResult OpenTransaction();

        // A new random request id
        static std::string NewRequestId();

        // A SELECT/EXPLAIN that sees every write committed before it began
        // (linearizable): the leader commits a no-op through the log first,
        // proving it still leads, then reads its own data. No clocks.
        // Inside transaction `txid` (if not empty) it sees that
        // transaction's uncommitted changes.
        ExecutionResult Read(const std::string &sql, const std::string &txid = "");

        // Membership changes, on the leader, one at a time (NuRaft
        // add_srv / remove_srv). A new member is caught up (from the log or
        // a snapshot) before it counts. Each returns success once the change
        // is in the group's configuration, or a failure starting with
        // "pending" while it is under way (call again later), or "not the
        // leader (leader: N)". Removing the leader hands leadership over
        // first (pending meanwhile).
        ExecutionResult AddMember(int id, const std::string &endpoint);
        ExecutionResult RemoveMember(int id);
        // The group's members as the leader sees them: "1,2,4"
        ExecutionResult Members();

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
